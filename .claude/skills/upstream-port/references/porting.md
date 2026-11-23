# Porting: checks by area, the game run, and the traps

## Checks by area

| The commit touches | Per commit | Notes |
|---|---|---|
| `dm/gfx/nvn` | host build; the graphics tests it adds or changes, and those of its subsystem (`make -C dm/gfx/tests t_x && make check_t_x`: the `check_` target runs the modes the Makefile gives it) | the whole suite at the end with `-k -j8`; without `-j` it builds every test one after another and takes far longer than an hour |
| `dm/host` | host build; `make -C dm/host/tests build/t_x` and run it; the whole `make -C dm/host/tests run` once a stage | `make tsan-threads` for threads, locks or clocks |
| `dm/gfx/tools`, the chain | the chain's Python tests; `dmsh_chain.py --header`, then `test_nsp_chain` | a chain change: group it, tell the user the app needs a rebuild |
| `dm/games/0400486024DEC000/patches.py` | `test_ocarina_*`, `test_patches`; rewrite the three modules and check the stubs' sites in `main.cuts.tsv` and the capabilities in `main.fixups.tsv`; `docs_patches.py` | disassemble a changed stub (capstone in `nix develop`) |
| `docs/reference` | `docs_symptoms.py`, `test_docs` | |
| a `DM_*` switch | `docs/env_switches.tsv` row, `test_env_switches` | |
| a launcher setting | `dm/games/settings.tsv`, `docs/settings.md`, `test_launcher_settings` | |
| a Python tool | its test; `lint_python.py` in `nix develop .#lint` | |

Run every suite inside `nix develop` (the session's plain shell has Python 3.9
and no cargo). A pipeline such as `nix develop -c bash -c '…; python3 -m
unittest x | tail -1'` exits with tail's status, so an `&& git commit` after it
commits a failing tree. Read the test output before committing. When
uncommitted work of a later port is in the tree, check the committed state
with `git stash`, run the tests, then `git stash pop`, before you say in a
message that the tests passed.

## A headless run of the game

On this Mac the build folder (`~/Documents/0400486024DEC000_build`) has the
player's helper `oot-silicon-ref/gamerun.sh <out> <seconds> <host> <modules>
[KEY=VALUE…]`. It runs the host on a copy of the player's save with the app's
RomFS, fonts and pipeline archive, and has a memory guard. Run it with:

- `DM_MSL_OVERRIDE=<build>/shader/cache` and `DM_SHMAP_OVERRIDE=<repo>/dm/games/0400486024DEC000/shader_map.tsv`, so that it reads the libraries of this checkout's chain;
- the modules rewritten by the tree under test (`oot-silicon-ref/rewrite.sh <tree> <out>`);
- a route, such as `DM_KEYS="35:A,40:A,43:A,105:LUp~4,111:LDown~3,116:LUp~4,122:LDown~3"`: the title, the file, a walk (in a Castle Town house on the player's save; its GPU time follows where the camera looks, about 20 or 29 ms a window, so it decides no A/B on GPU time).

Copy each host you run (`cp dm/host/dmhost <scratch>/hosts/<name>`), so a
later build cannot change it under a run. Read the log with
`upstream_sync.py summary <out>/log.txt --frames 3000,4200`, and look at the
`shot` screenshots. Without these helpers: `scripts/build_app.py <NUP>
--platform mac` prints the command that runs the game, and
[06-dev-loop.md](../../../../docs/agents/06-dev-loop.md) lists the switches.

Before a run, wait until no other `dmhost` runs (`pgrep -f '/dmhost( |$)'`):
another session's game makes the GPU figures meaningless and swaps a 16 GB Mac.
Don't run the graphics suite, a rewrite or `lint_c` during an A/B either.

A chain change: the host then loads only libraries of the new chain. To run
the game before the rebuild, build a test host with the previous
`dmsh_chain.h` (`git show <previous>:dm/gfx/nvn/dmsh_chain.h`), copy it, and
put the new header back. This holds only when the chain change leaves the
translation as it was, a change to `dmsh_build.py`'s publishing for instance.
Say so in the message.

## Traps met so far

- **The fork's own code under the merge.** The perf branch changed code upstream changed too: the wide GPU writes kept as intervals, the texture view cache by Metal texture, the replay-ahead holds (`hold_op`) around commits. Merge both intents and say which constants you kept.
- **Tests that pass upstream fail here.** `t_pkg_evict` waited one submission too few for a release the fork's deferred drain (a serial queue: `defer_settle`) gives later. `t_nvn`'s settle cases never moved the stub's step count, which main's frame advance (`nvn_fheld_judged`) reads. Find why before changing a test, and change the test only where it was wrong.
- **Upstream's own bugs.** `pkg_evict_idle` dropped view-cache entries by serial from a map keyed by Metal texture. Fix it and say so in the message.
- **Upstream code for other platforms.** Remove `TARGET_OS_IPHONE` branches, iOS defaults (trimming at warn), `os_proc_available_memory` and other games' links. Keep the provenance ("in nxnative, on an iPhone 17").
- **Commits that need code this tree lacks.** Port the part that stands alone, and record the rest as `partial` or `deferred` with what it needs (a gap commit, a removed subsystem).
- **A finding on another device.** Find the mechanism on the Mac's path before porting a fix for it, and measure it here when you can (the residency set, the GPU time per frame). Write which figures are upstream's and which are this Mac's.
- **Generated files.** `symptoms.md`, `game-patches.md` and `dmsh_chain.h` conflict on every merge. `pick` leaves them alone; regenerate them.
- **Untested hardware.** A Pro Controller, an external or variable-refresh screen, VideoToolbox AV1 (M3 and later), a real memory-pressure event, an 8 GB Mac. Name them as untested; don't present a test run in their place.
