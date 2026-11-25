---
name: upstream-port
description: Fetch the new commits of upstream nxnative (github.com/seedds/nxnative), sort out which ones help Ocarina of Time on a Mac in this fork, and port them the way this repository ports them (three-way merge, adaptation to what the fork removed, per-commit checks and commits, game runs, merge into main). Use it whenever the user mentions upstream, nxnative, seedds, "nouveaux commits", syncing or comparing with the fork (SailorSnoW/nxnative), cherry-picking or taking changes from nxnative, or asks what upstream did lately, even if they only want an analysis and no port yet.
---

# Porting upstream nxnative commits into NativeNavi

NativeNavi is a hard fork of nxnative. It runs one game (Ocarina of Time,
profile `dm/games/0400486024DEC000`), on Mac only, with iOS, Vulkan, OpenGL,
the other games and many subsystems removed (see [AGENTS.md](../../../AGENTS.md)).
Upstream keeps moving, and a few of its commits each day help this game. This
skill takes them over without losing what the fork changed.

The state lives in three places. Read them before doing anything:

- `dm/tools/upstream_sync.py`: fetch, list, show, pick, summary, record (its top comment lists them).
- `dm/tools/upstream_ledger.tsv`: every upstream commit already reviewed, with its verdict, the local commits that took it, and why. Its `# base` is the nxnative commit this repository was imported from.
- The game's note and `docs/reference`: where the findings of ported commits go (AGENTS.md says where each kind goes).

Talk to the user in their language (this user writes French). The repository's
files, commits and code comments stay in English.

## 0. Before starting

- Read AGENTS.md and `docs/agents/06-dev-loop.md`. They are the rules the ports must follow.
- Run `git status` in the main checkout and `ListAgents`. Another session may be editing the same files in the main tree; if anything is uncommitted there, or might be, work in a git worktree of your own (`git worktree add -b port/upstream-<date> <scratchpad>/wt main`). Never discard or stage another session's changes: a revert of yours goes by `git apply -R` of your own diff, not by `git checkout --`.
- Check the disk (`df -h ~`). A change to the shader chain writes a new compile cache and a new library, about 9 GB each, beside the old ones, and the app rebuild needs room on top.

## 1. Fetch and list

```
nix develop -c python3 dm/tools/upstream_sync.py fetch
nix develop -c python3 dm/tools/upstream_sync.py list              # everything after the ledger's base that it has no row for
nix develop -c python3 dm/tools/upstream_sync.py list --since <sha> # e.g. the user's fork's main, when they ask for "fork main...upstream main"
```

`list` tags each file: `gone` (not in this tree), `add`, `oot`,
`game:<id>` (another game), `ios` / `gl` / `vk`, `chain` (a shader chain file:
porting it means retranslating every shader), `generated` (an index a tool
writes), and the commit gets `skip?` when every file is away from this fork.
A `skip?` commit still gets its message read: a game port can carry a host fix
for every game.

The ledger's base is the import (`2edad182`), so the first unreviewed commits
may be older than the user expects: in November 2026 the 86 commits between
the import and the fork's main `6c50d74e` were never reviewed (except
`f7ae8a3a`). Say how many there are and ask whether to include them, rather
than silently doing either.

## 2. Triage

For each commit, decide one verdict, with the reason in terms of this game on a
Mac: frame pacing, power, load times, memory, picture, stability.

- `ported`: the commit does what it says for this game.
- `partial`: only some of its parts help. Name them.
- `docs`: a finding to record, with no code to take.
- `no-op`: it would do nothing here. Prove it from the code.
- `skip`: another game, iOS, a removed subsystem, or a path the game never reaches. Give the evidence.
- `deferred`: wanted, but waiting for something, such as a chain change to group with.

With more than about ten commits that need reading, split them by area (memory
and textures, submission and presentation, shader chain and packing, host
services, the game's profile and docs) and give each area to a subagent at
once. Use the brief in
[references/triage-brief.md](references/triage-brief.md), which says what the
fork removed and what to report. Then check the claims your decisions rest on
yourself, by reading the code on this tree's path. Two examples from the first
run of this skill:

- An "8 GiB alias region fixes a story blocker" commit did nothing on the Mac: the host already gives the Switch's 64 GiB there.
- "317 MB less wired" was measured on an iPhone, but the same pools did stay in the Mac's residency set, so that one held.

Show the user a ranked list (what, why for OoT, effort and risk, chain or
not) and the skip groups with one line each. Port only what they agree to. The
user wants a copy faithful to the game, so flag any mode that renders above the
game's resolution or above 60 fps: such a mode is not a goal. A reduction that
is off by default was accepted once (Render scale).

## 3. Port, one upstream commit at a time

In dependency order (a commit that builds on another goes after it), in your
worktree:

1. `upstream_sync.py show <sha>` for the message and stat, then `upstream_sync.py pick <sha> [paths…] --tree <worktree>`. `pick` merges each file three ways, writes added files, lists the files this tree lacks, and names the generated files to regenerate rather than merge. Only take the paths that belong to the parts you port.
2. Resolve the conflicts by hand, in the fork's terms:
   - no `TARGET_OS_IPHONE` branches;
   - nothing that only another game uses;
   - links to other games' notes become plain text;
   - reference entries say they were found upstream and name its commit (`upstream nxnative <sha>`);
   - comments say what is true here.

   When upstream builds on code this tree removed or changed (the perf branch's code, the removed geometry stage), adapt the change to this tree. Don't bring back the removed code.
3. Regenerate what tools write: `docs_symptoms.py` after a `docs/reference` change, `docs_patches.py` after a `patches.py` change, `dmsh_chain.py --header` after a chain change. After a `patches.py` change, also rewrite the modules and check that the sites matched.
4. Check the commit: the host build and the tests of what it touches (its own tests, and those of its subsystem). The full graphics suite is long: run it with `make -k -j8 -C dm/gfx/tests run` at the end, not per commit. See [references/porting.md](references/porting.md) for the checks by area and the traps.
5. Commit it on its own, with the message format of `docs/agents/06-dev-loop.md` §7:
   - `type(scope): summary` (Conventional Commits);
   - a body of a line or two only when the summary does not say why;
   - the trailers `Upstream: nxnative <sha>`, then upstream's `Co-authored-by` lines; no LLM co-author or session line.

   What ran and what did not goes in the report to the user; measurements go in the game's note.
6. `upstream_sync.py record <sha> <verdict> --local <commit>[,<commit>] --why "<one line>"`. Record the skips and no-ops of the triage too, so that the next run does not list them again.

Group chain changes: each one retranslates about 42,850 shaders. A chain change
leaves the host unable to load the present libraries until the app is rebuilt.
Tell the user, and give a game run before the rebuild a host built with the
previous `dmsh_chain.h` (see porting.md).

## 4. Check the whole stage on the game

At each stage's end (memory, presentation, profile…) and once at the end,
check on the game, headless:

- Build the host once at the branch's base for a baseline run, then run the same route with each stage's host. The route: the title, the file, a walk. [references/porting.md](references/porting.md) has the recipe.
- `upstream_sync.py summary <log> --frames 3000,4200` reads a run log: red lines, frame rate, GPU time, another `dmhost` on the GPU, the residency set.
- Wait for no other `dmhost` before a run, and rerun a run another host shared the GPU with.
- For a commit that claims speed, run an A/B: the same route, the host before and after, nothing heavy on the CPU meanwhile. Say plainly when it shows no difference.
- Look at the screenshots after a change that can affect the picture.

At the end, on the final tree:

- `make -C dm/host/tests run` and `make tsan-threads`;
- the full graphics suite;
- every Python test file on its own;
- `lint_python.py` in `nix develop .#lint`;
- `lint_c.py` compdb and tidy, compared with the same lint on main: report only what is new.

Record the findings each port measured here (residency, fps) in the game's
note, in the section they belong to, and keep upstream's own measurements as
provenance.

## 5. Merge and report

- In the worktree: `git switch -c merge-main main`, then `git merge --no-ff -m "Merge branch 'port/upstream-<date>'" port/upstream-<date>`. Resolve the conflicts with what main got meanwhile (another session's work: keep both sides' intent, and fix the tests one side broke for the other). Regenerate the indexes, build, and run the tests the conflicts touched and a game run.
- Fast-forward main from the main checkout (`git merge --ff-only merge-main`), and only when its tree is clean. Otherwise ask the user.
- Delete the worktree and the temporary branch. Delete the port branch when the user agrees. Commit `dm/tools/upstream_ledger.tsv` (with the merge or on its own).
- Report to the user:
  - what was taken;
  - what was measured on their Mac;
  - what is untested and why (hardware this Mac lacks, a path a headless run cannot reach);
  - what was left out and why;
  - whether the app must be rebuilt (a chain change), and the disk space that needs.
