# NativeNavi

Play your own copy of **The Legend of Zelda: Ocarina of Time** (Nintendo Switch 2) on an Apple silicon Mac, natively. It is not an emulator: the game's own code runs on the Apple chip, and the console's graphics are rebuilt on Metal.

Nothing from the game is in this repository: you build the Mac app from your own copy. The project is not affiliated with Nintendo.

[Features](#features) · [Requirements](#requirements) · [How to play](#how-to-play) · [Reporting a problem](#reporting-a-problem) · [How it works](#how-it-works)

## Features

- **The game as the console runs it.** 60 fps at the game's own resolution, with versions **1.1.1** and **1.0.0**. A trick or a timing that differs from a Switch 2 is a bug ([report it](https://github.com/SailorSnoW/nativenavi/issues/new?template=2-console-parity.yml)).
- **Practice tools.** [sloperinaGZ](https://github.com/PracticeROM/sloperina), the practice menu runners use on console and emulators, is built in: warps, watches, memfiles, frame advance, free camera, no void outs and more. It stays off until you turn it on in the settings window.
- **Instant reset.** ⌘R takes the game back to its first screen in under a second, as closing and reopening it does on a Switch.
- **Controllers.** Switch Pro Controller, Joy-Con, Joy-Con 2 and Pro Controller 2, Xbox, PlayStation, 8BitDo and other Bluetooth pads, with rumble and motion controls.
- **Few stutters.** The game's shaders are prepared when the app is built, not while you play.
- **And more.** Docked or handheld picture, window or fullscreen, the ocarina played with a microphone, and mods made for Switch emulators.

**Status:** playable, but not every area has been played on a Mac yet. The heaviest areas can drop below 60 fps: Kakariko Village runs at about 37 fps on an M2 Pro. Known issues are in [the game's note](dm/games/0400486024DEC000/README.md#open).

## Requirements

- An Apple silicon Mac with macOS 26.6 or later. 16 GB of memory is recommended: some areas use over 10 GB.
- [Xcode](https://developer.apple.com/download/all/?q=Xcode%2027.2) 27.2, free with an Apple Account. The game's shaders are built with its Metal compiler.
- Your own copy of the game as a `.nup` file ([NUP](https://github.com/a55uka/nup)), holding 1.0.0, 1.1.1 or both.
- About 80 GB of free space for the first build. The finished app takes about 37 GB.
- An internet connection for the first build.

## How to play

### With the NativeNavi app

NativeNavi builds the game for you: it gets the tools it needs, checks your file and shows each step. Make it once, in Terminal:

```
sudo xcode-select -s /Applications/Xcode.app      # Xcode 27.2
git clone https://github.com/SailorSnoW/nativenavi.git
cd nativenavi
python3 scripts/build_gui.py
open dist/NativeNavi.app
```

Then drop your `.nup` file on its window, choose the version and click **Build**. The first build takes a few hours, most of it the game's shaders; later updates redo only what changed. More: [The NativeNavi app](docs/gui.md).

### In Terminal

Needs a clone of this repository (above) and Python 3.12 or newer (arm64). Install the tools once:

```
sudo xcode-select -s /Applications/Xcode.app      # Xcode 27.2
xcodebuild -downloadComponent MetalToolchain
python3 -m pip install lz4 "capstone>=5,<6" fonttools zbic "backports.zstd; python_version < '3.14'"
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

Then build, in the clone:

```
python3 scripts/build_app.py ~/Games/OcarinaOfTime.nup          # 1.1.1
python3 scripts/build_app.py ~/Games/OcarinaOfTime.nup -p v0    # 1.0.0
```

The app appears beside the `.nup` file. One version is installed at a time: building the other replaces it. All options: [the packing script](docs/development.md#the-packing-script-in-detail).

### Playing

Open the game's app. A settings window comes first: language, frame rate, controls, practice tools ([Settings](docs/settings.md)). Controls, saves and reset: [Playing](docs/playing.md). A save from an nxnative build of the game is carried over at the first start.

## Reporting a problem

[Open an issue](https://github.com/SailorSnoW/nativenavi/issues/new/choose): a bug, a difference from the console, or a build that stops. Attach the log, `~/Library/Logs/NativeNavi/0400486024DEC000.log`. Never attach game files. Questions go to [Discussions](https://github.com/SailorSnoW/nativenavi/discussions).

## How it works

The game's Arm code runs directly on the Mac. A rewriter changes only the instructions macOS does not allow, a host answers the console's system calls and services, and NVN, the Switch 2's graphics API, is rebuilt on Metal, with the game's shaders translated when the app is built.

This is a fork of [nxnative](https://github.com/seedds/nxnative), which runs many Switch games on Macs, iPhones and iPads, reduced to this one game on the Mac.

For research and development: [the game's note](dm/games/0400486024DEC000/README.md) (its passes, measurements and open questions) · [How it works](docs/development.md) · [Reference](docs/reference/README.md) · [Front ends](docs/frontends.md) · [Contributing](CONTRIBUTING.md) · [Notes for agents](AGENTS.md)

## Acknowledgements

Built on other people's work ([licenses](dm/third_party/README.md)):

- **[nxnative](https://github.com/seedds/nxnative)** (MIT) and its contributors: the host, the graphics layer, the shader translation and almost every finding in the reference.
- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT), where this way of running a game natively began.
- **Ryujinx** (MIT): its shader translator, and its emulation as a reference.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: the reference for the console's kernel.
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)** and **[dav1d](https://code.videolan.org/videolan/dav1d)** (BSD 2-Clause): shaders for Metal, and the game's movies on Macs without a hardware AV1 decoder.
- **[sloperinaGZ](https://github.com/PracticeROM/sloperina)** (PracticeROM, GPL-3.0): the practice tools, downloaded from its official release when the app is built, not stored here.
- **[MacNativeGames](https://github.com/ly05010419/MacNativeGames)** (MIT): two GPU techniques.

## License

MIT ([LICENSE](LICENSE)). Third-party code keeps its own license ([dm/third_party](dm/third_party/README.md)).
