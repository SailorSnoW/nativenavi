# 06 — The developer loop: which tool when, and before a commit

One page for people and agents: the tools this repository has for building, running, debugging, measuring and checking a change, when to use which, and what a commit needs. It links to where each tool is described instead of repeating it. Game files, builds and runs stay outside the repository, in the game's build folder ([AGENTS.md](../../AGENTS.md)).

## 1. Build and run on the Mac

| Step | Tool | Details |
|---|---|---|
| Pack the game for development | `scripts/build_app.py <NUP> --platform mac` | Unpacking, rewriter, shader split and chain, the Mac host; prints the command that runs the game. [A Mac-only build](../development.md#a-mac-only-build-for-development) |
| A double-clickable app | the same with `--platform mac-app`, the default (adds the offline pipeline archive) | [A Mac app](../development.md#a-mac-app) |
| Rebuild only the host | `make -C dm/host dmhost` | [Build by hand (Mac)](../development.md#build-by-hand-mac) |
| Rebuild only the shaders | `dm/gfx/tools/dmsh_build.py <package dir> <cache dir>` (after a chain change: `dmsh_chain.py --header` first) | the chain procedure in [AGENTS.md](../../AGENTS.md); [04](04-graphics.md) |
| Run with scripted input | `DM_KEYS` (button presses at fixed seconds or frames), `DM_CTL` (presses, screenshots and dumps while it runs), `DM_REC` (records what the player does as `DM_KEYS` steps, to replay a report's path or make a route) | [05, Scripted input](05-debugging.md#scripted-input-and-thread-scheduling) |

Start Mac runs with the Metal performance HUD (`DM_METAL_HUD=1`). The run loop and the log are in [03](03-bringup.md).

## 2. When something is wrong

1. Search the [symptom index](symptoms.md) for the `[RED]` line, abort text or what you see; it leads to the reference entry and where it was found.
2. Crashes: the host's `CRASH sig=…` report, the macOS crash report, lldb scripts, Ghidra / objdump on the rewritten modules: [05](05-debugging.md#reading-a-crash-report).
3. Hangs and threads: `DM_WATCH=<s>` / `DM_WATCH_STALL=<s>` dump every thread ([03, Reading the log](03-bringup.md#reading-the-log)); `DM_THR=1`, `DM_SVC=1`, `DM_FS=1` log thread SVCs, memory SVCs and file opens; `sample` / `xctrace` name the guest threads during a hitch ([05](05-debugging.md#scripted-input-and-thread-scheduling)); `DM_HLECOUNT=<file>` counts every HLE function and SVC the game called.
4. Guest state without a debugger: `DM_PROBE` and its watch and heap variants ([05](05-debugging.md#probes-without-a-debugger-dm_probe)).
5. Wrong picture: `DM_NVN_SHOT` screenshots, `DM_NVN_PASSDUMP` (every pass, its attachments, bindings and draw state), `DM_NVN_TEXDUMP` / `DM_NVN_TEXSNAP` (render targets as PNG), `DM_NVN_CBDUMP` (uniform buffers, vertex streams), `DM_NVN_HILITE` / `DM_NVN_DEPTHOVR` / `DM_NVN_SKIP_FS` (isolate a draw), `DM_NVN_KEYDUMP` and `DM_NVN_MAGSCAN`: [04, Diagnostic switches](04-graphics.md#7-diagnostic-switches-host) and [05, Intermittent rendering glitches](05-debugging.md#intermittent-rendering-glitches).
6. Slow frames: the `[longframe]` lines and the hitch dump (`DM_HITCH_DUMP`), pipeline compile waits ([05](05-debugging.md#pipeline-compile-waits)), the periodic lines ([05](05-debugging.md#periodic-log-lines)), GPU time per pass (`DM_NVN_GPUPROF`, `DM_AB` A/B phases, `DM_GPU_CAPTURE` for Xcode; read with `dm/tools/gpuprof.py`): [05, GPU time on the device](05-debugging.md#gpu-time-on-the-device). Attachment traffic: `dm/gfx/tools/tbdr_*.py` over a pass dump.
7. Memory: the host's memory line, `footprint`, `vmmap`, `DM_NVN_ALLOCPROF=1`: [05, Memory growth](05-debugging.md#memory-growth).
8. Shaders: `dm/gfx/tools/shader_regressions.py` (shaders that built in the previous chain and fail now), the build log's [`[RED] lost without failing`](../reference/shaders.md#losses-a-shader-build-reports) line, `DM_NVN_SHDUMP` (programs missing from the map), the offline link check ([05](05-debugging.md#offline-pipeline-link-check)).
9. A new build of the game and fixed addresses: `dm/tools/nso_fnmap.py`, `nso_syms.py`, `insn_census.py`, `sdis.py`, `nso2elf.py` ([Tools](../development.md#tools-dmtools)).

## 3. Measuring

`scripts/bench.py <build folder> --route <name>` runs a route of the profile's `bench.tsv` from a fixed save and gives frame-time percentiles, GPU time and screenshots; variants (`--variant`) compare two hosts, shader libraries or sets of switches in one invocation, which is the only comparison it supports ([A benchmark on the Mac](../development.md#a-benchmark-on-the-mac)). Power and energy per frame: [Power](../development.md#power). Pass `--lock <file>` when other agents or people measure on the same Mac: the lock is held only while a game runs, and heavy builds should wait for it.

## 4. Checking a change: `scripts/regress.py`

`scripts/regress.py` compares a baseline revision with a candidate on the game's `--platform mac` build, without touching that build. The baseline is by default the merge base of `HEAD` and `main` (on `main`, the last commit), the candidate this checkout as it is, uncommitted and untracked files included. Each revision is checked out once as a git worktree, and the chain, host and rewriter come from there. Full description: [The regression harness](../development.md#the-regression-harness), and the comment at the top of the script.

Two locations have no default and go before the command, or in the environment:

- `--root` (`REGRESS_ROOT`): the source trees (`<root>/trees/`) and the reports (`<root>/reports/`).
- `--games-dir` (`REGRESS_GAMES`): the folder that holds the game's folder, the one with `0400486024DEC000_build` in it. What the harness derives from the game (chain builds, baselines, captures, its copy of the save) goes to `0400486024DEC000-work/regress/` in the game's folder.

| Command | What runs | Cost |
|---|---|---|
| `regress.py chain` | Static: each revision's shader build, compared shader by shader (built or failed, MSL / reflection / variant hashes), and the vertex / pixel interface of the pipeline archive's bases | nothing when the chain did not change; a changed chain translates every shader again, the longest step of a build |
| `regress.py check --quick` | The above plus a short boot: the route's `quick` seconds (60 by default), no pipeline prebuild | |
| `regress.py check` | The whole route with the pipeline prebuild first (`DM_PSO_PREBUILD=tool`) under a watchdog | |
| `regress.py capture --rev main` | Records one frame (the route's `capture` frame) with `DM_NVN_CAPTURE` and measures its replay noise ([Frame capture and replay](../development.md#frame-capture-and-replay)) | |
| `regress.py replay` | The current capture replayed by both revisions' `nvn_replay` with their own shader libraries, compared with the live frame within twice the capture's noise | seconds once the replayers and chains are built; checks the replay layer only (`nvn_mtl.m` and below) |
| `regress.py baseline --rev main --runs 2` | Records a baseline (a missing one is recorded by `check` itself); two runs give the noise | as `check`, per run |
| `regress.py list` | The game found, its route, its stored baselines and the chain it was last green on | |

The route is `dm/games/<id>/smoke.tsv`: a fixed save, the presses that load it (written by hand, or played once with `DM_REC` and joined to one line), the frame it must reach, and screenshots only while the player stands (where a walk ends moves from run to run). The profile's route is upstream nxnative's (`9143b6ff`): an empty save to the title and its movie with no input, 400 s, frame 900 by 360 s, its timings taken on an M4 (a cold prepare of 265 s in quick mode); it has not been run on this fork yet, so a slower Mac may need them loosened. Without a route, `check` makes the chain checks only and `capture` has nothing to record, and the run of the game is a separate, scripted run ([03](03-bringup.md#the-loop)). The script's top comment lists the route's keys.

A smoke run also compares the observed depth mode with the profile's `depth_mode`, even without an archive in quick mode, and detects a bundled archive rejected for a stamp mismatch. See [archive/profile depth mismatches](../reference/shaders.md#offline-pipeline-archive-and-profile-depth-disagree). Missing runtime observations remain unknown.

Reading the report (`report.md` beside `report.json` in `<root>/reports/<date-time>-<command>/`, or `--out`): `PASS` and `CHANGED` (MSL or numbers moved, nothing worse) exit 0; `NEW-FAIL` (a shader, the interface check, a crash, a hang or the frame that worked in the baseline fails, or a shader [lost more without failing](../reference/shaders.md#losses-a-shader-build-reports)) and `REGRESSION` (new `[RED]` lines, a screenshot, frame time, memory or prepare time beyond the tolerances) exit 1; `ERROR` (the harness could not check the game) exits 2. Failures the baseline has too are listed as known and do not fail the check. Changed MSL is expected after a chain change: say in the commit message why it changed. A screenshot difference may be a fix: look at the run's screenshots before calling it either. The report holds only numbers, hashes, shader keys and normalized `[RED]` texts.

## 5. Continuous integration

The workflows in `.github/workflows` run on GitHub, on pull requests and pushes to `main`. This fork has no remote yet, so nothing runs them: the local commands of the next section are the check.

- `tests.yml`: the host's unit tests and ThreadSanitizer targets; the Rust library's tests and lints; the host and CPU texture tests again under ASan, UBSan and TSan (a ratchet over the findings listed in `dm/sanitizers/`: `make -C dm/host/tests asan tsan-threads` and `make -C dm/gfx/tex/tests asan` locally); line coverage of those tests and the tool tests (report only; `make coverage` in both folders and `dm/tools/coverage_report.py` locally); the CPU-only graphics tests; the Python tool tests (`dm/tools/tests`, `dm/gfx/tests`), including the translator's synthetic-shader tests; a Mac host build, and every graphics test compiled ([Continuous integration](../development.md#continuous-integration)).
- `lint-python.yml` and `lint-c.yml`: the ratchets of `dm/tools/lint_python.py` and `dm/tools/lint_c.py`.
- `fuzz.yml` fuzzes the file parsers nightly from synthetic seeds; a change to a parser in `romfs.py`, `nacp.py`, `nso.py` or `shader_split.py` can run its harness locally (`dm/tools/fuzz/fuzz_<parser>.py --time 300`).

GitHub's Macs have no Metal 4 and no game files, so these stay local in any case: `make -C dm/gfx/tests run` (the Metal graphics tests), the translator tests with `DMSH_TEST_METAL=1` / `DMSH_TEST_GPU=1`, and everything `regress.py` does.

## 6. Before you commit

This fork has no remote and no pull requests yet: a change is one local commit per verified stage, made when the user asks, and not pushed ([AGENTS.md](../../AGENTS.md)). What to check, by what the change touches (the largest that applies):

- **Docs only:** `python3 -m unittest dm/tools/tests/test_docs.py`; after a change to `docs/reference`, `python3 dm/tools/docs_symptoms.py`.
- **A Python tool or script** (`scripts/`, the tools under `dm/`): the Python tests, each file on its own as CI runs them (`for f in dm/tools/tests/test_*.py dm/gfx/tests/test_*.py; do python3 -m unittest "$f"; done`), and the lint ratchet, ruff and pyright with no new finding: `<venv>/bin/python3 dm/tools/lint_python.py`, from a venv with `.github/requirements/lint.txt` installed ([Continuous integration](../development.md#continuous-integration)). The system Python has neither tool, and the check stops at a `[RED]` line there: that is not a pass. A generated page (`docs_*.py`) is compared on a clean checkout, so it must not read untracked or git-ignored files such as `dm/host/gen`.
- **The profile or the note** (`dm/games/0400486024DEC000/`): the Python tests; a full shader build after a change to the profile's shader inputs; the game on the Mac along its route, with `scripts/bench.py` numbers when the change is about speed; the finding in the game's note ([AGENTS.md](../../AGENTS.md)).
- **The host, the chain or the tools** (`dm/host`, `dm/gfx`, `dm/tools`, `dm/gfx/tools`, `scripts`): everything above, then
  - the host build, `make -C dm/host dmhost`;
  - after a change to the Rust library (`dm/host/rust`), its tests, lints and Miri there ([Rust in the host](../development.md#rust-in-the-host));
  - the host tests, `make -C dm/host/tests run`;
  - the graphics tests, `make -C dm/gfx/tests run` (they need this Mac's GPU);
  - `dmsh_chain.py --header` after a chain change;
  - `python3 scripts/regress.py check` (`--quick` while iterating, the full run before the commit), and a run of the game.

  Say in the commit message what was run and what was not, with a word on every `CHANGED`, `NEW-FAIL` or `REGRESSION` of the report; a path this Mac cannot exercise is named as untested. After a change to C or Objective-C in `dm/host` or `dm/gfx`, `dm/tools/lint_c.py compdb` and `tidy` with LLVM's release run what the `lint-c` workflow runs: no new finding (fix it, or `// NOLINT(<check>)` with a reason on a false positive), and `tidy --update` after fixing one ([Continuous integration](../development.md#continuous-integration)).

## 7. Commit messages

A commit is read as one line in `git log --oneline`, so that line says where the change lands and what it does, in the form most systems projects use (Linux, Git, Go, QEMU, Wine), and the full account goes below it:

```
host/nvn: no pipeline for a dispatch the host draws itself

(the symptom, the cause, the change, what was checked and what was not)
```

- **The first line** is `area/component: summary` (the component is optional), at most 72 characters, without a full stop; the summary starts lowercase unless its first word is a name.
- **The area** is where the change lands, that is where the cause was fixed. Take the names the history already uses (`git log --oneline`: `host`, `host/nvn`, `host/movie`, `gfx/tools`, `tools`, `tools/rewriter`, `games/oot`). The tests, reference entries and regenerated files that go with a change do not count, and a commit that holds two independent changes is two commits.
- **The component** comes from the code, never from a list: the service's file in the host (`hle_movie.c`: `host/movie`), the tool (`tools/rewriter`). When unsure, use what `git log --no-merges -- <files>` already shows.
- **No kind prefix** (`fix:`, `feat:`, `ADD:`): the summary's verb says what kind of change it is, and the body says whether it fixes the cause or works around it.
- **The body**, after a blank line, is as long as it needs. Lines of their own at the end: `Front-ends:` for a change a front end has to act on ([Following main](../frontends.md#following-main)), then `Co-authored-by:`.
