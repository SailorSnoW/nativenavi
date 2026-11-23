# The NativeNavi app

NativeNavi is a Mac app for players who do not use Terminal. It takes your copy of the game, gets the tools the build needs, builds the game's Mac app and shows you where it is. It runs [the packing script](development.md#the-packing-script-in-detail) for you, so the result is the same app as `python3 scripts/build_app.py` makes.

- [Requirements](#requirements)
- [Building the game](#building-the-game)
- [The game files it accepts](#the-game-files-it-accepts)
- [What it installs, and where](#what-it-installs-and-where)
- [Updating](#updating)
- [When a build stops](#when-a-build-stops)
- [Removing it](#removing-it)
- [Packing the app](#packing-the-app)
- [How it drives the build](#how-it-drives-the-build)

## Requirements

- An Apple silicon Mac with macOS 26.6 or later.
- [Xcode 27.2](https://developer.apple.com/download/all/?q=Xcode%2027.2), free with an Apple Account. The game's shaders were tested with this Xcode's Metal compiler, and NativeNavi builds with no other one. It finds Xcode 27.2 wherever it is (a newer Xcode can stay installed beside it, for example as `Xcode-27.2.app`); it does not change which Xcode the Mac selects.
- Your own copy of the game as a NUP container (a `.nup` file), one of [the versions it accepts](#the-game-files-it-accepts).
- About 80 GB of free space for a first build (the build files and the app, on the same drive), less for a later one.
- An internet connection for the first build: Python, Rust and Apple's Metal toolchain are downloaded.

## Building the game

1. Open NativeNavi. Drop the `.nup` file on its window (or on its Dock icon), or click **Choose File…**. It checks the file first ([below](#the-game-files-it-accepts)): a few seconds to a minute, longer when the file is in a cloud drive and has to be downloaded.
2. **What the Build Needs** lists the rest. A green check is ready; a blue arrow is downloaded when the build starts; an orange mark needs you: **Download…** opens Apple's page for Xcode 27.2, and **Open Xcode** is for a new Xcode that still has to accept its license and install its components (open it once, then come back: the list updates when NativeNavi is in front again).
3. **Put the game's app in** is Applications by default (your home folder's Applications when you cannot write to the main one).
4. Click **Build**. The first build takes a few hours, most of it the game's shaders; the window shows each step, the current one's progress and an estimate of the time left, and the Dock icon shows the step's progress. The Mac does not go to sleep while it builds. You can close the window: the build goes on, and a notification says when it is done. Quitting asks first, then stops the build.
5. When it is done, **Play** opens the game, and **Show in Finder** shows its app. Before the game starts, the game's own window lets you choose its language, controls and other settings ([Settings](settings.md)); saves and logs are where [Playing](playing.md) says.

A build that stops, or that you stop, picks up where it was at the next **Build Again**: everything finished before is reused.

## The game files it accepts

NativeNavi builds only from the NUP containers the game's profile lists in [`nup.tsv`](../dm/games/0400486024DEC000/nup.tsv), by SHA-256 and size: the dumps the game was built and played from. It hashes the file you choose and refuses any other one, with an alert that says which versions it accepts and a notification when it was in the background. A file of another size is refused at once; one of a listed size is read in full. A file it has hashed is not read again while its path, size, modification time and inode stay the same.

The command line has no such rule: `scripts/build_app.py` builds other versions of the game too, from a profile it derives for them ([The packing script in detail](development.md#the-packing-script-in-detail)).

## What it installs, and where

Nothing goes outside these folders but Apple's Metal toolchain, which macOS keeps for the selected Xcode as a system asset. NativeNavi asks for no administrator password, changes no shell profile and leaves `xcode-select` alone: each tool it runs gets Xcode 27.2 through `DEVELOPER_DIR`.

| Folder | What is in it |
|---|---|
| `~/Library/Developer/NativeNavi/Source` | the build scripts, unpacked from the app (the repository at the commit the app was packed from) |
| `~/Library/Developer/NativeNavi/Tools` | Python ([python-build-standalone](https://github.com/astral-sh/python-build-standalone)) with the packages in `.github/requirements/tools.txt`, and Rust ([rustup](https://rustup.rs), then the version `dm/host/rust/rust-toolchain.toml` names) |
| `~/Library/Developer/NativeNavi/Build` | the build files (the packing script's build folder, about 70 GB); Settings can move them to another drive, into a folder `nativenavi-build` there |
| `~/Library/Logs/NativeNavi/NativeNavi.log` | NativeNavi's log (every tool it ran, with its output; the previous session's is `NativeNavi.prev.log`), beside the game's own logs; the build's own logs are in the build files' `logs/` |
| Applications | the game's app |

The downloads are pinned by their SHA-256 in [`dm/gui/setup.json`](../dm/gui/setup.json), and a download with other bytes is refused. The folders under `~/Library/Developer/NativeNavi` are kept out of Time Machine. None of their paths has a space, which the host's Makefiles cannot take.

## Updating

A new version of NativeNavi carries a newer copy of this repository. Its first screen then offers **Update**: the build runs again from the build files, so it redoes only what changed (a change of the shader translation chain translates every shader again, and that takes as long as a first build).

## When a build stops

The window shows why: the packing script's last `[RED]` line. **Show Log** opens the build's logs; File › Show NativeNavi Log opens NativeNavi's own.

- **Not enough space**: the packing script checks the free space before it unpacks the game. Free up space, or move the build files to another drive in Settings (that build starts from the beginning).
- **Operation not permitted** when the app is replaced: macOS's App Management setting decides which apps may change other apps. Allow NativeNavi under System Settings › Privacy & Security › App Management, then build again.
- **A file in a cloud drive** (iCloud Drive, Proton Drive): it is downloaded when NativeNavi checks it. Keep it downloaded until the build is done.

## Removing it

Settings › **Remove Build Files…** and **Remove Tools…** free their space; the game's app stays as it is. To remove everything, delete NativeNavi, `~/Library/Developer/NativeNavi`, its two logs in `~/Library/Logs/NativeNavi` (the folder also holds the game's) and its preferences, `org.nativenavi.NativeNavi`.

## Packing the app

`scripts/build_gui.py` packs NativeNavi from the checkout, with Xcode 27.2 selected (or `DEVELOPER_DIR` set):

```
python3 scripts/build_gui.py                      # dist/NativeNavi.app, signed ad-hoc
python3 scripts/build_gui.py -o ~/Desktop/OoT\ Silicon.app
python3 scripts/build_gui.py --sign "Developer ID Application: <name> (<team ID>)" --zip
xcrun notarytool submit "dist/NativeNavi.zip" --keychain-profile <profile> --wait
xcrun stapler staple "dist/NativeNavi.app"
```

It compiles `dm/gui/*.swift` with `swiftc` (Swift 6, for macOS 26.6 from `dm/mac/min_os.mk`) and `dm/gui/AppIcon.icon` with `actool`, writes `Resources/setup.json` (`dm/gui/setup.json` with the commit, the tested Xcode and Metal compiler from `dm/gfx/tools/own_compilers.py` and `dmsh_chain.lock`, and the rows of `nup.tsv`) and packs the checkout's files into `Resources/source.tar.gz`: the tracked ones and the new ones git does not ignore, as they are on disk, so a checkout with changes packs them and calls its commit `<hash>+dirty`. With `--sign`, the app has the hardened runtime and a secure timestamp, which notarization needs; this app runs none of the game's code, so it can have them, unlike the game's own app ([A Mac app](development.md#a-mac-app)). Downloaded without notarization, it is blocked by Gatekeeper until it is allowed in System Settings › Privacy & Security. The game's app it builds is made on the player's Mac, so it is not quarantined.

`dm/tools/tests/test_gui.py` checks the pins, the facts the script reads and a packed app (with Xcode).

## How it drives the build

NativeNavi is a front end of the packing script, through the interface in [Front ends](frontends.md). Before the build it unpacks `source.tar.gz` into `Source` (once per commit, with `.source-commit` beside it, which `dm/mac/pack_app.py` reads for the game app's `DMCommit` in place of git), downloads Apple's Metal toolchain (`xcodebuild -downloadComponent metalToolchain`) when the tested Xcode lacks it, Python when its folder is not the pinned one, then installs the Python packages (`pip install --only-binary :all: -r .github/requirements/tools.txt`, quick when they are there), rustup (`rustup-init -y --no-modify-path --profile minimal --default-toolchain none`) and the host's Rust (`cargo --version` in `dm/host/rust`). Then it runs:

```
Tools/python/bin/python3 -u Source/scripts/build_app.py <NUP> --progress json --work-dir <build files> -o <folder for the app>
```

with `PATH` starting with its own Python and Rust, `DEVELOPER_DIR`, `RUSTUP_HOME` and `CARGO_HOME` set, and the variables that would pick other tools (`PYTHON*`, `VIRTUAL_ENV`, `CONDA_*`, `SDKROOT`, …) removed. It reads the events from standard output: `stage`, `step`, `progress` and `stage_end` draw the steps, `game` gives the game's name and the app's path, `say` lines go to Details, and the last `[RED]` line is the reason a build failed. Its Stop sends SIGTERM; exit codes 129, 130 and 143 are a stopped build. `-o` with a folder puts the app there under the game's name, which a front end does not know before the game is unpacked.

For development, `NATIVENAVI_SOURCE=<checkout>` makes the app build from that checkout instead of its own copy (run its executable from Terminal: `NATIVENAVI_SOURCE=$PWD "dist/NativeNavi.app/Contents/MacOS/NativeNavi"`).

Open: no build has been run through the app yet. The pinned downloads' SHA-256 and the packages' wheels for Python 3.14 on arm64 were checked; the installs themselves, a first build of an empty Mac (no Python, no Rust, no Metal toolchain) and the App Management prompt on replacing the game's app were not.
