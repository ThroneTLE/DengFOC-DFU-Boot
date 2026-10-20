#ifndef __FOC_H
#define __FOC_H

#include <stdint.h>

typedef enum
{
  MODE_OFF = 0,
  MODE_VECTOR,     /* fixed voltage vector, used by calibration */
  MODE_OPENLOOP,   /* rotating vector without the encoder */
  MODE_TORQUE,     /* closed loop, Uq = target */
  MODE_VELOCITY,   /* closed loop, velocity PI -> Uq */
  MODE_ANGLE,      /* closed loop, angle P -> velocity PI -> Uq */
  MODE_KNOB        /* closed loop, haptic knob preset (knob.c) -> Uq */
} FocMode;

typedef enum
{
  FAULT_NONE = 0,
  FAULT_NFAULT,
  FAULT_ENCODER,
  FAULT_VBUS,
  FAULT_TEMP,
  FAULT_DRV_CONFIG
} FocFault;

typedef struct
{
  /* configuration, written from the main loop */
  volatile float vbus;          /* V */
  volatile float u_limit;       /* V, phase-voltage amplitude limit */
  volatile float vel_limit;     /* rad/s */
  volatile float vel_p, vel_i;  /* V/(rad/s), V/rad */
  volatile float ang_p;         /* (rad/s)/rad */
  volatile float vel_tf;        /* s, velocity low-pass time constant */
  volatile int   pole_pairs;
  volatile int   dir;           /* +1/-1: sensor direction vs electrical angle */
  volatile float zero_offset;   /* rad, electrical */
  volatile int   calibrated;

  /* command */
  volatile FocMode mode;
  volatile float target;        /* V, rad/s or rad depending on mode */
  volatile float vec_ud, vec_angle;  /* MODE_VECTOR */
  volatile float ol_speed;           /* MODE_OPENLOOP, mechanical rad/s */

  /* state, written by the control interrupt */
  volatile float mech_angle;    /* rad, single turn, sensor direction */
  volatile float shaft_angle;   /* rad, multi-turn, motor direction */
  volatile float velocity;      /* rad/s, motor direction */
  volatile float elec_angle;    /* rad */
  volatile float uq;            /* V applied */
  volatile float vel_ref;       /* rad/s, inner velocity reference (VELOCITY/ANGLE) */
  volatile float fet_temp;      /* C, updated by the main loop */
  volatile float pos_offset;    /* rad, shaft angle shown as 0 deg */
  volatile uint16_t raw;
  volatile uint32_t enc_errors;
  volatile uint32_t isr_us;
  volatile uint32_t isr_count;
  volatile FocFault fault;
  volatile int armed;           /* EN_GATE high and DRV configured */
} Foc;

extern Foc foc;

void foc_init(uint32_t rate_hz);
uint32_t foc_rate(void);
void foc_isr(void);
void foc_output_enable(int on);
void foc_trip(FocFault f);
const char *foc_mode_name(FocMode m);
const char *foc_fault_name(FocFault f);

/* Scope: the control interrupt pushes one frame every `decim` ticks into a
 * ring buffer; the main loop pops frames and streams them as VOFA+ JustFloat.
 * Channels: 0 target (deg in ANGLE mode, else mode unit), 1 position deg,
 * 2 velocity rad/s, 3 velocity reference rad/s, 4 Uq V, 5 electrical angle rad,
 * 6 VBUS V, 7 ISR time us, 8 mode (FocMode), 9 fault (FocFault),
 * 10 FET temperature C, 11 calibrated (0/1), 12 knob detent index,
 * 13 knob preset (0 outside MODE_KNOB). In KNOB mode channel 0 is the current
 * detent centre in deg. */
#define SCOPE_CH 14
void scope_start(uint32_t decim);
void scope_stop(void);
int scope_running(void);
int scope_pop(float *frame);
uint32_t scope_dropped(void);

#endif /* __FOC_H */
