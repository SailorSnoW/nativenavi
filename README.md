# NativeNavi

Play your own copy of The Legend of Zelda: Ocarina of Time (Nintendo Switch 2) natively on an Apple silicon Mac. One script packs the game into a Mac app.

It is not an emulator: the game's own code runs directly on the Apple chip, with the Switch's graphics API rebuilt on Metal. Nothing from the game is in this repository, and the project is not affiliated with Nintendo.

This is a hard fork of [nxnative](https://github.com/seedds/nxnative), which runs many Switch games on Macs, iPhones and iPads. Here everything that did not serve this one game on a Mac was removed, and what remains is tuned for it.

[Features](#features) · [The game](#the-game) · [Requirements](#requirements) · [How to play](#how-to-play) · [For developers](#for-developers)

## Features

### From nxnative

- **Native code, not emulation.** The game's own Arm code runs on the Apple chip. A rewriter changes only the instructions macOS does not allow, and a host answers the Switch's kernel calls and system services: threads, memory, files with transactional saves, audio, and the AV1 movies (decoded in hardware on an M3 or later, by dav1d on an M1 or M2).
- **The Switch 2's graphics on Metal.** NVN, the console's graphics API, is rebuilt on Metal. The game's shaders are translated ahead of time, when the app is built, so none is translated while you play.
- **Few stutters.** Metal 4 pipelines built from a shared base, a pipeline archive that comes with the app and grows on the Mac, a stand-in prebuilt for every pair of the game's shaders, and a prewarm list of what the game is known to draw, prepared before it starts.
- **Frame pacing.** 30 or 60 fps, at the game's real speed even when frames run long. In heavy scenes the app can hold an even 30 instead of a mix of 30 and 60, and it turns the game's graphics commands into Metal's on a thread of its own.
- **Picture settings.** Docked or handheld resolution, a render scale down to 0.5, the game's own dynamic resolution, Apple's MetalFX in place of the game's 3D upscaler (with a sharpness setting) or to scale the finished picture to the screen, and window or fullscreen.
- **Controllers.** Anything Apple's controller support lists (Xbox, PlayStation, Switch Pro Controller), other Bluetooth gamepads such as 8BitDo read as raw HID, Joy-Con 2 and Pro Controller 2 over Bluetooth LE, and original Joy-Cons. Rumble (HD rumble on a Switch Pro Controller), motion controls, and a Home-button hold to quit.
- **The ocarina by microphone.** Turn on Microphone and use the game's Ocarina Mic Input setting to hum or play an instrument.
- **A settings window before the game**, with the defaults the game was tested with. Lines in an `env.txt` override any setting.
- **Mods and amiibo.** Mods made for Switch emulators (RomFS files, IPS and pchtxt patches) load from a mods folder and turn on and off from the game window's Mods menu. Amiibo are scanned from dumps of their tags.

### Added here for Ocarina of Time

- **Two versions of the game:** 1.1.1 and 1.0.0, built the same way, in the NativeNavi app as in Terminal ([The game](#the-game)). Version 1.0.0 has its own fixes: its frame step and movies found by their functions, the black 3D picture at its lowest dynamic resolution fixed, and its prewarm list carried over from 1.1.1's.
- **[The NativeNavi app](docs/gui.md)** builds the game without Terminal. Drop the `.nup` file on it and click Build. It checks the file against the dumps the game was built and played from, gets Python and Rust, and shows each step, the time left and where the app is.
- **[Practice tools](dm/games/0400486024DEC000/README.md#practice-tools):** [sloperinaGZ](https://github.com/PracticeROM/sloperina), the practice menu runners use for this game on consoles and emulators (warps, watches, health, levitation, frame advance…), loaded as a modded Switch loads it. It works on both versions and stays off until you turn it on in the settings window.
- **[Reset with ⌘R](docs/playing.md):** the game starts again from its first screen inside the running app, in under a second, with no settings window and no shader preparation.
- **A faster 3D upscaler:** the game's own upscaling network runs on Apple's SIMD-group matrices, in two passes, with the same picture. On a 16 GB M2 Pro, a walk through Kakariko Village, the heaviest area played so far, went from 33.1 to 36.9 fps.
- **A longer prewarm list:** 18,649 more rows recorded in a Mac's own play, so a first build prepares the pipelines that Mac otherwise built while playing.
- **Saves carried over:** a save and settings made with an nxnative build of the game are copied over at the first start.
- **Lean:** one game, on the Mac only. The host, the NVN layer and the shader translator keep only what this game uses.

## The game

[The Legend of Zelda: Ocarina of Time](dm/games/0400486024DEC000/README.md) (Switch 2), in two versions:

- **1.1.1** (update v131072), the default: the version the game was brought up, measured and played on.
- **1.0.0** (the base game, v0), with the same settings and the same practice tools. About 2,300 of the prewarm list's rows, the effects', are not carried over to it yet, so those effects are compiled the first time they appear.

Both are built the same way, from a NUP that holds the base game and its update: in the NativeNavi app, choose the version under **Version**; in Terminal, the newest is built, and `-p v0` builds 1.0.0. The game's app is one version at a time: building the other replaces it.

The game's note records the tested builds, the measurements, the progress and the known issues.

## Requirements

- An Apple silicon Mac with macOS 26.6 or later.
- [Xcode](https://developer.apple.com/download/all/?q=Xcode%2027.2) 27.2 and Python 3.12 or newer (an arm64 one).
- Rust, through [rustup](https://rustup.rs): part of the Mac host is written in Rust. The first build downloads the Rust version the host is built with.
- Your own copy of the game as a [NUP](https://github.com/a55uka/nup) container (a `.nup` file; the one this was built from is 25.6 GB).
- Disk space: on the Mac this was written on, the build folder takes about 70 GB and the finished app about 37 GB.

The game's movies are AV1. A Mac with an M3 chip or later decodes them in hardware; an M1 or M2 decodes them in software, which an M2 Pro does at the movies' full 60 pictures a second.

## How to play

**Without Terminal**: [the NativeNavi app](docs/gui.md) does the steps below for you: drop your `.nup` file on it and click Build. It downloads Python and Rust into a folder of its own, and needs Xcode 27.2 installed.

**In Terminal**:

**1. Install the tools** (only once), in Terminal. The shaders are built with the Metal compiler of Xcode 27.2: select it first with `sudo xcode-select -s /Applications/Xcode.app` (with several versions installed, name this one `Xcode-27.2.app` and select that).

```
xcodebuild -downloadComponent MetalToolchain
python3 -m pip install lz4 "capstone>=5,<6" fonttools zbic "backports.zstd; python_version < '3.14'"
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

The last line installs rustup, Rust's installer: answer its question with the default, then open a new Terminal window so that its tools are found.

**2. Build**, with the `.nup` file or the folder that holds it:

```
python3 scripts/build_app.py ~/Games/OcarinaOfTime.nup          # 1.1.1
python3 scripts/build_app.py ~/Games/OcarinaOfTime.nup -p v0    # 1.0.0
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
- **[sloperinaGZ](https://github.com/PracticeROM/sloperina)** (PracticeROM, GPL-3.0): the practice tools. It is not in this repository: the packing script downloads its official release, pinned by the release's SHA-256 ([`practice.tsv`](dm/games/0400486024DEC000/practice.tsv)), and puts it in the app with its license, unless `--no-practice`.
- **[MacNativeGames](https://github.com/ly05010419/MacNativeGames)** (MIT): the GPU widening of 8-bit indices (`dm/gfx/nvn/nvn_indirect.inc`) and the in-pass clears (`dm/gfx/nvn/nvn_exec.inc`).

## License

MIT ([LICENSE](LICENSE)). Third-party code keeps its own license ([dm/third_party](dm/third_party/README.md)).
