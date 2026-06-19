# ShiftNT — Disting NT port of Shift

Port of the [Signal Function Set Shift module](https://github.com/stuart78/SignalFunctionSet) — a 4-output CV shift register with per-output controls. 1:1 mapping of the VCV implementation.

## Concept

Four independent lanes, each with its own step count `N`, clock divider, and mode (Parallel / Cascade). Each lane outputs a CV (sampled on its tap fire) and a 1ms gate.

- **Parallel** — the lane is an N-step delay line. On each divided clock pulse, the lane pushes the current input CV into the delay line and outputs the value from N lane-steps ago.
- **Cascade** — the lane is a "tape loop" of length N. Its CV cycles through the buffer continuously at the lane-clock rate (so it never sits still). The parent lane (i−1) writes its current value into the buffer every time *it* ticks — slower than the read rate — so the buffer evolves gradually as new content drips in. Cascade on lane A falls back to Parallel (no parent).

Plus a **Jumble** output — on each input clock, picks a random lane's current held value, S&H, with its own gate pulse.

The full-depth history ring is mirrored to in parallel with the active buffer. When the CV input is **disconnected**, the lane outputs cycle through the 16-slot history at lane-clock rate instead — so a lane with N=1 in cascade mode still has a meaningful "play out the remainder" stream when CV drops.

## I/O

### Inputs (8)
1. CV (data, sampled on each tap fire)
2. Clock (advances all taps in parallel, and the cascade chain root)
3. N CV (±5V → ±15, summed into every N pot)
4. Reset (clears all counters, buffers, and held values)
5. Step CV A (±5V → ±15, summed with N pot A)
6. Step CV B
7. Step CV C
8. Step CV D

### Outputs (10, paired gate-then-CV)
1. Gate A
2. CV A
3. Gate B
4. CV B
5. Gate C
6. CV C
7. Gate D
8. CV D
9. Jumble Gate
10. Jumble CV

## Parameters

### Per lane (×4)
- **N** — step count, 0–15, integer. `0` = passthrough (output = input, no delay).
- **Mode** — Parallel / Cascade (enum)
- **Div** — input clock divider, 6 positions: `/1`, `/2`, `/3`, `/4`, `/5`, `/8`

### Action
- **Reset Now** — Off → On flips trigger a full clear (same effect as the Reset trigger input). Flip back to Off to re-arm. (No auto-reset on this enum; writing the same parameter that just changed crashes the host.)

### Routing
All 8 input bus assignments and 10 output bus + mode pairs. Default routing pairs gate→CV adjacent per lane (Gate A=13/CV A=14, Gate B=15/CV B=16, Gate C=17/CV C=18, Gate D=19/CV D=20). Jumble defaults to unrouted.

## Memory

All state in **SRAM**. ~1 KB total — 4 × 16 floats for `delayLine`, 4 × 16 floats for `historyLine`, plus ring pointers, divider counters, held values, gate-pulse counters. No DRAM / DTC / ITC needed.

## Display (256×64 mono)

- **Header line**: `SHIFT` on the left, `J: ±X.XX` on the right (current jumble held value)
- **4 lane rows**, one per lane:
  - Letter A/B/C/D (normal font)
  - `P` or `C` mode indicator
  - `/N` divider
  - `N=K` step count
  - Horizontal bar centred at zero, filled out to the held value (±10V → ±50 px)
  - Activity dot (bright while the lane's gate is currently high)
  - Numeric held voltage on the far right

## DSP behaviour — identical to Shift

All ported verbatim from `src/shift.cpp`:

- Per-lane divider counter wraps at the `laneDiv` value, gating all per-lane work.
- Parallel: reads from the always-written full-depth history ring at lookback = N, then writes the current input. Using the continuously-written ring (rather than an N-sized buffer) keeps the delay correct even when N is modulated on the fly via the Step CV — an N-sized buffer's slots go stale as N changes and the output reads frozen.
- Cascade: separate `readIdx` (advances at lane-clock rate) and `writeIdx` (advances only when parent fires) into the N-slot active buffer. Pulls the *parent's* held value, not the global CV.
- N = 0 in either mode: clean passthrough. Parallel outputs the current input; cascade outputs the parent value.
- Cascade on lane A falls back to Parallel.
- Disconnected CV input: lane reads from the full-depth `historyLine[16]` ring at lane-clock rate, regardless of N.
- Gates fire on the lane-clock tick whether or not CV is connected.
- Jumble re-rolls on every input clock (not lane-divided), picks a random lane uniformly, S&H, fires 1 ms gate.

## File structure

```
modules/shiftNT/
├── shiftNT.cpp     # single-file algorithm
└── Makefile        # standard NT example Makefile
```
