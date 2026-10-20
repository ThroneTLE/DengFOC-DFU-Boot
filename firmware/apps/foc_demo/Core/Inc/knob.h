#ifndef __KNOB_H
#define __KNOB_H

/* Haptic knob: software detents, endstops, springs, damping and a virtual
 * flywheel on top of the voltage-mode FOC. knob_tick() runs in the control
 * interrupt while foc.mode == MODE_KNOB. */

#define KNOB_PRESETS 41  /* table and names in knob.c; `knob` with no argument lists them */

/* call with the control interrupt masked; `velocity` seeds the virtual wheel */
void knob_start(int preset, float angle, float velocity);
float knob_tick(float angle, float velocity, float dt, float ulim);   /* returns Uq, V */
int knob_preset(void);
int knob_position(void);         /* detent index */
float knob_center(void);         /* rad, shaft angle of the current detent centre */
float knob_wheel_velocity(void); /* rad/s of the virtual wheel, 0 without one */
const char *knob_name(int preset);

extern volatile float knob_strength;   /* scales detents, damping and wheel friction, default 1 */
extern volatile float knob_comp_c0p, knob_comp_c0n, knob_comp_c1;   /* plant compensation: V, V, V/(rad/s) */

#endif /* __KNOB_H */
