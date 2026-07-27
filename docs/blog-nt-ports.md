# Signal Function Set on the Disting NT

Three modules from the Signal Function Set VCV plugin now run natively on Expert Sleepers' [Disting NT](https://www.expert-sleepers.co.uk/distingNT.html): **FugueNT** (3-voice harmonic-deviation sequencer), **ShiftNT** (4-output CV shift register), and **ChimeNT** (8-voice resonating drone). Full sources and per-plugin releases at [github.com/stuart78/SignalFunctionSet-DistingNT](https://github.com/stuart78/SignalFunctionSet-DistingNT).

This post is what I learned porting them.

---

## Two APIs for one module

The Disting NT gives you two ways to add code:

**Lua scripts.** Great for CV/gate work. A `step` function runs every 1 ms, so anything at LFO/sequencer rates (2 kHz and slower for gates) is fine. Params get an auto-generated UI. Triggers and gates get dedicated callbacks so you don't have to Schmitt-trigger inside your step. There is no audio-rate access, no sample-buffer loading, no filesystem I/O. If your module is Fugue-shaped — advances on a clock, emits CV — Lua is enough.

**C++ plugins.** A `step` callback runs per audio block (48 kHz sample rate, block size typically 4–128 samples). You get direct pointers into every bus (12 input + 8 output + 44 aux), a 256×64 monochrome framebuffer for custom UI, MIDI in/out, and JSON preset persistence. Compiled bare-metal for Cortex-M7, no STL, no exceptions, no heap. Memory has to be declared up-front in four pools (SRAM, DRAM, DTC, ITC).

For Signal Function Set, the C++ path was the only real option. Fugue's harmonic-lock consonance scoring runs at every clock event; Shift keeps 16-slot delay lines per lane; Chime is 24 sine oscillators plus envelopes at sample rate. None of that fits Lua's 1 ms cadence.

The good news: bare-metal C++11 on M7 hard-float is a comfortable environment for audio DSP, and the SDK is small enough to read start-to-finish in an evening.

---

## FugueNT first — three gotchas

Fugue was the shakedown. Straightforward CV/gate module — same shape as the VCV original, same tables of scales and deviation tiers, same adaptive-slew math. What tripped me was the loader.

### 1. `static` functions and relocation sections

I wrote every plugin callback as `static` — clean file-local scope. Compiled fine. Copied `.o` to the SD card. The module showed **"failed"** on load.

Diffing my `.o` against the working `gainCustomUI.o` from the SDK examples, one section was different: the relocations for the factory's function pointers (`construct`, `step`, `draw`, `customUi`, …) landed in `.data.rel.ro.local` in mine but `.data.rel.ro` in the working example. The loader processes the non-`.local` section only, leaving my factory struct with unpatched function pointers pointing at garbage — first call, crash.

The cause: `static` functions get local linkage, and GCC puts pointers to locally-linked symbols in the `.local` variant. Removing `static` from the nine functions referenced by the factory struct fixed it.

**Lesson:** functions whose address goes into the factory struct must have external linkage. Internal helpers can stay `static`.

### 2. Batch parameter writes crash the host

FugueNT has a "Randomize Sequence" input — a gate that scrambles all 8 pitch parameters. First implementation:

```cpp
// From step(), on the trigger edge:
for (int s = 0; s < 8; s++) {
    NT_setParameterFromUi(algIdx, STEP_PITCH(s) + off, randomValue());
}
```

Result: instant crash. Tried `NT_setParameterFromAudio` (the audio-thread variant) instead — same crash. Tried calling from `parameterChanged` instead of `step` — same crash.

Grepping every example plugin in the SDK for `NT_setParameterFromUi` returned exactly one call site — inside `gainCustomUI`'s `customUi()`. Nobody batches parameter writes from `step()`. The host is a message-passing layer, not a memcpy, and eight writes in a row overruns whatever queue it has.

The fix — the "flexSeqSwitch" pattern from the SDK examples — is to keep runtime-mutable state in plain struct fields, not parameters. But randomization *needs* the parameter values to change (so the UI sliders reflect the new state). So the reconciliation is a drip-feed queue:

```cpp
// On the trigger edge: pre-roll all 8 values into private state.
static void queueRandomize(_fugueNT* p) {
    if (p->pendingPitchIdx >= 0) return;  // already in flight
    for (int s = 0; s < 8; s++) {
        p->pendingPitches[s] = rand01() * 1000;
    }
    p->pendingPitchIdx = 0;
}

// Drain one write per step() block.
if (p->pendingPitchIdx >= 0 && p->pendingPitchIdx < 8) {
    NT_setParameterFromAudio(algIdx,
        STEP_PITCH(p->pendingPitchIdx) + off,
        p->pendingPitches[p->pendingPitchIdx]);
    p->pendingPitchIdx++;
    if (p->pendingPitchIdx >= 8) p->pendingPitchIdx = -1;
}
```

Eight writes take eight audio blocks — about 11 ms at 64-sample blocks. Imperceptible to a human.

**Lesson:** one parameter write per `step()` block, no more. If you need to mutate many, queue them and drip.

### 3. Never mutate the parameter that just changed

There was a related crash in the "Randomize Now" UI action — a Off/On enum you flip to trigger a randomize. Naively: on the Off→On edge, do the randomize, then set the parameter back to Off so it re-arms. That second write — to the same parameter that just triggered `parameterChanged` — re-enters the parameter system and takes the host down.

The fix is dumber than it sounds: don't auto-reset. Rename the values to Off/On, and the user flips back manually. Two flips per randomize, but the second flip is basically free. In hindsight this is the pattern most sequencer-UI designs use anyway — you never see a physical button that de-presses itself while you're staring at it.

---

## ShiftNT — the boring one

Shift is a CV utility. Ports cleanly. Ring buffers, xorshift RNG, Schmitt triggers — pure integer/float math, no transcendentals. The only novelty was that Shift had gotten a set of upstream fixes in the VCV plugin *after* I started the port, and I had to bring them across:

- Step count is now 0–15 (0 = passthrough) rather than 1–16
- Parallel mode reads from a continuously-written history ring rather than an N-sized buffer, so LFO modulation of N no longer freezes the output
- Added a ÷5 to the per-lane clock divider

Doing the port second-hand from the diff (`git show 29194af`) was fast — the DSP is the same across both platforms, only the parameter registration is different.

---

## ChimeNT — the audio one

Chime is where the C++ platform earns its keep. 8 voices × 3 partials = 24 sine oscillators, plus envelopes, plus 8 tube-rotation LFOs, plus a stereo pan mix — all at 48 kHz. The DSP ported straight from the VCV source, one-for-one. Built clean. Copied to the SD card.

**Failed.** Again. But this time the failure screen showed some numbers:

```
ITC: 4240   DTC: 1776   DRAM: 1732
```

Those aren't limits. Those are the section sizes of my `.o` — `.text`, `.data.rel.ro`, `.rodata` respectively. The loader was reporting what my plugin needed. FugueNT's numbers are bigger and it works, so the issue wasn't size.

Running `arm-none-eabi-nm -u chimeNT.o`:

```
U cosf
U exp2f
U expf
U log2f
U memset
U NT_drawShapeI
U NT_drawText
U NT_globals
U powf
U sinf
```

The transcendental math functions. The NT firmware doesn't ship libm. `roundf`, `fabsf`, `floorf`, `sqrtf`, `fminf`, `fmaxf` all get inlined to VFP instructions by GCC — they never become library calls, so FugueNT and ShiftNT which only use those never noticed. But `sinf` and `expf` become real function calls that need libm to resolve. The loader can't, so it fails.

The fix: bring your own math. I dropped 90 lines of inline approximations at the top of the file. `sinApprox` is a 7th-degree minimax polynomial after range reduction to [-π, π]. `exp2Approx` biases the IEEE-754 exponent field for the integer part and does a polynomial for the fractional part. `log2Approx` reverses it. `powApprox` is `exp2Approx(y × log2Approx(x))`. Total code weight ~1 KB, accuracy well below what the audio path cares about (max error around 10⁻⁵ for sin, similar for the others).

While the transcendentals were on my mind, two other wins fell out:

- **Pan gains are compile-time constants** per voice — `cos(c/7 × π/2)` for the left, `sin(c/7 × π/2)` for the right, for c = 0…7. Precomputed as a static table; the 16 sin/cos calls per sample became two array reads.
- **Partial ring-decay multipliers** only depend on the block's decay-time parameter and the fixed per-partial ratio — constant across the block. Hoisted from 24 `expApprox` calls per sample to 3 per block.

**Lesson:** if the loader says "failed" on an audio module, `nm -u` your `.o` first. If you see anything outside the NT SDK plus `memset`, that's your problem. Reach for polynomial approximations before you touch newlib.

---

## Multi-plugin releases

By the time ChimeNT worked I had three plugins in one repo. The Disting NT community gallery ([nt-gallery.nosuch.dev](https://nt-gallery.nosuch.dev)) wants one gallery entry per plugin, and each entry points at a specific release asset URL. If I lumped all three `.o` files into a single "v1.0.0" release, the gallery would only find one.

The convention I followed is the one [`NerdRoger/disting_nt_plugins`](https://github.com/NerdRoger/disting_nt_plugins) uses:

- One repo, one folder per plugin
- Tags namespaced per plugin: `fugue-nt/v1.0.0`, `shift-nt/v1.0.0`, `chime-nt/v1.0.0`
- One release per tag with the plugin's `.o` attached
- One gallery entry per plugin, its `downloadUrl` pointing at the specific tag's asset

Same repo, three parallel version histories. Bumping ChimeNT to v1.0.1 does not perturb Fugue.

The whole flow — rebuild `.o` cleanly, make the tag, push it, create the release, upload the asset — sits in one script I can re-run every time:

```bash
#!/usr/bin/env bash
# scripts/release-plugin.sh
# Release a single plugin as a per-plugin GitHub release.
#
# Usage: scripts/release-plugin.sh <plugin-slug> <version> [commit-ish]
#   plugin-slug   e.g. "fugue-nt". Maps to modules/{camelSlug}NT/.
#   version       Semver without leading 'v', e.g. "1.0.0".
#   commit-ish    Optional git ref for the tag (default: HEAD).

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <plugin-slug> <version> [commit-ish]" >&2
    exit 2
fi

SLUG="$1"
VERSION="$2"
COMMIT="${3:-HEAD}"

# "fugue-nt" → "fugueNT"
MODULE_DIR="$(
    python3 -c "
import sys
parts = sys.argv[1].split('-')
out = parts[0] + ''.join(w[:1].upper() + w[1:] for w in parts[1:])
if out.endswith('Nt'): out = out[:-2] + 'NT'
print(out)
" "$SLUG"
)"

MODULE_PATH="modules/${MODULE_DIR}"
OBJ="${MODULE_PATH}/plugins/${MODULE_DIR}.o"
TAG="${SLUG}/v${VERSION}"

# Rebuild clean so the asset is deterministic from source.
(cd "$MODULE_PATH" && rm -rf plugins && make >/dev/null)

# Tag (if it doesn't already exist).
if git rev-parse -q --verify "refs/tags/${TAG}" >/dev/null; then
    echo "==> Tag ${TAG} already exists — reusing"
else
    git tag -a "$TAG" "$COMMIT" -m "${MODULE_DIR} v${VERSION}"
    git push origin "$TAG"
fi

# Create or update the release.
if gh release view "$TAG" >/dev/null 2>&1; then
    gh release upload "$TAG" "$OBJ" --clobber
else
    gh release create "$TAG" "$OBJ" \
        --title "${MODULE_DIR} v${VERSION}" \
        --notes "${MODULE_DIR} v${VERSION}. Copy the .o to programs/plug-ins/ on your Disting NT SD card."
fi

echo "==> https://github.com/stuart78/SignalFunctionSet-DistingNT/releases/tag/${TAG}"
```

Cutting a new release is one line:

```bash
scripts/release-plugin.sh chime-nt 1.0.1
```

The script is idempotent — if a tag already exists, it just replaces the asset. That matters when you push a release, catch a bug on-device an hour later, and need to reissue the same version.

The gallery submission itself is manual: email the maintainer a JSON entry describing the plugin. FugueNT was already verified in the gallery by the time I was finishing ChimeNT; ShiftNT and ChimeNT are pending submission at time of writing. Full JSON entries and the submission template are in [`docs/gallery-submission.md`](https://github.com/stuart78/SignalFunctionSet-DistingNT/blob/main/docs/gallery-submission.md) in the repo.

---

## What ports and what doesn't

The obvious rule: anything that already ran at CV rates in VCV ports easily. Anything that ran at audio rate ports, but you're rewriting the transcendentals.

The less obvious rule: display work. The NT is 256×64 monochrome with 16 grey levels; a rich VCV widget with layered SVG and drag interactions has to be reimagined as a schematic. ChimeNT's original VCV display renders each voice as a rotating 3D tube seen from above, with a bore that opens toward you at centre — beautiful, meaningless as monochrome pixels. The NT version is a pendulum swinging left/right, with a dot at the tip that brightens near centre. Same information, one twentieth the visual language. That's a shift in taste as much as technique.

Modules that need per-sample audio I/O (samplers, granular, effects reading incoming audio) also port fine because the C++ SDK gives you direct bus access — a `float*` per bus, per block, both directions. `SamplePlayer` and `SampleStreamer` in the SDK examples show the pattern, including SD-card WAV loading via `NT_readSampleFrames`. That's the next batch — GSX (granular) and Phase (dual sample looper) are on the list.

The one thing I've hit that genuinely doesn't port is VCV's polyphonic cables. NT is one channel per bus, so a "polyphonic V/Oct" output becomes eight separate V/Oct outputs (or you skip it). ChimeNT dropped that feature, since it also has per-voice audio outs that carry the same information.

Modular is small on both sides — a Rack window and a rack row are the same *thing*, more or less. But there's something specific about a physical face that a computer window can't do: you touch three knobs at once, you spin something with your left hand while patching with your right, you commit to a preset because you can't easily hold four alternatives in RAM. Porting to hardware is worth the effort just to notice that.

Full source, releases, and per-module documentation: [github.com/stuart78/SignalFunctionSet-DistingNT](https://github.com/stuart78/SignalFunctionSet-DistingNT).
