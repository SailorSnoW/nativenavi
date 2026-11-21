# Front ends

A graphical app that packs games, keeps a library of them or starts them is a project of its own that builds on nxnative. [NX2Apple](https://github.com/LNPANDY/NX2Apple), [NX Play](https://github.com/varyform/nx-native-wrapper) and [NxNative Launcher](https://github.com/rennan101/nxnative_Launcher) are such apps. This page lists what such an app can rely on: how it runs a build and follows it, what a built app says about its game's settings, and how it starts a game with them. None of it needs a change to nxnative; when a front end needs more, the piece it needs is an issue or a small PR here.

- [Building: the NSP script](#building-the-nsp-script)
- [The settings list](#the-settings-list)
- [Starting a game](#starting-a-game)
- [An app that hosts the engine](#an-app-that-hosts-the-engine)
- [Microphone input on Mac](#microphone-input-on-mac)
- [Following main](#following-main)
- [What a front end cannot see yet](#what-a-front-end-cannot-see-yet)

## Building: the NSP script

`scripts/build_app.py <NSP folder>` builds one game: an IPA by default, a Mac app with `--platform mac-app`, at the path `-o` names. Its options are in [The packing script in detail](development.md#the-packing-script-in-detail) and `--help`.

With `--progress json` it asks nothing and writes one JSON object per line on stdout. Every object has `ev` (the event) and `t` (Unix time, seconds):

| `ev` | Fields | When |
|---|---|---|
| `stage` | `k`, `n` (stage k of n), `name`, `step` | a stage starts; `n` can grow during the build (a provisional profile's heap check) |
| `step` | `k`, `name`, `step` | a new step within the stage; its count starts again |
| `progress` | `k`, `name`, `step`, `done`, `total`, `unit` (`''`, `'B'` for bytes, `'files'`, …), `est` (the total is an estimate), `note`, `frac` (0 to 0.99, or `null` when the work cannot be counted), `eta` (seconds or `null`), `elapsed` | the count changes; at most every 0.5 s while only the count moves |
| `stage_end` | `k`, `name`, `ok`, `summary`, `elapsed`; `warn` (`true`, only when the stage finished without what it makes, with `ok` true: a pipeline archive that failed, so the app is packed without one) | a stage ends |
| `shaders_stale` | `reason` (`'chain'`: libraries of another translation chain `DMSH_CHAIN`; `'inputs'`: the profile's shader inputs differ from the library's stamp `inputs.tsv`), `old` (the chain folders `b-<V>` or the stamp's digest), `new` (`b-<V>` now or the digest now), `count` (base libraries scheduled: one per current shader key and selected target, including cache restores) | once per shader step, before rebuilding after a chain or profile change; interrupted chain rebuilds still report it until the selected targets have stamped libraries. A step that reuses its libraries, or has none cached for its targets, sends none. A `say` with the same news follows |
| `game` | `title_id` (the base game's, as the app's `DMTitleID`), `name`, `version`, `bundle_id` (what the app will carry, after `--app-name` and `--bundle-id`), `out` (the IPA or Mac app the build will write; `null` for `--prepare-xcode`), `icon` (the image given with `--icon`, else the game's own icon as extracted, a 256×256 `icon.jpg` in the build folder, else the image given with `--fallback-icon`, or `null` without any; pass a stand-in with `--fallback-icon` rather than `--icon`, so a game that carries an icon keeps it) | once, when the app's name, version and bundle ID are settled, before the shader stages; not for `--platform mac`, which makes no app. Copy the icon before the build ends: an IPA build without `--keep-extracted` deletes the extraction it is in. A `say` with the same news follows |
| `say` | `msg` | a message; one starting with `[RED] ` is an error |

The exit code is 0 for a finished build, 1 for a build that stopped on an error (its last `say` says why, starting with `[RED] `; an unexpected error also prints its traceback on stderr), 2 for invalid command-line arguments, 130 when interrupted, 129 when the terminal it runs in closed (SIGHUP, stopped as for SIGTERM), and 143 when stopped by SIGTERM (a front end's cancel: as for an interrupt, the running tool and everything it started get the signal first, and what is left of them 5 seconds later is killed; [Stopping a build left its tools running](reference/packing.md#stopping-a-build-left-its-tools-running)). Argument parsing happens before JSON mode starts: an argument error writes its usage and error to stderr and emits no JSON events. A front end must check the process's exit status and capture stderr as well as the JSON stream. Each tool's full output goes to `<NSP folder>/<title ID>_build/logs/`, not to stdout. The build folder `<title ID>_build/` keeps what the next build can reuse, so a second build after a change takes a fraction of the first. A front end that keeps each game's build elsewhere passes `--work-dir <folder>`: that folder is the build folder, used as given (the script does not add `<title ID>_build`), so the front end names it per game; the logs are in its `logs/`, and the provisional profile of a game without one in `profile/`, so a front end that reuses the folder for a game keeps its profile (one from an earlier run in `<NSP folder>/<title ID>_build/profile` is copied there once, and stays). Without `-o`, the IPA or Mac app goes to the folder holding it, so give `-o` to put it somewhere else.

## The settings list

A built app carries the settings its game supports, as the iPhone app's settings screen shows them ([Settings](settings.md)): `dm/launcher.json` in the app inside an IPA, `Contents/Resources/dm/launcher.json` in a Mac app. The Mac app's list leaves out the settings only the iPhone app reads (the frame-rate counter, touch feedback). `dm/games/settings.py` writes it from `dm/games/settings.tsv`, whose header describes the rows. Breath of the Wild's Mac list, shortened:

```json
{"schema": 1, "game": "01007EF00011E000", "settings": [
 {"env": "DM_FPS_TARGET", "kind": "choice", "section": "Game", "label": "Frame rate", "note": "…", "default": "60",
  "options": [["30", "30 fps"], ["60", "60 fps"]], "level": "basic"},
 {"env": "DM_RENDER_SCALE", "kind": "range", "section": "Game", "label": "Render scale", "note": "…", "default": "1.0", "min": 0.5, "max": 1.0, "step": 0.05},
 {"env": "DM_FPS_HOLD", "kind": "toggle", "section": "Performance", "label": "Hold frames when late", "note": "…", "default": "on", "on": "on", "off": "off"}
]}
```

- `env`: the environment switch the setting sets. `kind`: `choice` (one of `options`, each `[value, label]`), `range` (a number from `min` to `max` in `step`s) or `toggle` (`on` or `off`, the values the switch takes).
- `section`: the heading the row goes under, today `Game`, `Controls` or `Performance`. Sections come in the order of their first rows, and rows in the order to show them. A new section may appear in schema 1: a reader draws a section it does not know like the others, under its name, as the iPhone app does.
- `default`: the value for this app's platform ([Tested defaults](settings.md#tested-defaults)), with profile defaults resolved for its game. A front end that keeps only the choices that differ from it lets a newer build's tested defaults reach the player, as the iPhone app does.
- `base` (optional, render scale only): `{"docked": [w, h], "handheld": [w, h]}`, the 3D views' size that the scale multiplies, so a front end can show 0.75 as 1200×675 docked. It comes from the game's profile (`render_base`), and only some games have it.
- `level` (optional): `basic` marks a setting to show first, for a front end that shows a short list and puts the rest behind a second step. Without it, the front end decides.
- `needs` (optional): `<VAR>=<value>` makes the row a sub-setting of the row that sets `<VAR>`, drawn only while that row has that value (Rumble strength only while rumble is on). A row naming a variable the list does not have stays drawn.
- An app without the file has no settings to offer: start it as it is.

**Compatibility.** [launcher.schema.json](launcher.schema.json) describes the file as a JSON Schema, so a front end can check it and generate its types from it. `schema` is 1, and schema 1 grows only by optional fields: a reader ignores fields it does not know, draws sections it does not know, and a field never changes its meaning. A new `kind` would be schema 2, since a reader cannot draw a kind it does not know. A list written before the field existed has no `schema` and is 1. In `settings.tsv`, the optional fields come from the last column, `meta` (`key=value` pairs separated by `;`); a new key is added in `settings.py`'s `META`, here and in the schema together, and `test_launcher_settings.py` fails when the three disagree.

## Starting a game

**A Mac app** is started like any app, or by running its `Contents/MacOS/nxnative` with the chosen settings in its environment (`DM_FPS_TARGET=30`, …). The launcher there runs the host as a child, forwards SIGTERM, SIGINT and SIGHUP to it and exits with its exit code: 0 for a quit (⌘Q, the Home prompt, closing the window), 70 after the alert that macOS stopped running the game's GPU work ([A GPU whose access macOS revoked stops the game](reference/rendering.md#a-gpu-whose-access-macos-revoked-stops-the-game)), 134 for an abort. A variable already in the environment wins; then come `~/Library/Application Support/nxnative/<title ID>/env.txt` and the bundle's `dm/env.txt`, one `KEY=VALUE` per line ([A Mac app](development.md#a-mac-app)). The save is in `save/` in that Application Support folder; the log is `~/Library/Logs/nxnative/<title ID>.log`, the previous run's `<title ID>.prev.log`. The title ID is `DMTitleID` in the app's `Info.plist`.

A Mac app shows its own settings window before the game, drawn from its `launcher.json` and keeping its choices in the same store ([Saved choices](#saved-choices), [The Mac settings window](settings.md#the-mac-settings-window)). It shows a variable the launch environment or an `env.txt` sets as fixed. A front end that draws the settings itself starts the app with `DM_LAUNCHER=0` in its environment (or `--no-launcher` on its command line) to skip the window. The saved choices still apply, so a front end that writes them gets the same game as the window would.

A Mac game quits when the player holds a controller's Home button for 2 seconds and confirms with A ([On the Mac](playing.md#on-the-mac)). Its prompt is in English; a front end that shows the player another language passes the texts in `DM_HOME_QUIT_TEXT`, one per line in this order: the hold indicator ("Keep holding to quit"), the title ("Quit <game>?"), the note ("Unsaved progress will be lost."), the quit button and the cancel button. An empty or missing line keeps the English text. The same goes for the prompt that waits when a game asks for more players than have joined (Mac and iOS): `DM_CTRL_PROMPT_TEXT`, one per line, the title ("{min} players wanted"), the hint ("Press a button on each controller to join."), the count ("Players: {joined} of {min}"), the done button and the cancel button; `{min}`, `{max}` and `{joined}` are replaced by the numbers. Whether macOS lets the game have the Home button is the player's Home button setting for apps, which a front end reads itself on macOS 27 (`GCControllerHomeButtonSettingsManager`, while a controller is connected) to tell the player when the system keeps Home.

Settings a player chose in a front end and wants kept, also for a start from Finder or the Dock, go in the app's saved choices ([Saved choices](#saved-choices)), not in its environment.

**An IPA** is installed by the player (sideloading); its own settings screen starts the game. Lines in the app's `Documents/env.txt` win over that screen ([Settings in a file](settings.md#settings-in-a-file)). The save and `log.txt` are in the app's Documents folder.

## Local users and the account selector

The account applet uses `front_user_select()` (`dm/host/front.h`) on the guest thread; the front end presents native UI on its main thread and blocks the caller until a choice or cancellation. The built-in Mac dialog accepts mouse/keyboard input, and the iOS dialog accepts touches. Show the host-supplied users (including their in-use labels), and Add user / Skip only when allowed. Return the user's slot or the named `FRONT_USER_*` action. A front end without an interactive surface returns `FRONT_USER_CANCEL`; it must not guess an account from the number of controllers. The host validates the answer, owns registration and session state, and applies the game's exclusions and permitted automatic selection.

User 1 keeps the original UID, `DM_NICK`, and `DM_SAVE` directory. Additional local users have stable UIDs and `Player N` names; their saves are sibling directories `<DM_SAVE>_userN` (N = 2 through 8). Registered slots are stored in `<DM_SAVE>.users` (a small `nxnative-users-1` record with a hexadecimal slot mask); do not edit it while the game runs. Registration is atomic and separate from save commits. Open sessions are never persisted.

When moving, backing up or restoring an app's local saves, preserve the registry and all of these committed account directories together, as well as the existing device save. The transaction work directories (`.work` / `.new`) are not committed saves. The current cloud-sync package still covers only the primary account and device save: additional local users and their saves do not sync. A primary-save-only export does not contain the other users' progress.

## Saved choices

Each app keeps the player's choices in its own preferences domain, its bundle ID (`CFBundleIdentifier` in `Info.plist`, `org.nxnative.<title ID>` unless the app was packed with another): one string per setting, under the key `launcher.<env>` (`launcher.DM_FPS_TARGET` = `"30"`). The iPhone app's settings screen keeps them there, and a Mac app's launcher (`dm/mac/launcher.c`) reads them at every start, so a start from Finder or the Dock has them too. A front end writes the same keys (`CFPreferencesSetAppValue` with the app's bundle ID, or `defaults write org.nxnative.<title ID> launcher.DM_FPS_TARGET -string 30`), and a settings window inside the app would too: there is one set of choices, and every view of it agrees.

- **Only choices that differ from the tested default are kept.** A setting the player left at its default follows the defaults of a later build. Remove a key when the player picks the default again. For "Reset to tested defaults", remove the keys of the settings the player can change; the app's own screen does that when Play applies the defaults, and keeps the saved choice of a setting fixed for this start.
- **Checked when read.** The launcher uses a saved value only when this build still offers it: one of a `choice`'s options, a `toggle`'s `on` or `off`, or a number from a `range`'s `min` to `max`. It checks against the app's own list, which the packer copies into `Info.plist` as `DMSettings`. Any other value, a value that is not a string, or a key no setting has is ignored; it stays saved until the settings screen's Play replaces it with the player's choice. So a rebuild that stops offering a value falls back to its tested default.
- **The order**, the first that sets a variable wins:
  1. the launch environment (a front end's launch overrides, a terminal);
  2. the player's `env.txt` (`~/Library/Application Support/nxnative/<title ID>/env.txt`; on the iPhone, `Documents/env.txt`);
  3. the bundle's `dm/env.txt`;
  4. saved choices;
  5. the tested defaults (the host's own, from the game's profile).
- **Launch overrides are temporary.** A front end that starts the game with a variable in its environment overrides the saved choice for that start only. It writes the store only when the player means to keep the choice.
- **`DM_SAVED_CHOICES=0`** (in the environment or an `env.txt`) starts with the tested defaults and leaves the saved choices as they are. **`DM_LAUNCHER=0`** only skips the settings screen (the iPhone's, or a Mac app's window): the saved choices still apply.
- **A sandboxed front end** cannot write another app's preferences domain; this store is for front ends outside the App Sandbox, as NX2Apple is.

## Mods

Mods are files, not settings: the host reads a mods folder, `DM_MODS=<folder>`, else `<DM_ROOT>/mods` ([Mods](development.md#mods) has the layouts it accepts and what it refuses). A front end installs a mod by copying its folder there, and orders or disables mods by writing `order.txt` in it: one mod folder name per line, the highest priority first, `# <name>` for a mod that is off; folders the file does not name are on, after the named ones, in name order. The host reads the folder once, at the start.

A mod's code patch that the host cannot make at load time needs the module rewritten with it before the host starts: a Mac front end that keeps the original modules runs `dm/tools/mods_rewrite.py` first and starts the host on the folder it prints ([Code patches through the rewriter](development.md#code-patches-through-the-rewriter)); one that does not (the packed `.app`) leaves such patches refused, which the host logs in red (open).

## An app that hosts the engine

An app that links the host and the front end's game view into its own process (a launcher holding several games, say) has these hooks in `dm/host/front.h`. nxnative's own apps do not use them, so its tests and regression runs do not cover them. Each one is unset by default, and a hook must be set before the game starts.

- **`front_progress_hook`**: draws the prepare step's progress ("Preparing shaders", "Preparing pipelines") instead of the front end's screen. With a window, it gets each call's title, done and total on the main thread; the title is `NULL` when the step ends.
- **`dm_set_game_image_header`** (iOS): names the image that carries the game's rewritten modules, when the app links them into a framework it `dlopen`s from its signed bundle instead of into its executable. Call it with that image's mach header before `dm_host_preload_start`. Only the module sections are read from it.

An iOS front end that supplies its own audio-session lifecycle also implements `int dm_ios_output_channels(void)`, returning its active `AVAudioSession.outputNumberOfChannels`. After activating a session or changing routes, request six output channels when `maximumOutputNumberOfChannels` permits them. On `AVAudioSessionRouteChangeNotification`, apply that preference before calling `dm_audio_route_changed()`; keep the existing suspend/resume and media-services-reset calls. The bundled `front_ios.m` implements this contract. The Mac host watches its output routes itself. See [Output channels](reference/audio.md#output-channels).

## Microphone input on Mac

A packed Mac app exposes `DM_MIC` in its settings list when a game imports the supported SDK input stream. Its default is off. The app requests macOS permission before it starts the host on the first enabled launch, including with `DM_LAUNCHER=0`. A denied request starts the game with input disabled. Front ends that spawn the host themselves must request consent in their own app and include `NSMicrophoneUsageDescription` in its Info.plist.

The Mac launcher stores a selected input device's Core Audio UID as the string `launcher.DM_MIC_DEVICE` in the app's preferences. A `DM_MIC_DEVICE` set at launch or in an env file takes precedence. Use `default` to follow the system input. `DM_MIC_GAIN` uses the saved choices described above.

## Following main

nxnative has no releases or tags: a front end pins a commit and moves it forward. Between the commit it has (`A`) and the one it moves to (`B`), two things say what changed for it.

**What git shows.** These need no note from anyone, and no commit adds one:

| What changed | The command |
|---|---|
| This page, the settings list's schema, the settings a game can offer | `git log -p A..B -- docs/frontends.md docs/launcher.schema.json dm/games/settings.tsv` |
| The NSP script's options | `git diff A..B -- scripts/build_app.py \| grep add_argument` |
| The environment switches, with each one's class (`switch`, `path`, `diagnostic`) | `git diff A..B -- docs/env_switches.tsv` |
| The shader chain: each game's shaders are rebuilt at its next build, which the `shaders_stale` event reports | `git log --oneline A..B -- dm/gfx/nvn/dmsh_chain.h` |

**What only the author can say.** A commit a front end has to act on, in a way the commands above do not show, says so in a line of its own in its message, one line per thing, and `git log A..B --grep='^Front-ends:'` finds them:

```
Front-ends: break: what stopped working, and what to do about it
Front-ends: interface: an existing variable, argument, event, field or path that now means something else
Front-ends: new: a kind of input or game, or a requirement, that a front end could offer or has to meet
```

Most commits have none, and commits from before this section have none either. A line is the author's knowledge, so its absence is no proof that nothing changed.

**Data a player already has moves by itself.** When a change makes the host read saves or other kept data differently, the host carries the old form forward, as with [saves named before backslash paths](reference/services.md#saves-named-before-backslash-paths): a front end has nothing to patch, and a player of nxnative's own apps gets the same fix.

## What a front end cannot see yet

While a game runs, the host says what happens only in its log, as text meant for people: a controller that connects or goes away, the preparing of shaders before the first frame, a red line. A front end that reads the log to show these would break when a line's wording changes. Events a program can read (one JSON object per line, as the NSP script's) would be the next piece of this interface; until then, treat log lines as diagnostics, not as an interface.
