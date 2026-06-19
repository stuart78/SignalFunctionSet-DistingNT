# Signal Function Set — Disting NT

Ports of selected [Signal Function Set](https://github.com/stuart78/SignalFunctionSet) VCV Rack modules to the [Expert Sleepers Disting NT](https://www.expert-sleepers.co.uk/distingNT.html) hardware module.

Built against the [distingNT C++ API](https://github.com/expertsleepersltd/distingNT_API) (included here as a submodule).

## Modules

| Module | Status | Source | Docs |
|---|---|---|---|
| FugueNT | Released v1.0.0 | [modules/fugueNT](modules/fugueNT) | [docs/fugue-nt.md](docs/fugue-nt.md) |
| ShiftNT | In development | [modules/shiftNT](modules/shiftNT) | [docs/shift-nt.md](docs/shift-nt.md) |

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
