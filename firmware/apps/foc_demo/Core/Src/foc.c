/*
 * Voltage-mode FOC for a high-resistance gimbal motor (no current loop).
 * Runs in the TIM7 interrupt: read AS5600, update angle/velocity, run the
 * selected loop, write TIM1 duties (centre-aligned 24 kHz, 6-PWM to DRV8303).
 */
#include <math.h>
#include <string.h>
#include "foc.h"
#include "as5600.h"
#include "knob.h"
#include "main.h"

#define TWO_PI      6.28318530718f
#define SQRT3_2     0.86602540378f
#define PWM_ARR     3500U          /* 168 MHz / (2 * 3500) = 24 kHz */
#define DUTY_MAX    0.95f          /* keep bootstrap capacitors charged */
#define U_HARD_MAX  5.0f           /* 5 V / 2.55 ohm ~ 2 A: motor maximum */

Foc foc;

static uint32_t rate;
static float dt;
static float prev_mech;
static float full_turns_angle;     /* rad, multi-turn, sensor direction */
static float vel_integral;
static float ol_angle;
static uint32_t enc_err_run;
#define ENC_HYST 2                 /* counts */
static int32_t enc_held;           /* encoder count after hysteresis */
static uint32_t armed_ticks;

#define SCOPE_DEPTH 256U
static float scope_buf[SCOPE_DEPTH][SCOPE_CH];
static volatile uint32_t scope_head;   /* written by the ISR */
static volatile uint32_t scope_tail;   /* written by the main loop */
static volatile uint32_t scope_decim;  /* 0 = off */
static uint32_t scope_tick;
static volatile uint32_t scope_drops;

void scope_start(uint32_t decim)
{
  scope_decim = 0;
  scope_tail = scope_head;
  scope_drops = 0;
  scope_tick = 0;
  scope_decim = (decim == 0U) ? 1U : decim;
}

void scope_stop(void)
{
  scope_decim = 0;
}

int scope_running(void)
{
  return scope_decim != 0U;
}

uint32_t scope_dropped(void)
{
  return scope_drops;
}

int scope_pop(float *frame)
{
  uint32_t t = scope_tail;
  if (t == scope_head)
  {
    return 0;
  }
  memcpy(frame, scope_buf[t % SCOPE_DEPTH], sizeof(scope_buf[0]));
  scope_tail = t + 1U;
  return 1;
}

static void scope_push(void)
{
  if (scope_decim == 0U || ++scope_tick < scope_decim)
  {
    return;
  }
  scope_tick = 0;
  uint32_t h = scope_head;
  if ((h - scope_tail) >= SCOPE_DEPTH)
  {
    scope_drops++;
    return;
  }
  float *f = scope_buf[h % SCOPE_DEPTH];
  f[0] = (foc.mode == MODE_ANGLE) ? (foc.target - foc.pos_offset) * 57.2958f :
         (foc.mode == MODE_KNOB) ? (knob_center() - foc.pos_offset) * 57.2958f : foc.target;
  f[1] = (foc.shaft_angle - foc.pos_offset) * 57.2958f;
  f[2] = foc.velocity;
  f[3] = (foc.mode == MODE_KNOB) ? knob_wheel_velocity() : foc.vel_ref;
  f[4] = foc.uq;
  f[5] = foc.elec_angle;
  f[6] = foc.vbus;
  f[7] = (float)foc.isr_us;
  f[8] = (float)foc.mode;
  f[9] = (float)foc.fault;
  f[10] = foc.fet_temp;
  f[11] = (float)foc.calibrated;
  f[12] = (float)knob_position();
  f[13] = (foc.mode == MODE_KNOB) ? (float)knob_preset() : 0.0f;
  scope_head = h + 1U;
}

static float wrap_2pi(float a)
{
  a = fmodf(a, TWO_PI);
  return (a < 0.0f) ? a + TWO_PI : a;
}

static float clampf(float v, float lo, float hi)
{
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

void foc_output_enable(int on)
{
  if (on)
  {
    TIM1->CCR1 = PWM_ARR / 2U;
    TIM1->CCR2 = PWM_ARR / 2U;
    TIM1->CCR3 = PWM_ARR / 2U;
    TIM1->BDTR |= TIM_BDTR_MOE;
  }
  else
  {
    TIM1->BDTR &= ~TIM_BDTR_MOE;   /* OSSI: all six outputs go to idle low */
  }
}

void foc_trip(FocFault f)
{
  foc_output_enable(0);
  foc.mode = MODE_OFF;
  GPIOB->BSRR = (uint32_t)GPIO_PIN_12 << 16; /* EN_GATE low */
  foc.armed = 0;
  if (foc.fault == FAULT_NONE)
  {
    foc.fault = f;
  }
}

/* Phase voltages from (ud, uq) at electrical angle theta, min/max injection. */
static void set_voltage(float ud, float uq, float theta)
{
  float s = sinf(theta);
  float c = cosf(theta);
  float ua = ud * c - uq * s;   /* alpha */
  float ub = ud * s + uq * c;   /* beta  */

  float va = ua;
  float vb = -0.5f * ua + SQRT3_2 * ub;
  float vc = -0.5f * ua - SQRT3_2 * ub;
  float vmax = fmaxf(va, fmaxf(vb, vc));
  float vmin = fminf(va, fminf(vb, vc));
  float mid = 0.5f * (vmax + vmin);

  float vbus = (foc.vbus > 5.0f) ? foc.vbus : 12.0f;
  float da = clampf(0.5f + (va - mid) / vbus, 1.0f - DUTY_MAX, DUTY_MAX);
  float db = clampf(0.5f + (vb - mid) / vbus, 1.0f - DUTY_MAX, DUTY_MAX);
  float dc = clampf(0.5f + (vc - mid) / vbus, 1.0f - DUTY_MAX, DUTY_MAX);

  TIM1->CCR1 = (uint32_t)(da * PWM_ARR);
  TIM1->CCR2 = (uint32_t)(db * PWM_ARR);
  TIM1->CCR3 = (uint32_t)(dc * PWM_ARR);
  foc.uq = uq;
}

static void pwm_init(void)
{
  GPIO_InitTypeDef g = {0};

  __HAL_RCC_TIM1_CLK_ENABLE();
  TIM1->CR1 = 0;
  TIM1->CR2 = 0;                       /* OISx = OISxN = 0: idle low */
  TIM1->PSC = 0;
  TIM1->ARR = PWM_ARR;
  TIM1->RCR = 0;
  TIM1->CCMR1 = (6U << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE |
                (6U << TIM_CCMR1_OC2M_Pos) | TIM_CCMR1_OC2PE;   /* PWM mode 1 */
  TIM1->CCMR2 = (6U << TIM_CCMR2_OC3M_Pos) | TIM_CCMR2_OC3PE;
  TIM1->CCR1 = PWM_ARR / 2U;
  TIM1->CCR2 = PWM_ARR / 2U;
  TIM1->CCR3 = PWM_ARR / 2U;
  TIM1->BDTR = TIM_BDTR_OSSR | TIM_BDTR_OSSI | 20U; /* 20 clk dead time = 119 ns, MOE = 0 */
  TIM1->CCER = TIM_CCER_CC1E | TIM_CCER_CC1NE | TIM_CCER_CC2E | TIM_CCER_CC2NE |
               TIM_CCER_CC3E | TIM_CCER_CC3NE;      /* all active high */
  TIM1->CR1 = TIM_CR1_CMS_0 | TIM_CR1_ARPE;         /* centre-aligned mode 1 */
  TIM1->EGR = TIM_EGR_UG;
  TIM1->CR1 |= TIM_CR1_CEN;

  /* Hand the gate pins from GPIO-low to the timer, which holds them low (MOE = 0). */
  g.Mode = GPIO_MODE_AF_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_HIGH;
  g.Alternate = GPIO_AF1_TIM1;
  g.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10;
  HAL_GPIO_Init(GPIOA, &g);
  g.Pin = GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
  HAL_GPIO_Init(GPIOB, &g);
}

void foc_init(uint32_t rate_hz)
{
  rate = rate_hz;
  dt = 1.0f / (float)rate_hz;

  foc.vbus = 0.0f;
  foc.u_limit = 2.0f;
  foc.vel_limit = 30.0f;
  foc.vel_p = 0.1f;
  foc.vel_i = 2.0f;
  foc.ang_p = 20.0f;
  foc.vel_tf = 0.01f;
  foc.pole_pairs = 7;
  foc.dir = 1;
  foc.mode = MODE_OFF;

  pwm_init();

  /* TIM7: control interrupt */
  __HAL_RCC_TIM7_CLK_ENABLE();
  TIM7->PSC = 84U - 1U;                  /* 84 MHz -> 1 MHz */
  TIM7->ARR = (1000000U / rate_hz) - 1U;
  TIM7->DIER = TIM_DIER_UIE;
  TIM7->EGR = TIM_EGR_UG;
  TIM7->SR = 0;
  HAL_NVIC_SetPriority(TIM7_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(TIM7_IRQn);
  TIM7->CR1 = TIM_CR1_CEN;
}

uint32_t foc_rate(void)
{
  return rate;
}

void foc_isr(void)
{
  uint32_t t0 = DWT->CYCCNT;
  uint16_t raw;

  foc.isr_count++;

  /* DRV8303 fault (ignore the first 50 ms after arming) */
  if (foc.armed)
  {
    if (armed_ticks < rate / 20U)
    {
      armed_ticks++;
    }
    else if ((GPIOD->IDR & GPIO_PIN_2) == 0U)
    {
      foc_trip(FAULT_NFAULT);
    }
  }
  else
  {
    armed_ticks = 0;
  }

  int ok = as5600_read_raw(&raw);
  if (ok)
  {
    /* hysteresis: the AS5600 flickers between neighbouring counts at rest,
     * which the springs would turn into an audible torque hiss */
    int32_t dr = (((int32_t)raw - enc_held + 6144) % 4096) - 2048;
    if (foc.isr_count == 1U)
    {
      enc_held = raw;
    }
    else if (dr > ENC_HYST)
    {
      enc_held = ((int32_t)raw - ENC_HYST + 4096) % 4096;
    }
    else if (dr < -ENC_HYST)
    {
      enc_held = ((int32_t)raw + ENC_HYST) % 4096;
    }
  }
  float mech = ok ? (float)enc_held * (TWO_PI / 4096.0f) : prev_mech;
  float d = mech - prev_mech;
  if (d > 3.14159265f)
  {
    d -= TWO_PI;
  }
  else if (d < -3.14159265f)
  {
    d += TWO_PI;
  }
  /* > 0.5 rad in one tick is far beyond this motor: treat as a bad read */
  if (ok && foc.isr_count > 1U && fabsf(d) > 0.5f)
  {
    ok = 0;
    d = 0.0f;
  }
  if (!ok)
  {
    foc.enc_errors++;
    if (++enc_err_run > 20U && foc.mode != MODE_OFF && foc.mode != MODE_OPENLOOP)
    {
      foc_trip(FAULT_ENCODER);
    }
  }
  else
  {
    enc_err_run = 0;
    prev_mech = mech;
    full_turns_angle += d;
    foc.raw = raw;
  }

  float dirf = (float)foc.dir;
  float vel_raw = dirf * d / dt;
  float a = dt / (foc.vel_tf + dt);
  foc.velocity += a * (vel_raw - foc.velocity);
  foc.mech_angle = prev_mech;
  foc.shaft_angle = dirf * full_turns_angle;
  float theta = wrap_2pi(dirf * (float)foc.pole_pairs * prev_mech - foc.zero_offset);
  foc.elec_angle = theta;

  float ulim = clampf(foc.u_limit, 0.0f, U_HARD_MAX);
  float uq = 0.0f;

  switch (foc.mode)
  {
    case MODE_VECTOR:
      set_voltage(clampf(foc.vec_ud, -ulim, ulim), 0.0f, foc.vec_angle);
      break;

    case MODE_OPENLOOP:
      ol_angle = wrap_2pi(ol_angle + (float)foc.pole_pairs * foc.ol_speed * dt);
      set_voltage(0.0f, ulim, ol_angle);
      break;

    case MODE_TORQUE:
      set_voltage(0.0f, clampf(foc.target, -ulim, ulim), theta);
      break;

    case MODE_VELOCITY:
    case MODE_ANGLE:
    {
      float vref;
      if (foc.mode == MODE_ANGLE)
      {
        vref = foc.ang_p * (foc.target - foc.shaft_angle);
      }
      else
      {
        vref = foc.target;
      }
      vref = clampf(vref, -foc.vel_limit, foc.vel_limit);
      foc.vel_ref = vref;
      float err = vref - foc.velocity;
      vel_integral = clampf(vel_integral + foc.vel_i * err * dt, -ulim, ulim);
      uq = clampf(foc.vel_p * err + vel_integral, -ulim, ulim);
      set_voltage(0.0f, uq, theta);
      break;
    }

    case MODE_KNOB:
      set_voltage(0.0f, knob_tick(foc.shaft_angle, foc.velocity, dt, ulim), theta);
      break;

    case MODE_OFF:
    default:
      vel_integral = 0.0f;
      foc.uq = 0.0f;
      break;
  }
  if (foc.mode != MODE_VELOCITY && foc.mode != MODE_ANGLE)
  {
    foc.vel_ref = 0.0f;
  }

  foc.isr_us = (DWT->CYCCNT - t0) / (SystemCoreClock / 1000000U);
  scope_push();
}

const char *foc_mode_name(FocMode m)
{
  static const char *n[] = {"OFF", "VECTOR", "OPENLOOP", "TORQUE", "VELOCITY", "ANGLE", "KNOB"};
  return ((unsigned)m < 7U) ? n[m] : "?";
}

const char *foc_fault_name(FocFault f)
{
  static const char *n[] = {"none", "DRV nFAULT", "encoder", "VBUS out of range",
                            "over temperature", "DRV config"};
  return ((unsigned)f < 6U) ? n[f] : "?";
}
