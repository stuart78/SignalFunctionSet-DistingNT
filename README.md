# Signal Function Set — Disting NT

Ports of selected [Signal Function Set](https://github.com/stuart78/SignalFunctionSet) VCV Rack modules to the [Expert Sleepers Disting NT](https://www.expert-sleepers.co.uk/distingNT.html) hardware module.

Built against the [distingNT C++ API](https://github.com/expertsleepersltd/distingNT_API) (included here as a submodule).

## Modules

| Module | Status | Release | Source | Docs |
|---|---|---|---|---|
| FugueNT | Released, in gallery | [`fugue-nt/v1.0.0`](https://github.com/stuart78/SignalFunctionSet-DistingNT/releases/tag/fugue-nt%2Fv1.0.0) | [modules/fugueNT](modules/fugueNT) | [docs/fugue-nt.md](docs/fugue-nt.md) |
| ShiftNT | Released, submission pending | [`shift-nt/v1.0.0`](https://github.com/stuart78/SignalFunctionSet-DistingNT/releases/tag/shift-nt%2Fv1.0.0) | [modules/shiftNT](modules/shiftNT) | [docs/shift-nt.md](docs/shift-nt.md) |
| ChimeNT | Released, submission pending | [`chime-nt/v1.0.0`](https://github.com/stuart78/SignalFunctionSet-DistingNT/releases/tag/chime-nt%2Fv1.0.0) | [modules/chimeNT](modules/chimeNT) | [docs/chime-nt.md](docs/chime-nt.md) |

Each plugin is released under a namespaced tag `{plugin-slug}/v{version}` with a single `.o` file attached — matches the layout used by [`NerdRoger/disting_nt_plugins`](https://github.com/NerdRoger/disting_nt_plugins). See [docs/gallery-submission.md](docs/gallery-submission.md) for the [nt_helper gallery](https://nt-gallery.nosuch.dev/) registration flow and `scripts/release-plugin.sh` for the release helper.

## Cloning

```sh
git clone --recurse-submodules https://github.com/stuart78/SignalFunctionSet-DistingNT.git
```

If you already cloned without submodules:

```sh
git submodule update --init --recursive
```

## Building

Requires `arm-none-eabi-c++` (GCC for ARM Cortex-M). Per-module:

```sh
cd modules/fugueNT
make
```

Output is `plugins/fugueNT.o`. Copy to the Disting NT MicroSD card per the module documentation.

## License

See [LICENSE](LICENSE).
