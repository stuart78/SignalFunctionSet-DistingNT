# ChimeNT — Disting NT port of Chime

Port of the [Signal Function Set Chime module](https://github.com/stuart78/SignalFunctionSet) — an 8-note resonating drone machine inspired by a xylophone whose resonator tubes rotate on an axis beneath the bars. Each tube's coupling (and so the note's loudness) peaks as it swings through centre.

## Concept

Eight independent voices. Each voice has:

- A **tube rotation LFO** (bidirectional triangle, phase 0..1 → tri −1..+1..−1)
- **Amplitude** follows centre proximity (window)
- **Centre crossings** can trigger strikes (bar rings and fades)
- **Three bar partials** (xylophone-ish inharmonic stack)
- **Bow vs Strike** blend: continuous drone ↔ struck ring
- **Per-voice weight** (strike likelihood / bow level)
- **Per-voice atten** (swings a shorter arc → strikes more often, never fully silent)
- **Per-voice scale degree** (0..15, wraps octaves)

## Relate modes

Global control over how the eight voices' rotation rates relate:

- **Ramp** — rates fan smoothly slow → fast across channels
- **Stepped** — integer rate ratios (periodic realignment, phasing patterns)
- **Random** — seeded random ratios (Reseed button / trigger)
- **Ripple** — strikes excite neighbours, whose rotation speeds up briefly

**Drift** adds a slow per-channel random rate wobble so nothing locks perfectly.

## I/O

### Inputs (9)
1. Rate CV (±5V ≈ ±2 octaves)
2. Spread CV
3. Drift CV
4. Root CV (1V/oct semitone-quantised)
5. Scale CV (1V per scale)
6. Reseed trigger
7. Clock (syncs rotations; RATE knob picks clocks-per-rotation from 32/16/8/4/2/1)
8. Excite CV (0–10V bow → strike)
9. Oct CV (1V per octave)

### Outputs (18)
- 8 × Audio out (per-voice)
- 8 × Tube LFO out (per-voice, ±5V)
- Mix L / Mix R (equal-power panned across the 8 voices)

Note: the poly V/oct + Gate outputs from VCV Chime are dropped in this port — NT's routing is one-channel-per-bus, so poly wouldn't map cleanly. Use the individual per-voice audio outs instead.

## Parameters

### Global
- **Rate** — rotation rate, 0.02–2.0 Hz (`kBy100`)
- **Spread** — rate spread across channels (0–100%). In Ripple mode: neighbour coupling.
- **Drift** — semi-free-running wobble depth (0–100%)
- **Relate** — Ramp / Stepped / Random / Ripple
- **Shape** — rotation curve, −1..+1 (`kBy100`). Negative = exponential (tube dwells at extremes, whips through centre for short bright blooms). Positive = logarithmic (lingers near centre, long swells).
- **Oct** — global octave shift, −3..+3
- **Excite** — bow ↔ strike blend (0–100%)
- **Decay** — ring / bloom time, 0.3–8.0 s (`kBy10`)
- **Root** — C..B (enum)
- **Scale** — 19 scales, same order as FugueNT (enum)
- **Reseed Now** — action: Off → On reseeds random rates

### Per voice (×8)
- **Degree** — scale degree, 0–15 (wraps octaves)
- **Weight** — strike likelihood / bow level (0–100%)
- **Atten** — swing arc, 10–100%

## DSP behaviour — ported verbatim

Sample-rate voice DSP:
- Phase advances at `rateEff / atten` per sample. Smaller arc ⇒ faster centre crossings.
- Triangle then power-curve shaping (symmetric ⇒ crossing times unchanged, clock sync preserved).
- Window `(1 - |tri|)^2` smoothed with a 4 ms one-pole (declick).
- On centre crossing, if `rand() < weight` the voice strikes: 2.5 ms attack window feeds the strike ring envelope. Ripple mode also injects coupling energy into neighbours (weighted).
- Pitch is **latched** at strike / quiet moments so a ringing note doesn't glide when root/scale/octave/degree change under it.
- Three sine partials at ratios {1, 3.932, 9.538}, amps {1, 0.40, 0.15}, decays × {1, 0.45, 0.22}.
- Bow mode is a bed of the continuous partial sum; strike mode multiplies each partial by its ring envelope. `excite` interpolates.
- Output scaled × 3.5 per voice; mix is equal-power panned across the 8 voices, × 0.5 headroom scale.

Control-rate (every 64 samples):
- Rates, drift wobble, per-voice frequencies.
- **Clock sync**: when a clock is present, the RATE knob picks a musical division from {32, 16, 8, 4, 2, 1} clocks per rotation. Even divisions → strikes on clock beats.

## Memory

All state in **SRAM**, roughly 500 bytes total. No DRAM / DTC / ITC needed.

## Display (256×64 mono)

- **Header line** — title, current key (root + scale), octave (if not zero)
- **8 columns** — one per voice:
  - Note name at the top (reflects root + scale + degree + octave)
  - A pendulum swinging left/right at the tube's phase, brighter dot at the tip when close to centre (bloom)
  - Weight bar at the foot
- **Bottom line** — relate mode label + current rotation rate in Hz

No 3D-tube visualisation from VCV — the mono display can't do the same trick, so it's stripped down to a schematic pendulum per voice.

## File structure

```
modules/chimeNT/
├── chimeNT.cpp     # single-file algorithm
└── Makefile        # standard NT example Makefile
```
