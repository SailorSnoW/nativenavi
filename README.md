# NativeNavi

Play your own copy of The Legend of Zelda: Ocarina of Time (Nintendo Switch 2) natively on an Apple silicon Mac. One script packs the game into a Mac app.

It is not an emulator: the game's own code runs directly on the Apple chip, with the Switch's graphics API rebuilt on Metal. Nothing from the game is in this repository, and the project is not affiliated with Nintendo.

This is a hard fork of [nxnative](https://github.com/seedds/nxnative), which runs many Switch games on Macs, iPhones and iPads. Here everything that did not serve this one game on a Mac was removed, and what remains is tuned for it.

[The game](#the-game) · [Requirements](#requirements) · [How to play](#how-to-play) · [For developers](#for-developers)

## The game

[The Legend of Zelda: Ocarina of Time](dm/games/0400486024DEC000/README.md) (Switch 2, update v131072). Its note records the tested build, the measurements, the progress and the known issues.

## Requirements

- An Apple silicon Mac with macOS 26.6 or later.
- [Xcode](https://developer.apple.com/download/all/?q=Xcode%2027.2) 27.2 and Python 3.12 or newer (an arm64 one).
- Rust, through [rustup](https://rustup.rs): part of the Mac host is written in Rust. The first build downloads the Rust version the host is built with.
- Your own copy of the game as a [NUP](https://github.com/a55uka/nup) container (a `.nup` file; the one this was built from is 25.6 GB).
- Disk space: on the Mac this was written on, the build folder takes about 70 GB and the finished app about 37 GB.

The game's movies are AV1. A Mac with an M3 chip or later decodes them in hardware; an M1 or M2 decodes them in software, which an M2 Pro does at the movies' full 60 pictures a second.

## How to play

**1. Install the tools** (only once), in Terminal. The shaders are built with the Metal compiler of Xcode 27.2: select it first with `sudo xcode-select -s /Applications/Xcode.app` (with several versions installed, name this one `Xcode-27.2.app` and select that).

```
xcodebuild -downloadComponent MetalToolchain
python3 -m pip install lz4 "capstone>=5,<6" fonttools zbic "backports.zstd; python_version < '3.14'"
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

The last line installs rustup, Rust's installer: answer its question with the default, then open a new Terminal window so that its tools are found.

**2. Build**, with the `.nup` file or the folder that holds it:

```
python3 scripts/build_app.py ~/Games/OcarinaOfTime.nup
```

The script unpacks the game into `0400486024DEC000_build` beside the NUP, rewrites its code for macOS, translates and compiles its shaders (about 42,850, the long part of a first build), and packs the app, which appears beside the build folder. Building again reuses everything that did not change. All options: [The packing script in detail](docs/development.md#the-packing-script-in-detail).

**3. Play**: open the app from Finder. Controls, saves and settings: [Playing](docs/playing.md) and [Settings](docs/settings.md). The log of a run is `~/Library/Logs/NativeNavi/0400486024DEC000.log`.

## For developers

[How it works](docs/development.md) · [The game's note](dm/games/0400486024DEC000/README.md) · [Notes for agents](AGENTS.md) · [Working guides](docs/agents/README.md) · [Reference](docs/reference/README.md) · [Front ends](docs/frontends.md) · [Contributing](CONTRIBUTING.md)

`python3 scripts/build_app.py <nup> --platform mac` makes a development build instead of an app: the shaders, the host, and the command that runs the game from the build folder.

## Acknowledgements

This project is built on other people's work ([licenses](dm/third_party/README.md)):

- **[nxnative](https://github.com/seedds/nxnative)** (MIT): this repository is a fork of it, reduced to one game. The host, the NVN layer, the shader chain, the rewriter and almost every finding in the reference come from it and its contributors.
- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this native-execution approach, for Diablo II: Resurrected; nxnative started as a copy of its generic parts.
- **Ryujinx** (MIT): its shader translator (`Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator` 1.3.2, in `dm/third_party/ryujinx` with the changes listed in [its README](dm/third_party/ryujinx/README.md)) turns the game's shaders into SPIR-V. Its emulation of the kernel and system services is also a reference when the host's behaviour is in doubt.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: its kernel, mesosphère, is the reference for Horizon's synchronization semantics (address arbiter, condition variables).
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)**: SPIR-V to Metal Shading Language.
- **[dav1d](https://code.videolan.org/videolan/dav1d)** (BSD 2-Clause): decodes the game's AV1 movies on Macs without a hardware AV1 decoder.
- **[MacNativeGames](https://github.com/ly05010419/MacNativeGames)** (MIT): the GPU widening of 8-bit indices and in-pass clears in `dm/gfx/nvn/nvn_mtl.m`, and the visibility-counter lowering in the shader translator.

## License

MIT ([LICENSE](LICENSE)). Third-party code keeps its own license ([dm/third_party](dm/third_party/README.md)).
