# nt_helper Gallery Submission

The [Disting NT Community Gallery](https://nt-gallery.nosuch.dev/) (used by [nt_helper](https://github.com/thorinside/nt_helper)) lists plugins by pointing at a specific release asset URL. For a multi-plugin repo like this one, each plugin appears as its own gallery entry, each pointing at its own per-plugin tag.

The layout matches [`NerdRoger/disting_nt_plugins`](https://github.com/NerdRoger/disting_nt_plugins) — one repo, one `modules/` folder per plugin, tags namespaced as `{plugin-slug}/v{version}`, one `.o` file per release.

## Cutting a release

`scripts/release-plugin.sh` handles the whole flow. From the repo root:

```bash
scripts/release-plugin.sh <plugin-slug> <version>
# e.g.
scripts/release-plugin.sh chime-nt 1.0.1
```

It rebuilds the `.o` cleanly, creates the `{slug}/v{version}` tag, pushes it, and creates the GitHub release with the `.o` attached. Idempotent — safe to re-run.

## Registering a plugin

The live gallery data is at <https://nt-gallery.nosuch.dev/api/gallery.json>, maintained by Thorinside (email `thorinside@gmail.com`). Submissions are out-of-band — send the JSON entry(ies) directly. Schema is `docs/plugin_gallery_schema.json` in the [nt_helper](https://github.com/thorinside/nt_helper) repo.

### Current status

| Plugin | Gallery status | Release tag |
|---|---|---|
| FugueNT | Registered, verified | `v1.0.0` (the umbrella tag; equivalent per-plugin tag `fugue-nt/v1.0.0` also exists) |
| ShiftNT | **Needs submission** | `shift-nt/v1.0.0` |
| ChimeNT | **Needs submission** | `chime-nt/v1.0.0` |

### Submission JSON — ShiftNT

```json
{
  "id": "shiftnt",
  "name": "ShiftNT",
  "author": "stuart78",
  "category": "utilities",
  "type": "cpp",
  "featured": false,
  "verified": false,
  "isCollection": false,
  "guid": "SFsN",
  "description": "4-output CV shift register with per-output controls. Each lane has its own step count, clock divider, and Parallel/Cascade mode. Includes a Jumble output that randomly picks one lane's held value on each clock.",
  "longDescription": "Port of Signal Function Set's Shift module. Four independent lanes read from a shared CV input. In Parallel mode each lane is an N-step delay line; in Cascade mode each lane is a tape loop reading at lane-clock rate and writing when its parent fires. Per-lane clock divider (/1, /2, /3, /4, /5, /8), global N CV, per-lane Step CV, and a Jumble S&H+gate output. N=0 is a clean passthrough. Delay reads from a continuously-written history ring so the output stays correct under Step CV modulation.",
  "tags": ["cv", "shift-register", "utility", "delay", "sample-hold"],
  "repository": {
    "owner": "stuart78",
    "name": "SignalFunctionSet-DistingNT",
    "url": "https://github.com/stuart78/SignalFunctionSet-DistingNT",
    "branch": "main"
  },
  "releases": {
    "latest": "shift-nt/v1.0.0",
    "stable": "shift-nt/v1.0.0"
  },
  "installation": {
    "assetPattern": ".*\\.zip$",
    "extractPattern": ".*\\.o$",
    "downloadUrl": "https://github.com/stuart78/SignalFunctionSet-DistingNT/releases/download/shift-nt/v1.0.0/shiftNT.o",
    "targetPath": "/programs/plug-ins/stuart78/",
    "subdirectory": null
  },
  "compatibility": {
    "minFirmwareVersion": "1.15.0",
    "requiredFeatures": []
  },
  "documentation": {
    "readme": "https://raw.githubusercontent.com/stuart78/SignalFunctionSet-DistingNT/main/docs/shift-nt.md",
    "manual": null
  }
}
```

### Submission JSON — ChimeNT

```json
{
  "id": "chimentnt",
  "name": "ChimeNT",
  "author": "stuart78",
  "category": "generators",
  "type": "cpp",
  "featured": false,
  "verified": false,
  "isCollection": false,
  "guid": "SFcN",
  "description": "8-note resonating drone machine. Each voice's tube LFO blooms amplitude as it rotates through centre; centre crossings can strike the bar. Bow to Strike blend, four relate modes for how the voices' rotations relate.",
  "longDescription": "Port of Signal Function Set's Chime module. Xylophone-tube-rotation metaphor: each voice has a bidirectional LFO whose centre proximity blooms the amplitude, and centre crossings can strike the bar (ringing three inharmonic partials that decay away as the tube turns). Bow to Strike interpolates between continuous drone and struck-and-decay hits. Four Relate modes (Ramp, Stepped, Random, Ripple) shape how the eight rotations relate; per-voice weight, atten, and scale degree; global root/scale/octave with the same 19 scales as FugueNT. Clock sync locks rotations to musical divisions (32/16/8/4/2/1 clocks per rotation).",
  "tags": ["drone", "resonator", "generative", "polyphonic", "scale", "clock-sync"],
  "repository": {
    "owner": "stuart78",
    "name": "SignalFunctionSet-DistingNT",
    "url": "https://github.com/stuart78/SignalFunctionSet-DistingNT",
    "branch": "main"
  },
  "releases": {
    "latest": "chime-nt/v1.0.0",
    "stable": "chime-nt/v1.0.0"
  },
  "installation": {
    "assetPattern": ".*\\.zip$",
    "extractPattern": ".*\\.o$",
    "downloadUrl": "https://github.com/stuart78/SignalFunctionSet-DistingNT/releases/download/chime-nt/v1.0.0/chimeNT.o",
    "targetPath": "/programs/plug-ins/stuart78/",
    "subdirectory": null
  },
  "compatibility": {
    "minFirmwareVersion": "1.15.0",
    "requiredFeatures": []
  },
  "documentation": {
    "readme": "https://raw.githubusercontent.com/stuart78/SignalFunctionSet-DistingNT/main/docs/chime-nt.md",
    "manual": null
  }
}
```

## Suggested email to Thorinside

> Subject: Two new plugins for the disting NT gallery (SignalFunctionSet-DistingNT)
>
> Hi Thorinside,
>
> I've added two more plugins to my `SignalFunctionSet-DistingNT` repo alongside the existing FugueNT (which you already have listed as `fuguent`). Both are tagged, uploaded, and follow the same conventions.
>
> - **ShiftNT** — 4-output CV shift register, utilities. GUID `SFsN`. Release: `shift-nt/v1.0.0`.
> - **ChimeNT** — 8-voice resonating drone, generators. GUID `SFcN`. Release: `chime-nt/v1.0.0`.
>
> Submission JSON for both: <https://github.com/stuart78/SignalFunctionSet-DistingNT/blob/main/docs/gallery-submission.md>
>
> Thanks!

## Notes

- All three plugins compile clean against `kNT_apiVersionCurrent = 13`.
- GUIDs `SFfN`, `SFsN`, `SFcN` — all mixed-case, so they don't clash with all-lowercase built-in reservations.
- Each release attaches a single `.o`; no zip wrapping. The `assetPattern: ".*\\.zip$"` in the JSON is convention (matches the existing FugueNT entry and every other single-`.o` plugin in the gallery); the `downloadUrl` is what actually gets fetched.
- `minFirmwareVersion` set to `1.15.0` to match FugueNT's existing entry.
