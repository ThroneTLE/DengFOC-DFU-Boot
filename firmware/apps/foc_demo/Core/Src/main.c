/*
 * DengFOC-ODrive single board: FOC demo for a 2804 gimbal motor + AS5600.
 *
 * USB CDC console, one command per line (see `help`). The motor stays off
 * until a motion command; every motion command checks VBUS, arms the DRV8303
 * and, for closed-loop modes, requires `cal` first.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "main.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"
#include "as5600.h"
#include "drv8303.h"
#include "foc.h"
#include "knob.h"

#define TWO_PI        6.28318530718f
#define VBUS_MIN      9.0f
#define VBUS_MAX      16.8f    /* motor is rated 7.4-16 V */
#define TEMP_MAX      80.0f
#define CAL_VOLTAGE   1.5f     /* ~0.6 A into 2.55 ohm */

USBD_HandleTypeDef hUsbDeviceFS;
static ADC_HandleTypeDef hadc1;
static float fet_temp;

/* ------------------------------------------------------------------ output */

static void out(const char *fmt, ...)
{
  char buf[240];
  if (scope_running())
  {
    return;   /* keep the JustFloat stream clean */
  }
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0)
  {
    cdc_write(buf, (uint32_t)((n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1));
  }
}

/* printf without float support: format v with 1..3 decimals into b */
static const char *fs(char *b, float v, int dec)
{
  const char *sign = (v < 0.0f) ? "-" : "";
  float a = fabsf(v);
  if (dec <= 1)
  {
    uint32_t x = (uint32_t)(a * 10.0f + 0.5f);
    sprintf(b, "%s%lu.%01lu", sign, x / 10U, x % 10U);
  }
  else if (dec == 2)
  {
    uint32_t x = (uint32_t)(a * 100.0f + 0.5f);
    sprintf(b, "%s%lu.%02lu", sign, x / 100U, x % 100U);
  }
  else
  {
    uint32_t x = (uint32_t)(a * 1000.0f + 0.5f);
    sprintf(b, "%s%lu.%03lu", sign, x / 1000U, x % 1000U);
  }
  return b;
}

/* ---------------------------------------------------------------- hardware */

static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  osc.HSEState = RCC_HSE_ON;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM = 8;
  osc.PLL.PLLN = 336;
  osc.PLL.PLLP = RCC_PLLP_DIV2;
  osc.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK)
  {
    Error_Handler();
  }

  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV4;
  clk.APB2CLKDivider = RCC_HCLK_DIV2;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* Drive every power-stage input to its off state before anything else runs. */
static void power_stage_safe(void)
{
  GPIO_InitTypeDef g = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 |
                    GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);  /* DRV8303 nSCS */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_SET);   /* AS5047P CSn */

  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  g.Pin = GPIO_PIN_3 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 |
          GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
  HAL_GPIO_Init(GPIOB, &g);
  g.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10;
  HAL_GPIO_Init(GPIOA, &g);
  g.Pin = GPIO_PIN_13;
  HAL_GPIO_Init(GPIOC, &g);

  /* nFAULT (PD2) input; external pull-up on the board */
  g.Mode = GPIO_MODE_INPUT;
  g.Pin = GPIO_PIN_2;
  HAL_GPIO_Init(GPIOD, &g);
}

static void adc_init(void)
{
  GPIO_InitTypeDef g = {0};

  g.Mode = GPIO_MODE_ANALOG;
  g.Pull = GPIO_NOPULL;
  g.Pin = GPIO_PIN_6;                 /* VBUS_S */
  HAL_GPIO_Init(GPIOA, &g);
  g.Pin = GPIO_PIN_5;                 /* M0_TEMP */
  HAL_GPIO_Init(GPIOC, &g);

  __HAL_RCC_ADC1_CLK_ENABLE();
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }
}

static float adc_volts(uint32_t channel)
{
  ADC_ChannelConfTypeDef c = {0};
  uint32_t sum = 0;
  c.Channel = channel;
  c.Rank = 1;
  c.SamplingTime = ADC_SAMPLETIME_144CYCLES;
  (void)HAL_ADC_ConfigChannel(&hadc1, &c);
  for (int i = 0; i < 8; i++)
  {
    HAL_ADC_Start(&hadc1);
    (void)HAL_ADC_PollForConversion(&hadc1, 5);
    sum += HAL_ADC_GetValue(&hadc1);
  }
  HAL_ADC_Stop(&hadc1);
  return (float)sum / 8.0f * (3.3f / 4095.0f);
}

/* Pack scope frames as VOFA+ JustFloat (floats + 00 00 80 7F) into USB packets. */
static void scope_drain(void)
{
  static const uint8_t tail[4] = {0x00, 0x00, 0x80, 0x7F};
  static uint8_t pkt[CDC_TX_MAX];
  static uint32_t pkt_len;
  float frame[SCOPE_CH];

  if (!scope_running())
  {
    pkt_len = 0;
    return;
  }
  for (int k = 0; k < 4; k++)
  {
    if (pkt_len == 0U)
    {
      while (pkt_len + sizeof(frame) + sizeof(tail) <= sizeof(pkt) && scope_pop(frame))
      {
        memcpy(pkt + pkt_len, frame, sizeof(frame));
        pkt_len += sizeof(frame);
        memcpy(pkt + pkt_len, tail, sizeof(tail));
        pkt_len += sizeof(tail);
      }
      if (pkt_len == 0U)
      {
        return;
      }
    }
    if (!cdc_try_write(pkt, pkt_len))
    {
      return;   /* endpoint busy: keep the packet for the next call */
    }
    pkt_len = 0;
  }
}

/* VBUS, FET temperature and their trips; call often from the main loop. */
static void housekeeping(void)
{
  static uint32_t last;
  scope_drain();
  if ((HAL_GetTick() - last) < 20U)
  {
    return;
  }
  last = HAL_GetTick();

  foc.vbus = adc_volts(ADC_CHANNEL_6) * 11.0f;
  float x = adc_volts(ADC_CHANNEL_15) / 3.3f;
  fet_temp = ((363.93910201f * x - 462.15369634f) * x + 307.55129571f) * x - 27.72569531f;
  foc.fet_temp = fet_temp;

  if (foc.armed && (foc.vbus < VBUS_MIN || foc.vbus > VBUS_MAX))
  {
    foc_trip(FAULT_VBUS);
  }
  if (foc.armed && fet_temp > TEMP_MAX)
  {
    foc_trip(FAULT_TEMP);
  }
}

static void delay_hk(uint32_t ms)
{
  uint32_t t0 = HAL_GetTick();
  while ((HAL_GetTick() - t0) < ms)
  {
    housekeeping();
  }
}

/* ------------------------------------------------------------ arm / disarm */

static void disarm(void)
{
  foc.mode = MODE_OFF;
  foc_output_enable(0);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_RESET);
  foc.armed = 0;
}

static int arm(void)
{
  char b1[16];
  if (foc.armed)
  {
    return 1;
  }
  if (foc.fault != FAULT_NONE)
  {
    out("fault active (%s): send `clear` first\r\n", foc_fault_name(foc.fault));
    return 0;
  }
  if (foc.vbus < VBUS_MIN || foc.vbus > VBUS_MAX)
  {
    out("VBUS %s V outside %d-%d V: connect the 12 V battery to XT30\r\n",
        fs(b1, foc.vbus, 2), (int)VBUS_MIN, (int)VBUS_MAX);
    return 0;
  }
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET);   /* EN_GATE */
  HAL_Delay(30);
  if (!drv_configure())
  {
    uint16_t s0 = 0;
    uint16_t s1 = 0;
    (void)drv_read(0x00, &s0);
    (void)drv_read(0x01, &s1);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_RESET);
    out("DRV8303 config readback failed (status1=0x%03X status2=0x%03X)\r\n", s0, s1);
    return 0;
  }
  HAL_Delay(5);
  if (HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_2) == GPIO_PIN_RESET)
  {
    uint16_t s0 = 0;
    (void)drv_read(0x00, &s0);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_RESET);
    out("DRV8303 nFAULT low after enable (status1=0x%03X)\r\n", s0);
    return 0;
  }
  foc.armed = 1;
  out("armed: DRV8303 6-PWM, OC latch, VBUS %s V\r\n", fs(b1, foc.vbus, 2));
  return 1;
}

static int start_mode(FocMode m, float target, int need_cal)
{
  if (need_cal && !foc.calibrated)
  {
    out("run `cal` first\r\n");
    return 0;
  }
  if (!arm())
  {
    return 0;
  }
  foc.target = target;
  if (foc.mode != m)
  {
    foc.mode = m;
  }
  if ((TIM1->BDTR & TIM_BDTR_MOE) == 0U)
  {
    foc_output_enable(1);
  }
  return 1;
}

/* ------------------------------------------------------------- calibration */

static void calibrate(void)
{
  char b1[16], b2[16], b3[16];
  if (!arm())
  {
    return;
  }
  float ua = (foc.u_limit < CAL_VOLTAGE) ? foc.u_limit : CAL_VOLTAGE;
  if (ua < 0.5f)
  {
    out("u limit too low for calibration\r\n");
    return;
  }

  foc.calibrated = 0;
  foc.dir = 1;
  foc.zero_offset = 0.0f;
  foc.vec_ud = ua;
  foc.vec_angle = 0.0f;
  foc.mode = MODE_VECTOR;
  foc_output_enable(1);
  out("cal: holding vector at %s V\r\n", fs(b1, ua, 2));
  delay_hk(500);

  float a0 = foc.shaft_angle;
  for (int i = 0; i <= 200 && foc.fault == FAULT_NONE; i++)
  {
    foc.vec_angle = TWO_PI * (float)i / 200.0f;
    delay_hk(5);
  }
  delay_hk(200);
  float a1 = foc.shaft_angle;
  for (int i = 200; i >= 0 && foc.fault == FAULT_NONE; i--)
  {
    foc.vec_angle = TWO_PI * (float)i / 200.0f;
    delay_hk(5);
  }
  delay_hk(200);
  float a2 = foc.shaft_angle;

  if (foc.fault != FAULT_NONE)
  {
    out("cal aborted: %s\r\n", foc_fault_name(foc.fault));
    return;
  }

  float moved = a1 - a0;
  float expect = TWO_PI / (float)foc.pole_pairs;
  out("cal: one electrical turn moved the shaft %s deg (expected %s deg), return error %s deg\r\n",
      fs(b1, moved * 57.2958f, 1), fs(b2, expect * 57.2958f, 1), fs(b3, (a2 - a0) * 57.2958f, 1));

  if (fabsf(moved) < 0.3f * expect)
  {
    out("cal failed: rotor did not follow. Check motor wires, battery and that the encoder sees the magnet\r\n");
    disarm();
    return;
  }
  float pp_est = TWO_PI / fabsf(moved);
  if (fabsf(pp_est - (float)foc.pole_pairs) > 0.25f * (float)foc.pole_pairs)
  {
    out("cal failed: measured %s pole pairs, configured %d. Set `pp <n>` and retry\r\n",
        fs(b1, pp_est, 2), foc.pole_pairs);
    disarm();
    return;
  }

  foc.dir = (moved > 0.0f) ? 1 : -1;
  foc.vec_angle = 0.0f;
  delay_hk(700);
  foc.zero_offset = fmodf((float)foc.dir * (float)foc.pole_pairs * foc.mech_angle, TWO_PI);
  if (foc.zero_offset < 0.0f)
  {
    foc.zero_offset += TWO_PI;
  }
  foc.mode = MODE_OFF;
  foc_output_enable(0);
  delay_hk(5);
  foc.pos_offset = foc.shaft_angle;
  foc.calibrated = 1;
  out("cal ok: dir=%+d  pole pairs ~%s  zero offset %s rad. Position zeroed here.\r\n",
      foc.dir, fs(b1, pp_est, 2), fs(b2, foc.zero_offset, 3));
}

/* ---------------------------------------------------------------- commands */

/* Current position becomes 0 deg; in KNOB mode it also becomes detent 0. */
static void set_zero(void)
{
  foc.pos_offset = foc.shaft_angle;
  if (foc.mode == MODE_ANGLE)
  {
    foc.target = foc.pos_offset;
  }
  else if (foc.mode == MODE_KNOB)
  {
    NVIC_DisableIRQ(TIM7_IRQn);
    knob_start(knob_preset(), foc.shaft_angle, foc.velocity);
    NVIC_EnableIRQ(TIM7_IRQn);
  }
}

/* `knob <n> [deg]`: the optional offset starts the detent centre that far
 * from the shaft, so the knob springs back on its own (step test). */
static void cmd_knob(int n, float offset_deg)
{
  if (n <= 0)
  {
    disarm();
    return;
  }
  if (n > KNOB_PRESETS)
  {
    for (int i = 1; i <= KNOB_PRESETS; i++)
    {
      out("%2d %s\r\n", i, knob_name(i));
    }
    return;
  }
  if (!foc.calibrated)
  {
    out("run `cal` first\r\n");
    return;
  }
  /* set the preset before the interrupt can run it */
  NVIC_DisableIRQ(TIM7_IRQn);
  knob_start(n, foc.shaft_angle + offset_deg * (TWO_PI / 360.0f), foc.velocity);
  NVIC_EnableIRQ(TIM7_IRQn);
  if (start_mode(MODE_KNOB, 0.0f, 1))
  {
    out("knob: %s\r\n", knob_name(n));
  }
}

static void print_status(void)
{
  char a[16], v[16], t[16], u[16], vb[16], tp[16];
  /* target unit follows the mode: deg for ANGLE, rad/s for VELOCITY, V for TORQUE */
  float tgt = (foc.mode == MODE_ANGLE) ? (foc.target - foc.pos_offset) * 57.2958f : foc.target;
  out("%-8s cal=%d pos=%s deg vel=%s rad/s tgt=%s uq=%s V vbus=%s V fet=%s C raw=%4u encerr=%lu isr=%luus armed=%d fault=%s\r\n",
      foc_mode_name(foc.mode), foc.calibrated,
      fs(a, (foc.shaft_angle - foc.pos_offset) * 57.2958f, 1), fs(v, foc.velocity, 2),
      fs(t, tgt, 2), fs(u, foc.uq, 2), fs(vb, foc.vbus, 2), fs(tp, fet_temp, 1),
      foc.raw, foc.enc_errors, foc.isr_us, foc.armed, foc_fault_name(foc.fault));
  if (foc.mode == MODE_KNOB)
  {
    out("knob: %s  detent %d  strength %s\r\n", knob_name(knob_preset()), knob_position(),
        fs(a, knob_strength, 2));
  }
}

static void cmd_enc(void)
{
  char b1[16];
  if (!as5600_found())
  {
    out("AS5600 not found on Pin1..Pin7. Check VCC->'3', GND->'G', SCL/SDA on the header, then `detect`\r\n");
    return;
  }
  out("AS5600: SCL=%s SDA=%s  read=%lu us (%s)  control loop %lu Hz\r\n",
      as5600_scl_name(), as5600_sda_name(), as5600_read_time_us(),
      as5600_fast_mode() ? "pointer-less" : "full", foc_rate());
  out("raw=%u  angle=%s deg  read errors=%lu\r\n", foc.raw,
      fs(b1, (float)foc.raw * 360.0f / 4096.0f, 1), foc.enc_errors);
  if (foc.mode != MODE_OFF)
  {
    return;
  }
  uint8_t st = 0;
  uint8_t agc = 0;
  uint16_t mag = 0;
  uint16_t conf = 0;
  NVIC_DisableIRQ(TIM7_IRQn);
  int ok = as5600_read_diag(&st, &agc, &mag, &conf);
  NVIC_EnableIRQ(TIM7_IRQn);
  if (ok)
  {
    out("STATUS=0x%02X (magnet %s%s%s)  AGC=%u  MAGNITUDE=%u  CONF=0x%04X\r\n", st,
        (st & 0x20U) ? "detected" : "NOT detected",
        (st & 0x10U) ? ", too weak" : "", (st & 0x08U) ? ", too strong" : "", agc, mag, conf);
  }
}

static void cmd_mon(float seconds)
{
  uint32_t t0 = HAL_GetTick();
  uint32_t next = t0;
  while ((HAL_GetTick() - t0) < (uint32_t)(seconds * 1000.0f))
  {
    housekeeping();
    if (cdc_getc() >= 0)
    {
      break;
    }
    if ((int32_t)(HAL_GetTick() - next) >= 0)
    {
      next += 100U;
      print_status();
    }
  }
}

static void cmd_help(void)
{
  out("status/monitor: s | mon [sec] | enc | detect\r\n");
  out("motion:  open <rad/s> | cal | tq <V> | vel <rad/s> | pos <deg> | zero | stop\r\n");
  out("tuning:  ulim <V> (now %d.%d) | vlim <rad/s> | vpi <P> <I> | vp <P> | vi <I> | ap <P> | pp <n>\r\n",
      (int)foc.u_limit, (int)(foc.u_limit * 10.0f) % 10);
  out("knob:    knob <1..%d> [deg] (`knob` lists them) | knob 0 | kstr <x> (feel strength, now %d.%02d)"
      " | kcomp <c0> <c1>\r\n", KNOB_PRESETS, (int)knob_strength, (int)(knob_strength * 100.0f) % 100);
  out("scope:   scope [hz] (VOFA+ JustFloat, text paused) | scope off\r\n");
  out("other:   clear | act <0 stop|1 cal|2 clear|3 zero> | dfu\r\n");
}

static void handle_line(char *line)
{
  char *cmd = strtok(line, " \t");
  char *a1 = strtok(NULL, " \t");
  char *a2 = strtok(NULL, " \t");
  float f1 = a1 ? strtof(a1, NULL) : 0.0f;
  char b1[16], b2[16];

  if (cmd == NULL)
  {
    out("> ");
    return;
  }
  if (strcmp(cmd, "help") == 0)             cmd_help();
  else if (strcmp(cmd, "s") == 0)           print_status();
  else if (strcmp(cmd, "mon") == 0)         cmd_mon(a1 ? f1 : 5.0f);
  else if (strcmp(cmd, "enc") == 0)         cmd_enc();
  else if (strcmp(cmd, "detect") == 0)
  {
    if (foc.mode != MODE_OFF)
    {
      out("stop the motor first\r\n");
    }
    else
    {
      NVIC_DisableIRQ(TIM7_IRQn);
      (void)as5600_detect();
      NVIC_EnableIRQ(TIM7_IRQn);
      cmd_enc();
    }
  }
  else if (strcmp(cmd, "cal") == 0)         calibrate();
  else if (strcmp(cmd, "open") == 0)
  {
    foc.ol_speed = a1 ? f1 : 3.0f;
    if (start_mode(MODE_OPENLOOP, 0.0f, 0))
    {
      out("open loop %s rad/s at %s V\r\n", fs(b1, foc.ol_speed, 2), fs(b2, foc.u_limit, 2));
    }
  }
  else if (strcmp(cmd, "tq") == 0)          (void)start_mode(MODE_TORQUE, f1, 1);
  else if (strcmp(cmd, "vel") == 0)         (void)start_mode(MODE_VELOCITY, f1, 1);
  else if (strcmp(cmd, "pos") == 0)         (void)start_mode(MODE_ANGLE, foc.pos_offset + f1 * (TWO_PI / 360.0f), 1);
  else if (strcmp(cmd, "zero") == 0)        set_zero();
  else if (strcmp(cmd, "knob") == 0)        cmd_knob(a1 ? (int)f1 : KNOB_PRESETS + 1, a2 ? strtof(a2, NULL) : 0.0f);
  else if (strcmp(cmd, "kstr") == 0 && a1)
  {
    knob_strength = (f1 < 0.0f) ? 0.0f : (f1 > 3.0f) ? 3.0f : f1;
    out("knob strength %s\r\n", fs(b1, knob_strength, 2));
  }
  else if (strcmp(cmd, "kcomp") == 0)
  {
    /* plant compensation for free-spinning presets: Uq = c1*w + c0(dir)*sign(w)
     * `kcomp <c0+> <c0-> <c1>` from tools/ident_friction.py */
    char *a3 = strtok(NULL, " \t");
    if (a1 && a2 && a3)
    {
      float c0n = strtof(a2, NULL);
      float c1 = strtof(a3, NULL);
      knob_comp_c0p = (f1 < 0.0f) ? 0.0f : (f1 > 0.3f) ? 0.3f : f1;
      knob_comp_c0n = (c0n < 0.0f) ? 0.0f : (c0n > 0.3f) ? 0.3f : c0n;
      knob_comp_c1 = (c1 < 0.0f) ? 0.0f : (c1 > 0.06f) ? 0.06f : c1;
    }
    char b3[16];
    out("kcomp c0+=%s V c0-=%s V c1=%s mV/(rad/s)\r\n", fs(b1, knob_comp_c0p, 3), fs(b2, knob_comp_c0n, 3),
        fs(b3, knob_comp_c1 * 1000.0f, 2));
  }
  else if (strcmp(cmd, "scope") == 0)
  {
    /* `scope off` or `scope 0` stops; `scope`, `scope 1` = full rate; `scope <hz>` */
    if (a1 && (strcmp(a1, "off") == 0 || f1 == 0.0f))
    {
      scope_stop();
      HAL_Delay(20);
      out("scope off, frames dropped: %lu\r\n", scope_dropped());
    }
    else
    {
      uint32_t hz = (a1 && f1 > 1.0f) ? (uint32_t)f1 : foc_rate();
      uint32_t decim = (hz == 0U || hz >= foc_rate()) ? 1U : foc_rate() / hz;
      out("scope: VOFA+ JustFloat, %d ch @ %lu Hz: tgt pos_deg vel vel_ref uq theta_e vbus isr_us"
          " mode fault fet_c cal knob_pos knob_mode\r\n", SCOPE_CH, foc_rate() / decim);
      out("text output paused; send `scope off` to resume\r\n");
      HAL_Delay(20);
      scope_start(decim);
      return;
    }
  }
  else if (strcmp(cmd, "act") == 0 && a1)
  {
    /* one command for a GUI button bar: 0 stop, 1 cal, 2 clear, 3 zero */
    switch ((int)f1)
    {
      case 0: disarm(); break;
      case 1: calibrate(); break;
      case 2: disarm(); foc.fault = FAULT_NONE; break;
      case 3: set_zero(); break;
      default: out("act: 0 stop, 1 cal, 2 clear, 3 zero\r\n"); break;
    }
  }
  else if (strcmp(cmd, "stop") == 0)        disarm();
  else if (strcmp(cmd, "clear") == 0)
  {
    disarm();
    foc.fault = FAULT_NONE;
  }
  else if (strcmp(cmd, "ulim") == 0 && a1)
  {
    foc.u_limit = (f1 < 0.0f) ? 0.0f : (f1 > 5.0f) ? 5.0f : f1;
    out("u limit %s V\r\n", fs(b1, foc.u_limit, 2));
  }
  else if (strcmp(cmd, "vlim") == 0 && a1)  foc.vel_limit = fabsf(f1);
  else if (strcmp(cmd, "vpi") == 0 && a1 && a2)
  {
    foc.vel_p = f1;
    foc.vel_i = strtof(a2, NULL);
  }
  else if (strcmp(cmd, "vp") == 0 && a1)    foc.vel_p = f1;
  else if (strcmp(cmd, "vi") == 0 && a1)    foc.vel_i = f1;
  else if (strcmp(cmd, "ap") == 0 && a1)    foc.ang_p = f1;
  else if (strcmp(cmd, "pp") == 0 && a1)
  {
    if (foc.mode != MODE_OFF)
    {
      out("stop the motor first\r\n");
    }
    else
    {
      foc.pole_pairs = (int)f1;
      foc.calibrated = 0;
    }
  }
  else if (strcmp(cmd, "dfu") == 0)
  {
    disarm();
    out("rebooting into DFU bootloader\r\n");
    HAL_Delay(50);
    *(volatile uint32_t *)BOOT_MAGIC_ADDR = BOOT_MAGIC_DFU_REQ;
    NVIC_SystemReset();
  }
  else
  {
    out("unknown command, try `help`\r\n");
  }
  out("> ");
}

int main(void)
{
  power_stage_safe();

  HAL_Init();
  SystemClock_Config();

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  adc_init();
  drv_spi_init();

  if (USBD_Init(&hUsbDeviceFS, &FS_Desc, DEVICE_FS) != USBD_OK ||
      USBD_RegisterClass(&hUsbDeviceFS, &USBD_CDC) != USBD_OK ||
      USBD_CDC_RegisterInterface(&hUsbDeviceFS, &USBD_Interface_fops_FS) != USBD_OK ||
      USBD_Start(&hUsbDeviceFS) != USBD_OK)
  {
    Error_Handler();
  }

  (void)as5600_detect();
  /* 2 kHz if a read leaves enough headroom, else 1 kHz */
  foc_init(as5600_read_time_us() < 280U ? 2000U : 1000U);

  char line[48];
  size_t len = 0;
  while (1)
  {
    housekeeping();
    int c = cdc_getc();
    if (c < 0)
    {
      continue;
    }
    if (c == '\r' || c == '\n')
    {
      line[len] = '\0';
      handle_line(line);
      len = 0;
    }
    else if (len < sizeof(line) - 1U)
    {
      line[len++] = (char)c;
    }
  }
}

void Error_Handler(void)
{
  TIM1->BDTR &= ~TIM_BDTR_MOE;
  GPIOB->BSRR = (uint32_t)(GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12) << 16;
  __disable_irq();
  while (1)
  {
  }
}
