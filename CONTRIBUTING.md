# Contributing

This page says how a change gets in; what the code does and where things are is in [AGENTS.md](AGENTS.md) and [docs/development.md](docs/development.md). The project has one aim: The Legend of Zelda: Ocarina of Time (Switch 2) running as well as possible on Apple silicon Macs. A change for another game, for iPhone or iPad, or for another graphics API belongs in [nxnative](https://github.com/seedds/nxnative), which this repository was forked from.

## Reporting a problem

Say what a problem is traced with: the commit, the shader chain version (`DMSH_CHAIN` in `dm/gfx/nvn/dmsh_chain.h`), the game's version, the Mac and its macOS, how the game was built and run, the settings changed and environment overrides, and the whole log of the run. A Mac app writes `~/Library/Logs/nxnative/0400486024DEC000.log`; a development build writes where its command says.

## Never game files or keys

Never commit or attach game files or anything extracted from the game: RomFS, code modules, shaders, dumps. They stay in the game's build folder ([AGENTS.md](AGENTS.md)). Logs, screenshots, build IDs and offsets are fine.

## Before you start

- Read [AGENTS.md](AGENTS.md): it lists where to start for each kind of task, and the rules for the profile, per-build knowledge and the shader translation chain.
- A crash, a hang or a wrong picture: search the [symptom index](docs/agents/symptoms.md) for the text in the log first.
- Fix a bug at its cause ([AGENTS.md](AGENTS.md)). A change that only serves this game is welcome.

## A change

- **One change per commit.** Each commit message opens with `area/component: summary` and says what changed and why, in plain English sentences, as the history does: the symptom, the cause, the fix, and what was measured or checked ([Commit messages](docs/agents/06-dev-loop.md#7-commit-messages)).
- **Front ends.** A change a front end has to act on takes a `Front-ends:` line in the commit message, unless git already shows it ([Front ends](docs/frontends.md)). A change that makes the host read saves or other kept data differently also carries the old form forward.
- **Where findings go.** A fact about the game goes in its note ([dm/games/0400486024DEC000/README.md](dm/games/0400486024DEC000/README.md)). A mechanism of the host or the tools gets an entry in `docs/reference/<subsystem>.md` (Symptom, Cause, Fix, Found on), followed by `python3 dm/tools/docs_symptoms.py`.
- **The shader chain.** After changing one of its tools, run `python3 dm/gfx/tools/dmsh_chain.py --header` and commit `dm/gfx/nvn/dmsh_chain.h` with the change. The game's shaders are then all rebuilt, so say what you checked in game.
- **Tests.** CI runs the host tests, the CPU-only graphics tests, the tool tests and the lint ([Continuous integration](docs/development.md#continuous-integration)). GitHub's runners have no Metal 4, so run the GPU tests on an Apple silicon Mac: `make -C dm/gfx/tests run`. Then run the game ([06-dev-loop.md](docs/agents/06-dev-loop.md)).
- **What was not tested.** Say which Macs a change was run on. A path a Mac cannot exercise (hardware AV1 decoding needs an M3 or later) is named as untested.

## Graphical front ends

An app that packs the game or starts it (a build window, a library) is a separate project, not a part of this repository. Such an app needs no changes here to work: the packing script reports its stages as JSON lines (`--progress json`), a built Mac app carries the list of the game's settings (`dm/launcher.json`, from `dm/games/settings.tsv`), and the app takes those settings as environment switches or as saved choices in its preferences, which every start reads ([Front ends](docs/frontends.md)).

The settings window inside the Mac app is part of this repository ([Settings](docs/settings.md)): it draws the same list and keeps the same saved choices as any front end.
