/*
 * Haptic knob engine for the 2804 gimbal motor (voltage mode: torque ~ Uq).
 * All torques are in volts of Uq; 1 V ~ 14 mN*m at stall. Every preset is a
 * combination of the building blocks below; the table lists where each
 * preset comes from (experiments/2026-10-20_力反馈旋钮 has the survey).
 *
 * Positions and detents (SmartKnob): a spring pulls toward the current detent
 * centre. Once the angle is `snap` x width away, the centre jumps to the
 * neighbouring position, the spring force flips and the knob falls into the
 * next notch. `bias` moves the snap points so steps toward position 0 are
 * easier than steps away from it. `magnets` limits the spring to a few
 * positions, `zone` to a band around each centre (catch-zone notches). A small
 * dead zone around each centre keeps sensor noise out, and while the knob
 * rests the centre creeps to where it stopped (idle re-centre), so the spring
 * does not keep pushing against cogging and amplifying encoder noise.
 * Endstops: past `min`/`max` a stiff spring with damping pushes back.
 * One-way ratchet: the current position is an endstop in the blocked direction.
 * Staged spring (Jesse Schoch): return-to-centre whose torque limit steps up
 * every `width` degrees, so each zone boundary is felt as a click.
 * Texture: a periodic torque of period `width` (sine, square, triangle, saw).
 * Click: a short alternating-torque burst at every position change.
 * Shaft drag: viscous `damping`, Coulomb `friction`, constant `force`.
 *
 * Virtual wheel (HelloWord flywheel, iOS picker): the firmware simulates a
 * wheel with inertia and friction that carries the detents. The shaft drives
 * it through a spring-damper and the motor feels the reaction, so a flick
 * spins the wheel up, it coasts, drags the shaft along and ticks through the
 * detents until it is too slow to climb the next one; holding the knob stops
 * it. `settle` damps the wheel when slow so it lands without wobbling.
 * Speed-adaptive (Logitech MagSpeed): detents only while the wheel is slower
 * than `param`, free spin above.
 * Spin: the motor turns the knob at `param` rad/s; the hand can hold it.
 * Buzz: a `param` Hz vibration, 0.15 s on every second (notification).
 *
 * Plant compensation: with Uq = 0 the bridge shorts the motor, so back-EMF
 * brakes it with p*psi_f*w (0.0245 V per rad/s), and bearing/iron losses add
 * more. Free-spinning presets add `comp` x (c1*w + c0*sign(w)), c0 per
 * direction, from tools/ident_friction.py (`kcomp`). comp stays below 1 so a
 * model error cannot turn into positive feedback. Back-EMF (c1) repeats well
 * between runs, friction does not (0.107/0.071 V, then 0.066/0.041 V after a
 * new `cal`), so c0 is only half of the lowest friction measured.
 */
#include <math.h>
#include <stdint.h>
#include "knob.h"

#define DEG          0.0174532925f
#define TWO_PI       6.28318530718f
#define SIGN_DB      0.3f      /* rad/s: friction and its compensation are off below this (noise) */
#define SIGN_W0      0.5f      /* rad/s: then ramp in over this much speed */
#define SAW_DROP     0.2f      /* saw texture: fraction of the period with the steep side */
#define SAW_RAMP     0.08f     /* saw texture: half-width of the smoothed corners */
#define V_RUNAWAY    50.0f     /* rad/s: no torque above this (avoids positive feedback) */
#define ENDSTOP_DAMPING 0.03f  /* V/(rad/s) past an end position */
#define COUPLE_K     12.0f     /* V/rad: spring between shaft and virtual wheel */
#define COUPLE_C     0.01f     /* V/(rad/s) */
#define WHEEL_MAX    40.0f     /* rad/s */
#define STICK_V      0.05f     /* rad/s: below this the wheel can stick */
#define SETTLE_V     4.0f      /* rad/s: settling damping fades in below this */
#define CLICK_TICKS  4U        /* detent click burst, control ticks */
#define START_TICKS  16U       /* "beep" when a preset starts */
#define START_V      0.4f
#define SPIN_KP      0.05f     /* V/(rad/s) */
#define IDLE_V       0.15f     /* rad/s: below this the knob counts as resting */
#define IDLE_TICKS   1000U     /* 0.5 s at 2 kHz */
#define IDLE_ALPHA   0.0005f   /* per tick: centre creeps toward the rest angle, ~1 s */
#define IDLE_RANGE   (5.0f * DEG)
#ifndef KNOB_LPF_HZ
#define KNOB_LPF_HZ  400.0f    /* low-pass on the knob torque (clicks bypass it) */
#endif
#define NONE_POS     1000000

enum { K_NORMAL = 0, K_ONEWAY, K_MAGSPEED, K_SPIN, K_STAGED, K_BUZZ };
enum { W_SINE = 0, W_SQUARE, W_TRIANGLE, W_SAW };

typedef struct
{
  const char *name;
  uint8_t kind;
  /* positions */
  float width;       /* deg between positions (texture period, stage size) */
  int min, max;      /* range; max < min means unbounded */
  float detent;      /* V of spring torque at the snap point; STAGED: V per stage */
  float snap;        /* fraction of the width where the centre jumps; 0 = 0.5 */
  float bias;        /* snap-point shift toward position 0, fraction of width */
  float zone;        /* spring only within zone x width of a centre; 0 = everywhere */
  uint32_t magnets;  /* bit n: detent only at position n (0..31); 0 = all */
  float endstop;     /* V/rad past an end position; STAGED: spring stiffness */
  float texture;     /* V, periodic texture amplitude; BUZZ: vibration amplitude */
  uint8_t wave;      /* texture waveform */
  float click;       /* V, burst at every position change */
  /* shaft */
  float damping;     /* V/(rad/s) */
  float friction;    /* V, Coulomb */
  float force;       /* V, constant torque */
  float comp;        /* fraction of back-EMF + friction to cancel, < 1 */
  /* virtual wheel */
  float inertia;     /* V*s^2/rad; 0 = no wheel */
  float w_friction;  /* V */
  float w_viscous;   /* V/(rad/s) */
  float settle;      /* V/(rad/s) below SETTLE_V */
  float param;       /* ONEWAY: +1/-1 free direction; MAGSPEED: switch speed; SPIN: speed; BUZZ: Hz */
} Preset;

static const Preset presets[KNOB_PRESETS + 1] = {
  [0]  = {.name = "off"},
  /* --- first set: SmartKnob, HelloWord and common knob feels */
  [1]  = {.name = "free", .comp = 0.8f},
  [2]  = {.name = "fine detents", .width = 5.0f, .max = -1, .detent = 0.6f, .snap = 1.1f, .damping = 0.003f},
  [3]  = {.name = "coarse detents", .width = 30.0f, .max = -1, .detent = 1.2f, .snap = 0.6f, .damping = 0.005f},
  [4]  = {.name = "bounded 0-10", .width = 15.0f, .max = 10, .detent = 0.5f, .snap = 0.6f, .endstop = 8.0f,
          .damping = 0.005f},
  [5]  = {.name = "on/off", .width = 60.0f, .max = 1, .detent = 1.5f, .snap = 0.55f, .endstop = 8.0f,
          .damping = 0.01f},
  [6]  = {.name = "return to centre", .width = 60.0f, .max = 0, .endstop = 4.0f, .damping = 0.01f},
  [7]  = {.name = "viscous", .width = 10.0f, .max = -1, .damping = 0.04f},
  [8]  = {.name = "flywheel", .width = 10.0f, .max = -1, .comp = 0.8f,
          .inertia = 3.0e-3f, .w_friction = 0.004f, .w_viscous = 1.0e-4f},
  [9]  = {.name = "inertia detents", .width = 12.0f, .max = -1, .detent = 0.4f, .snap = 0.52f, .comp = 0.8f,
          .inertia = 2.5e-3f, .w_friction = 0.002f, .w_viscous = 1.0e-4f, .settle = 0.05f},
  [10] = {.name = "multi-turn 0-72", .width = 10.0f, .max = 72, .endstop = 8.0f, .damping = 0.003f},
  [11] = {.name = "fine values 0-255", .width = 1.0f, .max = 255, .detent = 0.15f, .snap = 1.1f,
          .endstop = 8.0f, .damping = 0.004f},
  [12] = {.name = "coarse 0-31 strong", .width = 8.2258f, .max = 31, .detent = 1.0f, .snap = 1.1f,
          .endstop = 8.0f, .damping = 0.005f},
  [13] = {.name = "coarse 0-31 weak", .width = 8.2258f, .max = 31, .detent = 0.15f, .snap = 1.1f,
          .endstop = 8.0f, .damping = 0.003f},
  [14] = {.name = "magnetic 2/10/21/22", .width = 7.0f, .max = 31, .detent = 0.9f, .snap = 0.7f,
          .magnets = (1u << 2) | (1u << 10) | (1u << 21) | (1u << 22), .endstop = 8.0f, .damping = 0.003f},
  [15] = {.name = "centre with detents", .width = 60.0f, .min = -6, .max = 6, .detent = 1.2f, .snap = 0.55f,
          .bias = 0.4f, .endstop = 8.0f, .damping = 0.01f},
  [16] = {.name = "3-way switch", .width = 40.0f, .max = 2, .detent = 1.2f, .snap = 0.55f, .endstop = 8.0f,
          .damping = 0.01f},
  [17] = {.name = "damped with walls", .width = 10.0f, .min = -5, .max = 5, .endstop = 8.0f, .damping = 0.04f},
  [18] = {.name = "friction", .width = 10.0f, .max = -1, .friction = 0.15f},
  [19] = {.name = "one-way ratchet", .kind = K_ONEWAY, .width = 15.0f, .max = -1, .detent = 0.8f, .snap = 0.6f,
          .endstop = 8.0f, .damping = 0.005f, .param = 1.0f},
  [20] = {.name = "speed adaptive", .kind = K_MAGSPEED, .width = 15.0f, .max = -1, .detent = 0.6f, .snap = 0.55f,
          .comp = 0.8f, .inertia = 2.5e-3f, .w_friction = 0.002f, .w_viscous = 1.0e-4f, .settle = 0.05f,
          .param = 8.0f},
  [21] = {.name = "texture", .width = 5.0f, .max = -1, .texture = 0.2f, .damping = 0.01f},
  [22] = {.name = "clicky", .width = 10.0f, .max = -1, .detent = 0.3f, .snap = 1.1f, .click = 0.6f,
          .damping = 0.003f},
  [23] = {.name = "spin", .kind = K_SPIN, .comp = 0.8f, .param = 10.0f},
  /* --- second set: the open-source survey */
  [24] = {.name = "menu 25deg", .width = 25.0f, .max = -1, .detent = 1.5f, .snap = 0.55f,     /* SeedLabs */
          .damping = 0.005f},
  [25] = {.name = "4-way menu", .width = 90.0f, .max = -1, .detent = 1.5f, .snap = 0.55f,     /* Super Dial */
          .damping = 0.01f},
  [26] = {.name = "16 per turn", .width = 22.5f, .max = -1, .detent = 1.2f, .snap = 0.55f,    /* Super Dial */
          .damping = 0.005f},
  [27] = {.name = "adjuster 0-14", .width = 25.714f, .max = 14, .detent = 1.2f, .snap = 1.1f, /* Wang-Nainai */
          .endstop = 8.0f, .damping = 0.005f},
  [28] = {.name = "dimmer 0-100", .width = 2.4f, .max = 100, .detent = 0.2f, .snap = 1.1f,    /* SeedLabs */
          .endstop = 8.0f, .damping = 0.004f},
  [29] = {.name = "hue", .width = 4.0f, .max = -1, .detent = 0.3f, .snap = 1.0f,              /* SeedLabs */
          .damping = 0.003f},
  [30] = {.name = "surface dial", .width = 3.0f, .max = -1, .detent = 0.25f, .snap = 1.1f,    /* Super Dial */
          .click = 0.4f, .damping = 0.003f},
  [31] = {.name = "blinds 0-20", .width = 12.3f, .max = 20, .detent = 1.0f, .snap = 1.1f,     /* SeedLabs */
          .endstop = 8.0f, .damping = 0.005f},
  [32] = {.name = "attractor 8", .width = 45.0f, .max = -1, .detent = 1.5f, .snap = 0.52f,    /* DengFOC */
          .damping = 0.008f},
  [33] = {.name = "window 90deg", .width = 15.0f, .min = -3, .max = 3, .detent = 0.6f,        /* DengFOC */
          .snap = 0.55f, .endstop = 4.0f, .damping = 0.005f},
  [34] = {.name = "notches", .width = 10.0f, .max = -1, .detent = 0.25f, .snap = 0.5f,        /* simonhtet10 */
          .zone = 0.25f, .damping = 0.003f},
  [35] = {.name = "staged spring", .kind = K_STAGED, .width = 30.0f, .detent = 0.45f,         /* Schoch */
          .endstop = 6.0f, .damping = 0.01f},
  [36] = {.name = "square texture", .width = 10.0f, .max = -1, .texture = 0.2f, .wave = W_SQUARE,
          .damping = 0.02f},
  [37] = {.name = "pawl texture", .width = 15.0f, .max = -1, .texture = 0.4f, .wave = W_SAW, .damping = 0.01f},
  [38] = {.name = "constant-force return", .width = 10.0f, .max = 30, .force = -0.3f, .endstop = 8.0f,
          .damping = 0.01f},
  [39] = {.name = "inertia list 0-30", .width = 12.0f, .max = 30, .detent = 0.4f, .snap = 0.52f,  /* iOS */
          .endstop = 2.5f, .comp = 0.8f, .inertia = 2.5e-3f, .w_friction = 0.002f, .w_viscous = 1.0e-4f,
          .settle = 0.05f},
  [40] = {.name = "buzz", .kind = K_BUZZ, .texture = 0.6f, .param = 120.0f},
  [41] = {.name = "damped spring", .width = 60.0f, .max = 0, .endstop = 3.0f, .damping = 0.06f}, /* zhiziranyi */
};

volatile float knob_strength = 1.0f;
/* experiments/2026-10-20_力反馈旋钮/data/friction_ident*.csv */
volatile float knob_comp_c0p = 0.033f;  /* V, half the lowest forward friction (0.066) */
volatile float knob_comp_c0n = 0.020f;  /* V, half the lowest backward friction (0.041) */
volatile float knob_comp_c1 = 0.0232f;  /* V/(rad/s), lowest back-EMF + viscous slope */

static int preset_id;
static const Preset *p = &presets[0];
static float center, origin;
static int position;
static float wheel_angle, wheel_vel;
static int wheel_free;          /* MAGSPEED: detents released */
static int at_end;              /* last positions() call was past an end */
static uint32_t click_left, start_left, idle_ticks;
static float uq_lpf, t_buzz;

static float clampf(float v, float lo, float hi)
{
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

static float soft_sign(float v)
{
  float m = clampf((fabsf(v) - SIGN_DB) / SIGN_W0, 0.0f, 1.0f);
  return (v < 0.0f) ? -m : m;
}

/* Ratchet-pawl profile (as torque, + pushes forward): a long gentle slope
 * against forward motion, then a short steep drop that throws the knob on.
 * Backwards the steep side comes first. Corners are ramps so the stiffness
 * stays finite; the mean is zero, so no net torque per period. */
static float saw_shape(float ph)
{
  const float lo = -SAW_DROP / (1.0f - SAW_DROP);   /* gentle side */
  const float edge = 1.0f - SAW_DROP;
  ph -= floorf(ph);
  if (ph > 1.0f - SAW_RAMP)
  {
    ph -= 1.0f;
  }
  if (ph < SAW_RAMP)
  {
    return 1.0f + (ph + SAW_RAMP) / (2.0f * SAW_RAMP) * (lo - 1.0f);
  }
  if (ph < edge - SAW_RAMP)
  {
    return lo;
  }
  if (ph < edge + SAW_RAMP)
  {
    return lo + (ph - edge + SAW_RAMP) / (2.0f * SAW_RAMP) * (1.0f - lo);
  }
  return 1.0f;
}

void knob_start(int preset, float angle, float velocity)
{
  if (preset < 1 || preset > KNOB_PRESETS)
  {
    preset = 1;
  }
  preset_id = preset;
  p = &presets[preset];
  center = angle;
  origin = angle;
  position = (p->max < p->min || (p->min <= 0 && p->max >= 0)) ? 0 : p->min;
  wheel_angle = angle;
  wheel_vel = clampf(velocity, -WHEEL_MAX, WHEEL_MAX);   /* switching while spinning keeps the momentum */
  wheel_free = 0;
  at_end = 0;
  click_left = 0;
  start_left = START_TICKS;
  idle_ticks = 0;
  uq_lpf = 0.0f;
  t_buzz = 0.0f;
}

static float wave(float a)
{
  float ph = (a - origin) / (p->width * DEG);
  float s = sinf(TWO_PI * ph);
  switch (p->wave)
  {
    case W_SQUARE:   return tanhf(2.0f * s) / 0.96403f;
    case W_TRIANGLE: return 0.63662f * asinf(s);
    case W_SAW:      return -saw_shape(ph);
    default:         return s;
  }
}

/* Position springs, endstops and texture at angle `a`. Returns V. */
static float positions(float a, float s)
{
  at_end = 0;
  if (p->width <= 0.0f)
  {
    return 0.0f;
  }
  float w = p->width * DEG;
  float t = 0.0f;

  if (p->kind == K_STAGED)
  {
    float e = a - center;
    float stage = floorf(fabsf(e) / w) + 1.0f;
    position = (int)((e >= 0.0f) ? stage - 1.0f : 1.0f - stage);
    t = -clampf(p->endstop * e, -stage * p->detent, stage * p->detent);
    at_end = 1;   /* spring everywhere: no idle re-centre, endstop damping */
    return s * t;
  }

  int lo = (p->max >= p->min) ? p->min : -NONE_POS;
  int hi = (p->max >= p->min) ? p->max : NONE_POS;
  if (p->kind == K_ONEWAY)
  {
    if (p->param > 0.0f)
    {
      lo = position;
    }
    else
    {
      hi = position;
    }
  }

  float snap = (p->snap > 0.0f) ? p->snap : 0.5f;   /* no detents: count to the nearest position */
  float err = a - center;
  for (int i = 0; i < 4; i++)
  {
    float up = (snap + ((position >= 0) ? p->bias : -p->bias)) * w;
    float down = (snap + ((position <= 0) ? p->bias : -p->bias)) * w;
    if (err > up && position < hi)
    {
      center += w;
      err -= w;
      position++;
      click_left = CLICK_TICKS;
    }
    else if (err < -down && position > lo)
    {
      center -= w;
      err += w;
      position--;
      click_left = CLICK_TICKS;
    }
    else
    {
      break;
    }
  }

  float dz = fminf(0.2f * w, 1.0f * DEG);
  float e = (err > dz) ? err - dz : (err < -dz) ? err + dz : 0.0f;
  at_end = (err > 0.0f && position >= hi) || (err < 0.0f && position <= lo);
  if (at_end)
  {
    t = -p->endstop * e;
  }
  else if (p->detent > 0.0f)
  {
    int magnet = (p->magnets == 0U) ||
                 (position >= 0 && position < 32 && (p->magnets & (1UL << position)) != 0U);
    int in_zone = (p->zone <= 0.0f) || (fabsf(err) < p->zone * w);
    if (magnet && in_zone)
    {
      t = -(p->detent / (snap * w)) * e;
    }
  }
  if (p->texture > 0.0f)
  {
    t -= p->texture * wave(a);
  }
  return s * t;
}

/* One step of the virtual wheel; returns the coupling torque on the shaft. */
static float wheel(float angle, float vel, float dt, float s, float ulim)
{
  float tc = clampf(COUPLE_K * (angle - wheel_angle) + COUPLE_C * (vel - wheel_vel), -ulim, ulim);

  if (p->kind == K_MAGSPEED)
  {
    float v = fabsf(wheel_vel);
    if (!wheel_free && v > p->param)
    {
      wheel_free = 1;
    }
    else if (wheel_free && v < 0.7f * p->param)
    {
      wheel_free = 0;
    }
  }
  float td = positions(wheel_angle, s);   /* keeps tracking the position even when released */
  if (wheel_free)
  {
    td = 0.0f;
  }

  float slow = 1.0f - fabsf(wheel_vel) / SETTLE_V;
  float c = p->w_viscous + ((slow > 0.0f) ? p->settle * slow : 0.0f);
  float net = tc + td - c * wheel_vel;
  float fr = s * p->w_friction;
  if (fabsf(wheel_vel) < STICK_V && fabsf(net) <= fr)
  {
    wheel_vel = 0.0f;   /* static friction holds the wheel */
  }
  else
  {
    net -= (wheel_vel > 0.0f || (wheel_vel == 0.0f && net > 0.0f)) ? fr : -fr;
    wheel_vel = clampf(wheel_vel + net / p->inertia * dt, -WHEEL_MAX, WHEEL_MAX);
  }
  wheel_angle += wheel_vel * dt;
  return -tc;
}

/* While the knob rests, let the detent centre creep to the rest angle. */
static void idle_recentre(float angle, float vel)
{
  idle_ticks = (fabsf(vel) < IDLE_V) ? idle_ticks + 1U : 0U;
  if (idle_ticks > IDLE_TICKS && !at_end && fabsf(angle - center) < IDLE_RANGE)
  {
    center += IDLE_ALPHA * (angle - center);
  }
}

float knob_tick(float angle, float vel, float dt, float ulim)
{
  float s = knob_strength;
  float uq;

  if (p->kind == K_SPIN)
  {
    uq = SPIN_KP * (p->param - vel);
  }
  else if (p->kind == K_BUZZ)
  {
    t_buzz = fmodf(t_buzz + dt, 1.0f);
    uq = (t_buzz < 0.15f) ? s * p->texture * sinf(TWO_PI * p->param * t_buzz) : 0.0f;
  }
  else if (p->inertia > 0.0f)
  {
    uq = wheel(angle, vel, dt, s, ulim);
  }
  else
  {
    uq = positions(angle, s);
    float c = at_end ? fmaxf(p->damping, ENDSTOP_DAMPING) : p->damping;   /* endstops: no rebound */
    uq -= s * c * vel;
    if (p->detent > 0.0f && p->kind != K_STAGED)
    {
      idle_recentre(angle, vel);
    }
  }
  uq -= s * p->friction * soft_sign(vel);
  uq += s * p->force;
  if (p->comp > 0.0f)
  {
    float sg = soft_sign(vel);
    uq += p->comp * (knob_comp_c1 * vel + ((sg > 0.0f) ? knob_comp_c0p : knob_comp_c0n) * sg);
  }

  /* smooth the 2 kHz steps (encoder noise x stiffness), then add the clicks */
  if (p->kind != K_BUZZ)
  {
    uq_lpf += (1.0f - expf(-TWO_PI * KNOB_LPF_HZ * dt)) * (uq - uq_lpf);
    uq = uq_lpf;
  }
  if (click_left > 0U)
  {
    uq += ((click_left & 1U) ? 1.0f : -1.0f) * s * p->click;
    click_left--;
  }
  if (start_left > 0U)
  {
    uq += ((start_left & 2U) ? 1.0f : -1.0f) * START_V;
    start_left--;
  }

  if (fabsf(vel) > V_RUNAWAY)
  {
    uq = 0.0f;
  }
  return clampf(uq, -ulim, ulim);
}

int knob_preset(void)
{
  return preset_id;
}

int knob_position(void)
{
  return position;
}

float knob_center(void)
{
  return center;
}

float knob_wheel_velocity(void)
{
  return (p->inertia > 0.0f) ? wheel_vel : 0.0f;
}

const char *knob_name(int preset)
{
  return (preset >= 0 && preset <= KNOB_PRESETS) ? presets[preset].name : "?";
}
