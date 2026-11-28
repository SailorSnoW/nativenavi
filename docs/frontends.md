# Front ends

This repository has one graphical app that packs the game, [the NativeNavi app](gui.md), which uses the interface described here and nothing else. Other such apps are projects of their own: [NX2Apple](https://github.com/LNPANDY/NX2Apple), [NX Play](https://github.com/varyform/nx-native-wrapper) and [NxNative Launcher](https://github.com/rennan101/nxnative_Launcher) are such apps, written for nxnative, the project this repository was forked from; none of them was checked against this repository, whose apps carry NativeNavi's names where nxnative's carry nxnative's: the launcher `Contents/MacOS/nativenavi`, the bundle ID `org.nativenavi.<title ID>`, the folders below. This page lists what such an app can rely on here: how it runs a build and follows it, what the built app says about the game's settings, and how it starts the game with them.

- [Building: the packing script](#building-the-packing-script)
- [The settings list](#the-settings-list)
- [Starting a game](#starting-a-game)
- [Local users](#local-users)
- [Saved choices](#saved-choices)
- [Mods](#mods)
- [An app that hosts the engine](#an-app-that-hosts-the-engine)
- [Microphone input on Mac](#microphone-input-on-mac)
- [Following main](#following-main)
- [What a front end cannot see yet](#what-a-front-end-cannot-see-yet)

## Building: the packing script

`scripts/build_app.py <NUP file or its folder>` builds the game from its NUP container: a Mac app with `--platform mac-app` (the default), at the path `-o` names, or with `--platform mac` a development build without an app. Its options are in `--help` and in [The packing script in detail](development.md#the-packing-script-in-detail).

With `--progress json` it asks nothing and writes one JSON object per line on stdout. Every object has `ev` (the event) and `t` (Unix time, seconds):

| `ev` | Fields | When |
|---|---|---|
| `stage` | `k`, `n` (stage k of n), `name`, `step` | a stage starts; `n` can grow during the build (a stage that turns out to be needed: a check of an image's files, the practice tools, the pipeline prebuild, the pipeline archive, the prewarm pipelines) |
| `step` | `k`, `name`, `step` | a new step within the stage; its count starts again |
| `progress` | `k`, `name`, `step`, `done`, `total`, `unit` (`''`, `'B'` for bytes, `'files'`, …), `est` (the total is an estimate), `note`, `frac` (0 to 0.99, or `null` when the work cannot be counted), `eta` (seconds or `null`), `elapsed` | the count changes; at most every 0.5 s while only the count moves |
| `stage_end` | `k`, `name`, `ok`, `summary`, `elapsed`; `warn` (`true`, only when the stage finished without what it makes, with `ok` true: a pipeline archive, prewarm pipelines or practice tools that failed, or practice tools the release has no patch for, so the app is packed without them) | a stage ends |
| `shaders_stale` | `reason` (`'chain'`: libraries of another translation chain `DMSH_CHAIN`; `'inputs'`: the profile's shader inputs differ from the library's stamp `inputs.tsv`), `old` (the chain folders `b-<V>` or the stamp's digest), `new` (`b-<V>` now or the digest now), `count` (base libraries scheduled: one per current shader key, including cache restores) | once per shader step, before rebuilding after a chain or profile change; interrupted chain rebuilds still report it until the libraries are stamped. A step that reuses its libraries, or has none cached, sends none. A `say` with the same news follows |
| `game` | `title_id` (the base game's, as the app's `DMTitleID`), `name`, `version`, `bundle_id` (what the app will carry, after `--app-name` and `--bundle-id`), `out` (the Mac app the build will write), `icon` (the image given with `--icon`, else the game's own icon as unpacked from the NUP, `icon.jpg` in the build folder's `extract/`, else the image given with `--fallback-icon`, or `null` without any; pass a stand-in with `--fallback-icon` rather than `--icon`, so a game that carries an icon keeps it) | once, when the app's name, version and bundle ID are settled, before the shader stages; not for `--platform mac`, which makes no app. A `say` with the same news follows |
| `say` | `msg` | a message; one starting with `[RED] ` is an error |

The exit code is 0 for a finished build, 1 for a build that stopped on an error (its last `say` says why, starting with `[RED] `; an unexpected error also prints its traceback on stderr), 2 for invalid command-line arguments, 130 when interrupted, 129 when the terminal it runs in closed (SIGHUP, stopped as for SIGTERM), and 143 when stopped by SIGTERM (a front end's cancel: as for an interrupt, the running tool and everything it started are stopped; [Stopping a build left its tools running](reference/packing.md#stopping-a-build-left-its-tools-running)). Argument parsing happens before JSON mode starts: an argument error writes its usage and error to stderr and emits no JSON events. A front end must check the process's exit status and capture stderr as well as the JSON stream. Each tool's full output goes to the build folder's `logs/`, not to stdout. The build folder, `<title ID>_build/` in the NUP's folder, keeps what the next build can reuse, so a second build after a change takes a fraction of the first. A front end that keeps the build elsewhere passes `--work-dir <folder>`: that folder is the build folder, used as given (the script does not add `<title ID>_build`); the logs are in its `logs/`, and the derived profile of a version the game's profile does not register in its `profile/` (`--profile-out` names another place). Without `-o`, the Mac app goes beside the build folder, so give `-o` to put it somewhere else: a path ending in `.app`, or a folder that exists, where the app goes under its default name (`<game name>.app`, which the `game` event's `out` gives), for a front end that lets the player pick a folder such as Applications.

## The settings list

The built app carries the settings the game supports, as its settings window shows them ([Settings](settings.md)): `Contents/Resources/dm/launcher.json`. `dm/games/settings.py` writes it from `dm/games/settings.tsv`, whose header describes the rows. Ocarina of Time's list, shortened:

```json
{"schema": 1, "game": "0400486024DEC000", "settings": [
 {"env": "DM_FPS_TARGET", "kind": "choice", "section": "Game", "label": "Frame rate", "note": "", "default": "60", "level": "basic",
  "options": [["30", "30 fps"], ["60", "60 fps"]]},
 {"env": "DM_UPSCALER_SHARPNESS", "kind": "range", "section": "Game", "label": "3D upscaler sharpness", "note": "", "default": "0.7", "needs": "DM_UPSCALER=metalfx", "min": 0.0, "max": 1.0, "step": 0.05},
 {"env": "DM_FPS_HOLD", "kind": "toggle", "section": "Performance", "label": "Hold frames when late", "note": "", "default": "on", "on": "on", "off": "off"}
]}
```

- `env`: the environment switch the setting sets. `kind`: `choice` (one of `options`, each `[value, label]`), `range` (a number from `min` to `max` in `step`s) or `toggle` (`on` or `off`, the values the switch takes).
- `section`: the heading the row goes under, today `Game`, `Controls`, `Performance` or `Practice` (Practice tools, `DM_PRACTICE`: only in an app that carries them, `Resources/dm/practice/`). Sections come in the order of their first rows, and rows in the order to show them. A new section may appear in schema 1: a reader draws a section it does not know like the others, under its name, as the app's own window does.
- `default`: the tested default ([Tested defaults](settings.md#tested-defaults)), with profile defaults resolved for the game. A front end that keeps only the choices that differ from it lets a newer build's tested defaults reach the player, as the app's own window does.
- `level` (optional): `basic` marks a setting to show first, for a front end that shows a short list and puts the rest behind a second step. Without it, the front end decides.
- `needs` (optional): `<VAR>=<value>` makes the row a sub-setting of the row that sets `<VAR>`, drawn only while that row has that value (Rumble strength only while rumble is on). A row naming a variable the list does not have stays drawn.
- `test` (optional): `sticks` asks for a live test under the row: the Mac settings window draws the controller's two sticks with this row's dead zone (a `range` row's current value) and marks when the game receives input. A front end that draws the settings itself may ignore it.
- An app without the file has no settings to offer: start it as it is.

**Compatibility.** [launcher.schema.json](launcher.schema.json) describes the file as a JSON Schema, so a front end can check it and generate its types from it. `schema` is 1, and schema 1 grows only by optional fields: a reader ignores fields it does not know, draws sections it does not know, and a field never changes its meaning. A new `kind` would be schema 2, since a reader cannot draw a kind it does not know. A list written before the field existed has no `schema` and is 1. In `settings.tsv`, the optional fields come from the last column, `meta` (`key=value` pairs separated by `;`); a new key is added in `settings.py`'s `META`, here and in the schema together, and `test_launcher_settings.py` fails when the three disagree.

## Starting a game

The Mac app is started like any app, or by running its `Contents/MacOS/nativenavi` with the chosen settings in its environment (`DM_FPS_TARGET=30`, …). The launcher there runs the host as a child, forwards SIGTERM, SIGINT and SIGHUP to it and exits with its exit code: 0 for a quit (⌘Q, the Home prompt, closing the window), 70 after the alert that macOS stopped running the game's GPU work ([A GPU whose access macOS revoked stops the game](reference/rendering.md#a-gpu-whose-access-macos-revoked-stops-the-game)), 134 for an abort. A variable already in the environment wins; then come `~/Library/Application Support/NativeNavi/<title ID>/env.txt` and the bundle's `dm/env.txt`, one `KEY=VALUE` per line ([A Mac app](development.md#a-mac-app)). The save is in `save/` in that Application Support folder; an app's first start, with no such folder yet, takes a copy of the one an nxnative build of the game left (`~/Library/Application Support/nxnative/<title ID>`) and of that app's saved choices ([A Mac app](development.md#a-mac-app)). The log is `~/Library/Logs/NativeNavi/<title ID>.log`, the previous run's `<title ID>.prev.log`. The title ID is `DMTitleID` in the app's `Info.plist`.

The app shows its own settings window before the game, drawn from its `launcher.json` and keeping its choices in the same store ([Saved choices](#saved-choices), [The Mac settings window](settings.md#the-mac-settings-window)). It shows a variable the launch environment or an `env.txt` sets as fixed. A front end that draws the settings itself starts the app with `DM_LAUNCHER=0` in its environment (or `--no-launcher` on its command line) to skip the window. The saved choices still apply, so a front end that writes them gets the same game as the window would.

The game quits when the player holds a controller's Home button for 2 seconds and confirms with A ([Controllers](playing.md#controllers)). Its prompt is in English; a front end that shows the player another language passes the texts in `DM_HOME_QUIT_TEXT`, one per line in this order: the hold indicator ("Keep holding to quit"), the title ("Quit <game>?"), the note ("Unsaved progress will be lost."), the quit button and the cancel button. An empty or missing line keeps the English text. The same goes for the prompt that waits when a game asks for more players than have joined: `DM_CTRL_PROMPT_TEXT`, one per line, the title ("{min} players wanted"), the hint ("Press a button on each controller to join."), the count ("Players: {joined} of {min}"), the done button and the cancel button; `{min}`, `{max}` and `{joined}` are replaced by the numbers. Whether macOS lets the game have the Home button is the player's Home button setting for apps, which a front end reads itself on macOS 27 (`GCControllerHomeButtonSettingsManager`, while a controller is connected) to tell the player when the system keeps Home.

Settings a player chose in a front end and wants kept, also for a start from Finder or the Dock, go in the app's saved choices ([Saved choices](#saved-choices)), not in its environment.

## Local users

The host has no account selector and creates no users: `dm/host/front.h` has no user-selection call for a front end to implement. The preselected user the game opens is user 1 (`dm/host/hle_account.c`).

User 1 keeps the original UID, `DM_NICK`, and `DM_SAVE` directory. A save store that carries a registry of more users, `<DM_SAVE>.users` (a small `nxnative-users-1` record with a hexadecimal slot mask, which the host only reads), still lists them: they have stable UIDs and `Player N` names, and their saves are sibling directories `<DM_SAVE>_userN` (N = 2 through 8). A registry the host cannot read is left as it is and logged in red. Open sessions are never persisted.

When moving, backing up or restoring an app's local saves, preserve the registry, when there is one, and all of these committed account directories together. The transaction work directories (`.work` / `.new`) are not committed saves.

## Saved choices

The app keeps the player's choices in its own preferences domain, its bundle ID (`CFBundleIdentifier` in `Info.plist`, `org.nativenavi.<title ID>` unless the app was packed with another): one string per setting, under the key `launcher.<env>` (`launcher.DM_FPS_TARGET` = `"30"`). The app's settings window keeps them there, and its launcher (`dm/mac/Launcher.swift`) reads them at every start, so a start from Finder or the Dock without the window has them too. A front end writes the same keys (`CFPreferencesSetAppValue` with the app's bundle ID, or `defaults write org.nativenavi.<title ID> launcher.DM_FPS_TARGET -string 30`): there is one set of choices, and every view of it agrees.

- **Only choices that differ from the tested default are kept.** A setting the player left at its default follows the defaults of a later build. Remove a key when the player picks the default again. For "Reset to tested defaults", remove the keys of the settings the player can change; the app's own window does that when Play applies the defaults, and keeps the saved choice of a setting fixed for this start.
- **Checked when read.** The launcher uses a saved value only when this build still offers it: one of a `choice`'s options, a `toggle`'s `on` or `off`, or a number from a `range`'s `min` to `max`. It checks against the app's own list, `Contents/Resources/dm/launcher.json`, with the window or without. Any other value, a value that is not a string, or a key no setting has is ignored; it stays saved until the settings window's Play replaces it with the player's choice. So a rebuild that stops offering a value falls back to its tested default.
- **The order**, the first that sets a variable wins:
  1. the launch environment (a front end's launch overrides, a terminal);
  2. the player's `env.txt` (`~/Library/Application Support/NativeNavi/<title ID>/env.txt`);
  3. the bundle's `dm/env.txt`;
  4. saved choices;
  5. the tested defaults: `launcher.json`'s, then the host's own for a variable no setting names.
- **Launch overrides are temporary.** A front end that starts the game with a variable in its environment overrides the saved choice for that start only. It writes the store only when the player means to keep the choice.
- **`DM_SAVED_CHOICES=0`** (in the environment or an `env.txt`) starts with the tested defaults and leaves the saved choices as they are. **`DM_LAUNCHER=0`** only skips the settings window: the saved choices and `launcher.json`'s defaults still apply, in the same order, and nothing is saved.
- **A sandboxed front end** cannot write another app's preferences domain; this store is for front ends outside the App Sandbox.

## Mods

Mods are files, not settings: the host reads a mods folder, `DM_MODS=<folder>`, else `<DM_ROOT>/mods` ([Mods](development.md#mods) has the layouts it accepts and what it refuses). A front end installs a mod by copying its folder there, and orders or disables mods by writing `order.txt` in it: one mod folder name per line, the highest priority first, `# <name>` for a mod that is off; folders the file does not name are on, after the named ones, in name order. The host reads the folder once, at the start.

A mod's code patch that the host cannot make at load time needs the module rewritten with it before the host starts: a front end that keeps the original modules runs `dm/tools/mods_rewrite.py` first and starts the host on the folder it prints ([Code patches through the rewriter](development.md#code-patches-through-the-rewriter)); one that does not (the packed `.app`) leaves such patches refused, which the host logs in red (open).

## An app that hosts the engine

An app that links the host and the front end's game view into its own process has one hook in `dm/host/front.h`. This repository's own app does not use it, so its tests and regression runs do not cover it. It is unset by default, and must be set before the game starts.

- **`front_progress_hook`**: draws the prepare step's progress ("Preparing shaders", "Preparing pipelines") instead of the front end's screen. With a window, it gets each call's title, done and total on the main thread; the title is `NULL` when the step ends.

## Microphone input on Mac

A packed Mac app exposes `DM_MIC` in its settings list when a game imports the supported SDK input stream. Its default is off. The app requests macOS permission before it starts the host on the first enabled launch, including with `DM_LAUNCHER=0`. A denied request starts the game with input disabled. Front ends that spawn the host themselves must request consent in their own app and include `NSMicrophoneUsageDescription` in its Info.plist.

The Mac launcher stores a selected input device's Core Audio UID as the string `launcher.DM_MIC_DEVICE` in the app's preferences. A `DM_MIC_DEVICE` set at launch or in an env file takes precedence. Use `default` to follow the system input. `DM_MIC_GAIN` uses the saved choices described above.

## Following main

This repository has no releases or tags: a front end pins a commit of `main` and moves it forward. Between the commit it has (`A`) and the one it moves to (`B`), two things say what changed for it.

**What git shows.** These need no note from anyone, and no commit adds one:

| What changed | The command |
|---|---|
| This page, the settings list's schema, the settings the game can offer | `git log -p A..B -- docs/frontends.md docs/launcher.schema.json dm/games/settings.tsv` |
| The packing script's options | `git diff A..B -- scripts/build_app.py \| grep add_argument` |
| The environment switches, with each one's class (`switch`, `path`, `diagnostic`) | `git diff A..B -- docs/env_switches.tsv` |
| The shader chain: the game's shaders are rebuilt at its next build, which the `shaders_stale` event reports | `git log --oneline A..B -- dm/gfx/nvn/dmsh_chain.h` |

**What only the author can say.** A commit a front end has to act on, in a way the commands above do not show, says so in a line of its own in its message, one line per thing, and `git log A..B --grep='^Front-ends:'` finds them:

```
Front-ends: break: what stopped working, and what to do about it
Front-ends: interface: an existing variable, argument, event, field or path that now means something else
Front-ends: new: a kind of input, or a requirement, that a front end could offer or has to meet
```

Most commits have none. A line is the author's knowledge, so its absence is no proof that nothing changed.

## What a front end cannot see yet

While a game runs, the host says what happens only in its log, as text meant for people: a controller that connects or goes away, the preparing of shaders before the first frame, a red line. A front end that reads the log to show these would break when a line's wording changes. Events a program can read (one JSON object per line, as the packing script's) would be the next piece of this interface; until then, treat log lines as diagnostics, not as an interface.
