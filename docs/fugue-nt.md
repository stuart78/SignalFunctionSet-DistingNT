# FugueNT — Disting NT port of Fugue

Port of the [Signal Function Set Fugue module](https://github.com/stuart78/SignalFunctionSet) plus the per-playhead extras from FugueX. Drops the FugueX per-step trigger outs.

## Concept

8-step harmonic deviation sequencer with three independent CV/gate voices. Each voice has its own clock, its own "wander" amount (how far it deviates from the step's quantized pitch), and now its own step count, sleep division, and probability — borrowed from FugueX.

## I/O

### Inputs (21)
1. Clock A
2. Clock B  (normalled to A)
3. Clock C  (normalled to B)
4. Reset
5. Root CV
6. Scale CV
7. Steps CV (global)
8. Slew CV
9. Wander A CV
10. Wander B CV
11. Wander C CV
12. Steps A CV  (per-playhead)
13. Steps B CV
14. Steps C CV
15. Sleep A CV  (per-playhead)
16. Sleep B CV
17. Sleep C CV
18. Probability A CV  (per-playhead)
19. Probability B CV
20. Probability C CV
21. Randomize Sequence trigger

### Outputs (9, gate-then-CV per NT convention)
1. Gate A
2. Gate B
3. Gate C
4. CV A
5. CV B
6. CV C
7. Min  (lowest of the three voice voltages, per sample)
8. Mid  (middle of the three voice voltages, per sample)
9. Max  (highest of the three voice voltages, per sample)

## Parameters

### Global / tonality
- **Root** — C..B (12-step enum)
- **Scale** — 19 scales (Chromatic, Major, Minor, Pentatonic Maj/Min, Blues, Whole tone, Harmonic series, Dorian, Phrygian, Lydian, Mixolydian, Harmonic Minor, Hijaz, Hirajoshi, Pelog, Slendro, Melodic Minor, Locrian). Same order/contents as VCV Fugue v2 so patches share semantics.
- **Steps** — global step count cap, 1–8
- **Slew** — 0–100%, adaptive (slew time = `slewPercent * timeToNextActiveGate`)
- **Fader Range** — 1V / 2V / 5V (enum)
- **Harmonic Lock** — Off / On. When On, each voice picks the most consonant of 3 deviation candidates relative to the other two voices' targets.
- **S&H Mode** — Off / On. When On, voice voltage only commits on accepted clocks (no continuous slew). On a probability-suppressed step, voltage holds previous value.

### Per step (×8, shared by all voices)
- **Pitch fader** — 0–1000 (scaled ÷1000 = 0.0–1.0). Quantized at runtime to (scale × root × range).
- **Gate Step n Voice A** — Off / On
- **Gate Step n Voice B** — Off / On
- **Gate Step n Voice C** — Off / On

### Per playhead (×3)
- **Wander** — 0–100%. 0 = faithful to fader pitch; 1 = full wander across the deviation tiers.
- **Steps** — 1–8. Per-playhead step count, capped at the global Steps value.
- **Sleep** — enum: 0 / 1 / 2 / 4 / 5 / 8 / 16 / 32 / 48 / 64. Clocks the voice waits at end-of-cycle.
- **Probability** — 0–100%. Per clock, rolls whether the step's gate fires. In S&H mode, also gates the CV update.

## DSP behavior

All ported verbatim from the VCV implementation:

- **Quantization** (`faderToVoltage`): linear scan over scale degrees in semitones, pick closest to fader voltage, add root offset. Supports non-12-TET scales (Pelog, Slendro, Harmonic) via float intervals.
- **Wander** (`selectDeviationNote`): tiered deviation algorithm. Chromatic scales use semitone-interval tiers (P5/P4 → M3/m3/M6/m6 → M2/m7 → m2/M7/tritone). Diatonic uses scale-degree tiers (unison → 3rd/5th → 7th/9th/11th → 6th → chromatic neighbor). Pentatonic uses a shorter version. Stability parameter biases tier selection toward unison.
- **Harmonic Lock**: generates 3 candidates per step advance, scores each via `intervalConsonance` against the other voices' current targets, picks the highest-scoring.
- **Adaptive Slew**: per voice, looks ahead to next active gate and sets slew time = `slewPercent * timeToNextActiveGate`. With a constant clock period this gives smooth glides that always arrive exactly when the next gate fires.
- **Sleep**: counter set at end-of-cycle wrap; voice halts step advance while counter > 0.
- **Probability**: shared `probRng` (xorshift32). Roll per clock; on miss → suppress gate, and in S&H mode suppress CV update too.

## Memory

All state in SRAM. Estimate < 2 KB total. No DRAM/DTC/ITC needed.

## Display (256×64 mono, 16 grey)

**Main page** — sequencer view:
- Top status line: `ROOT  SCALE  RNG=NV` plus `[H]` and `[S]` badges for Harmonic Lock / S&H
- 8 columns spanning the width. Each column shows:
  - Vertical pitch bar (fader value height)
  - 3 small dots underneath (gate toggles A/B/C, lit = on)
  - Note name of the quantized pitch above the bar
- Playhead markers above the column for each voice's current step
- Bottom strip showing per-voice live state (current note name, sleep countdown, probability roll outcome)

**Per-voice page** (entered via button):
- Big "A" / "B" / "C" header
- Four horizontal sliders: Wander · Steps · Sleep · Probability
- Encoder cycles voices; pots edit values

## Custom UI (initial cut)

- **Pot 1** — pitch of currently selected step
- **Pot 2** — wander of focused voice
- **Pot 3** — defers to standard parameter scroll
- **Encoder L** — select step (1–8)
- **Encoder R** — switch focus voice (A/B/C)
- **Button 1** — toggle gate of focused voice on selected step
- **Button 2** — toggle between main page and per-voice page

This may iterate; the standard parameter pages always work as a fallback.

## Persistence

All algorithm parameters are auto-persisted by the NT preset system. We add a single `schemaVersion` integer via `serialise()` / `deserialise()` for future migration headroom.

## File structure

```
modules/fugueNT/
├── fugueNT.cpp     # single-file algorithm + UI
├── Makefile        # standard NT example Makefile
└── README.md       # build/install notes
```
