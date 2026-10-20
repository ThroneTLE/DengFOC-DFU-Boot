"""Simulate the haptic knob (firmware knob.c, compiled for the PC) on a model
of the 2804 motor, the AS5600 and the 2 kHz control interrupt.

Plant: J dw/dt = Kt*i - friction + hand,  L di/dt = Uq - Ke*w - R*i.
Controller: same as foc_isr(): 12-bit angle after the AS5600 filter, velocity
low-pass 10 ms, knob_tick() -> Uq applied after the ISR run time.

  python tools/knob_sim.py flick --preset 9          # flick and let go
  python tools/knob_sim.py spin --preset 9 --v0 20   # switch to the preset while spinning (bench test)
  python tools/knob_sim.py grab --preset 9           # flick, then hold the knob
  python tools/knob_sim.py turn --preset 3           # turn slowly by hand: torque you feel
  python tools/knob_sim.py rest --preset 9           # untouched: noise, buzz
Add -D NAME=VALUE to override a #define in knob.c (e.g. -D COUPLE_K=8.0f), --plot out.png for a figure.
"""
import argparse
import ctypes
import math
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "firmware", "apps", "foc_demo", "Core")
BUILD = os.path.join(HERE, "build")

# 2804 + AS5600 (docs/电机与编码器参数.md; J from the 2026-10-20 kick test)
J = 2.5e-6        # kg*m^2, bare rotor
R = 2.55          # ohm
L = 0.35e-3 * R   # H, from tau_e = 0.35 ms
KT = 0.0368       # N*m/A (dq, amplitude-invariant: 1.5*p*psi_f)
KE = 0.0225       # V/(rad/s) seen in Uq (p*psi_f); theory 0.0245, see T_COULOMB
# losses measured with tools/ident_friction.py (2026-10-20), two runs:
# c0 0.107/0.071 V then 0.066/0.041 V (forward/back), c1 0.023-0.028. The model takes the
# lowest losses (worst case for positive feedback); --plant scales them further down.
T_COULOMB = 0.041 * KT / R   # N*m backward
T_COULOMB_FWD = 0.066 * KT / R  # N*m forward
B_VISC = 0.0016 * KT / R     # N*m*s, c1 0.0241 above the back-EMF
RATE = 2000
DT_SIM = 1e-5
ISR_DELAY = 130e-6
SENSOR_TAU = 0.15e-3   # AS5600 slow filter 2x
NOISE_DEG = 0.043      # AS5600 output noise, 1 sigma, slow filter 2x (datasheet)
ENC_HYST = 2           # counts of encoder hysteresis in foc_isr()
VEL_TF = 0.01
ULIM = 2.0


def build(defines):
    os.makedirs(BUILD, exist_ok=True)
    tag = "_".join(d.replace("=", "-").replace(".", "p") for d in defines) or "default"
    dll = os.path.join(BUILD, f"knob_{abs(hash(tag)) % 10**8}.dll")
    cmd = ["gcc", "-O2", "-shared", "-o", dll, os.path.join(SRC, "Src", "knob.c"), "-I", os.path.join(SRC, "Inc")]
    cmd += [f"-D{d}" for d in defines]
    subprocess.run(cmd, check=True)
    lib = ctypes.CDLL(dll)
    lib.knob_start.argtypes = [ctypes.c_int, ctypes.c_float, ctypes.c_float]
    lib.knob_tick.argtypes = [ctypes.c_float] * 4
    lib.knob_tick.restype = ctypes.c_float
    lib.knob_center.restype = ctypes.c_float
    lib.knob_wheel_velocity.restype = ctypes.c_float
    lib.knob_name.restype = ctypes.c_char_p
    return lib


def simulate(lib, preset, seconds, hand, w0=0.0, strength=1.0, seed=1):
    """hand(t, theta, w) -> N*m. Returns a dict of per-tick arrays."""
    rng = np.random.default_rng(seed)
    held = 0
    rng = np.random.default_rng(seed)
    ctypes.c_float.in_dll(lib, "knob_strength").value = strength
    theta, w, i = 0.0, w0, 0.0
    sensed = 0.0
    vel = w0
    prev_raw = 0
    turns = 0.0
    uq_now = uq_next = 0.0
    lib.knob_start(preset, 0.0, w0)
    n_tick = int(seconds * RATE)
    sub = int(round(1.0 / RATE / DT_SIM))
    delay_steps = int(round(ISR_DELAY / DT_SIM))
    out = {k: np.zeros(n_tick) for k in ("t", "theta", "w", "uq", "hand", "pos", "center", "wheel")}
    for k in range(n_tick):
        t0 = k / RATE
        # --- control interrupt: read the angle, run the knob, schedule Uq
        noisy = sensed + rng.normal(0.0, math.radians(NOISE_DEG)) if NOISE_DEG > 0 else sensed
        raw = int(math.floor((noisy % (2 * math.pi)) / (2 * math.pi) * 4096)) % 4096
        dr = (raw - held + 2048) % 4096 - 2048
        if dr > ENC_HYST:
            held = (raw - ENC_HYST) % 4096
        elif dr < -ENC_HYST:
            held = (raw + ENC_HYST) % 4096
        raw = held
        d = raw - prev_raw
        if d > 2048:
            d -= 4096
        elif d < -2048:
            d += 4096
        prev_raw = raw
        dang = d * 2 * math.pi / 4096
        turns += dang
        a = (1.0 / RATE) / (VEL_TF + 1.0 / RATE)
        vel += a * (dang * RATE - vel)
        uq_next = lib.knob_tick(turns, vel, 1.0 / RATE, ULIM)
        # --- plant between ticks
        hsum = 0.0
        for s in range(sub):
            if s == delay_steps:
                uq_now = uq_next
            t = t0 + s * DT_SIM
            th = hand(t, theta, w)
            hsum += th
            di = (uq_now - KE * w - R * i) / L
            i += di * DT_SIM
            fric = (T_COULOMB_FWD if w > 0 else T_COULOMB) * math.tanh(w / 0.05) + B_VISC * w
            w += (KT * i - fric + th) / J * DT_SIM
            theta += w * DT_SIM
            sensed += (theta - sensed) * DT_SIM / SENSOR_TAU
        out["t"][k] = t0
        out["theta"][k] = theta
        out["w"][k] = w
        out["uq"][k] = uq_now
        out["hand"][k] = hsum / sub
        out["pos"][k] = lib.knob_position()
        out["center"][k] = lib.knob_center()
        out["wheel"][k] = lib.knob_wheel_velocity()
    return out


def flick_torque(peak=0.02, dur=0.04):
    """Half-sine finger push of `peak` N*m for `dur` s, then no contact."""
    return lambda t, th, w: peak * math.sin(math.pi * t / dur) if t < dur else 0.0


def report(r, title):
    pos = r["pos"]
    ticks = np.flatnonzero(np.diff(pos) != 0)
    moving = np.flatnonzero(np.abs(r["w"]) > 0.3)
    stop_t = r["t"][moving[-1]] if len(moving) else 0.0
    end = len(r["t"]) - 1
    resid = math.degrees(r["theta"][end] - r["center"][end])
    print(f"== {title}")
    print(f"  peak speed {np.max(np.abs(r['w'])):.1f} rad/s, coasting stops at {stop_t:.2f} s, "
          f"{len(ticks)} detent ticks, final detent {pos[end]:.0f}, rest {resid:+.2f} deg from centre")
    if len(ticks) > 2:
        gaps = np.diff(r["t"][ticks]) * 1000
        print("  tick spacing ms: first", np.round(gaps[:4]).astype(int).tolist(), " last", np.round(gaps[-4:]).astype(int).tolist())
    tail = r["uq"][int(0.8 * end):]
    print(f"  |Uq| max {np.max(np.abs(r['uq'])):.2f} V; last 20 %: Uq std {np.std(tail) * 1000:.1f} mV, "
          f"speed std {np.std(r['w'][int(0.8 * end):]):.3f} rad/s")


def hiss(r):
    """Audible torque noise: Uq above 100 Hz, second half of the run."""
    u = r["uq"][len(r["uq"]) // 2:]
    a = math.exp(-2 * math.pi * 100 / RATE)
    lp = np.zeros_like(u)
    for k in range(1, len(u)):
        lp[k] = a * lp[k - 1] + (1 - a) * u[k]
    hp = u - lp
    steps = np.count_nonzero(np.abs(np.diff(u)) > 0.5e-3) / (len(u) / RATE)
    print(f"  hiss: Uq >100 Hz {np.std(hp[200:]) * 1000:.2f} mV rms, {steps:.0f} Uq steps/s, "
          f"mean Uq {np.mean(u) * 1000:.0f} mV")


def drag(rate):
    """Finger dragging the knob at `rate` rad/s through a stiff spring."""
    return lambda t, th, w: -0.5 * (th - rate * t) - 2e-3 * (w - rate)


def check_all(lib, strength):
    """Every preset: rest, slow turn both ways, flick; plus a 15 % weaker plant for runaway."""
    global KE, T_COULOMB, T_COULOMB_FWD, B_VISC
    names = []
    i = 1
    while True:
        n = lib.knob_name(i).decode()
        if n == "?":
            break
        names.append(n)
        i += 1
    print(f"{'#':>2} {'preset':22s} {'rest mV':>7} {'turn+ mNm':>9} {'pos+':>5} {'turn- mNm':>9} {'pos-':>5} "
          f"{'flick v':>7} {'stop s':>6} {'ticks':>5} {'weak plant':>10}")
    for k, name in enumerate(names, 1):
        rest = simulate(lib, k, 1.0, lambda t, th, w: 0.0, strength=strength)
        rest_mv = np.std(rest["uq"][1000:]) * 1000 + abs(np.mean(rest["uq"][1000:])) * 1000
        up = simulate(lib, k, 2.0, drag(1.0), strength=strength)
        dn = simulate(lib, k, 2.0, drag(-1.0), strength=strength)
        fl = simulate(lib, k, 3.0, flick_torque(0.02), strength=strength)
        moving = np.flatnonzero(np.abs(fl["w"]) > 0.3)
        stop = fl["t"][moving[-1]] if len(moving) else 0.0
        ticks = int(np.count_nonzero(np.diff(fl["pos"])))
        saved = (KE, T_COULOMB, T_COULOMB_FWD, B_VISC)
        KE, T_COULOMB, T_COULOMB_FWD, B_VISC = KE * 0.85, T_COULOMB * 0.85, T_COULOMB_FWD * 0.85, B_VISC * 0.85
        weak = simulate(lib, k, 3.0, flick_torque(0.02), strength=strength)
        KE, T_COULOMB, T_COULOMB_FWD, B_VISC = saved
        runaway = np.max(np.abs(weak["w"][-1000:])) > 1.0 and lib.knob_name(k).decode() != "spin"
        print(f"{k:2d} {name:22s} {rest_mv:7.1f} {np.sqrt(np.mean(up['hand'] ** 2)) * 1000:9.2f} "
              f"{up['pos'][-1]:5.0f} {np.sqrt(np.mean(dn['hand'] ** 2)) * 1000:9.2f} {dn['pos'][-1]:5.0f} "
              f"{np.max(np.abs(fl['w'])):7.1f} {stop:6.2f} {ticks:5d} {'RUNAWAY' if runaway else 'ok':>10}")


def main():
    global KE, T_COULOMB, T_COULOMB_FWD, B_VISC, NOISE_DEG, ENC_HYST
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario", choices=["flick", "spin", "grab", "turn", "rest", "hold", "all"])
    ap.add_argument("--preset", type=int, default=9)
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--peak", type=float, default=0.02, help="flick torque, N*m")
    ap.add_argument("--v0", type=float, default=20.0)
    ap.add_argument("--strength", type=float, default=1.0)
    ap.add_argument("-D", dest="defines", action="append", default=[])
    ap.add_argument("--plot")
    ap.add_argument("--plant", type=float, default=1.0, help="scale the plant back-EMF and friction (robustness)")
    ap.add_argument("--noise", type=float, default=NOISE_DEG, help="encoder noise, deg rms")
    ap.add_argument("--hyst", type=int, default=ENC_HYST, help="encoder hysteresis, counts")
    args = ap.parse_args()
    KE, T_COULOMB, B_VISC = KE * args.plant, T_COULOMB * args.plant, B_VISC * args.plant
    T_COULOMB_FWD *= args.plant
    NOISE_DEG, ENC_HYST = args.noise, args.hyst
    lib = build(args.defines)

    if args.scenario == "all":
        check_all(lib, args.strength)
        return
    if args.scenario == "flick":
        r = simulate(lib, args.preset, args.seconds, flick_torque(args.peak), strength=args.strength)
    elif args.scenario == "spin":
        r = simulate(lib, args.preset, args.seconds, lambda t, th, w: 0.0, w0=args.v0, strength=args.strength)
    elif args.scenario == "grab":
        flick = flick_torque(args.peak)
        hold = {}

        def hand(t, th, w):
            if t < 0.3:
                return flick(t, th, w)
            hold.setdefault("at", th)
            return -0.5 * (th - hold["at"]) - 2e-3 * w   # finger: stiff spring-damper
        r = simulate(lib, args.preset, args.seconds, hand, strength=args.strength)
    elif args.scenario == "turn":
        def hand(t, th, w):   # finger drags the knob at 1 rad/s through a stiff spring
            return -0.5 * (th - 1.0 * t) - 2e-3 * (w - 1.0)
        r = simulate(lib, args.preset, args.seconds, hand, strength=args.strength)
        print(f"  felt torque: max {np.max(np.abs(r['hand'])) * 1000:.1f} mN*m, "
              f"rms {np.sqrt(np.mean(r['hand'] ** 2)) * 1000:.1f} mN*m")
    elif args.scenario == "hold":
        # finger holds the knob 2.5 deg off the detent centre, like friction does at rest
        r = simulate(lib, args.preset, args.seconds, lambda t, th, w: -0.5 * (th - math.radians(2.5)) - 2e-3 * w,
                     strength=args.strength)
        hiss(r)
    else:
        r = simulate(lib, args.preset, args.seconds, lambda t, th, w: 0.0, strength=args.strength)
    report(r, f"{args.scenario}, preset {args.preset} {args.defines}")

    if args.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(3, 1, figsize=(10, 7), sharex=True)
        ax[0].plot(r["t"], r["w"], label="shaft rad/s")
        ax[0].plot(r["t"], r["wheel"], label="virtual wheel rad/s")
        ax[0].legend()
        ax[1].plot(r["t"], np.degrees(r["theta"]), label="shaft deg")
        ax[1].plot(r["t"], np.degrees(r["center"]), label="detent centre deg", drawstyle="steps-post")
        ax[1].legend()
        ax[2].plot(r["t"], r["uq"], label="Uq V")
        ax[2].plot(r["t"], r["hand"] / (KT / R), label="hand torque (V equiv.)")
        ax[2].legend()
        ax[2].set_xlabel("s")
        fig.tight_layout()
        fig.savefig(args.plot, dpi=90)
        print("plot:", args.plot)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    main()
