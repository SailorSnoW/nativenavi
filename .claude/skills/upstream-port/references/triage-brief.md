# The brief for a triage subagent

Give each subagent one area's commits and this brief, filled in. It knows only
what the brief says, so spell out the paths. It must not change anything.

```
You are analysing upstream commits of nxnative for a hard fork of it. READ-ONLY task: do not edit, stage or commit anything in <repo>
(another session may have uncommitted work there). Write scratch files only under <scratchpad>.

## Context
- <repo> ("oot-silicon") is a hard fork of nxnative (Switch games run natively on Apple silicon: Horizon SVCs and services in dm/host, NVN on
  Metal in dm/gfx/nvn, the shader translator chain in dm/gfx/tools, the module rewriter dm/tools/patch_nso.py) reduced to ONE game: The Legend of
  Zelda: Ocarina of Time for Switch 2 (title 0400486024DEC000, profile dm/games/0400486024DEC000/: game.tsv, patches.py, README.md = the game
  note), Mac only. Read its AGENTS.md first.
- Removed by the fork: iOS, Vulkan, OpenGL, every other game's profile, the cheat engine, iCloud save sync, NRO / nn::ro, the web applet, the
  software keyboard, irs; in NVN ray tracing, transform feedback, tessellation and the geometry stage (mesh / task draws are in
  dm/gfx/nvn/nvn_mesh.inc), the Switch 1 paths; in the translator what the game's shaders do not use. `git -C <repo> log --oneline` and the
  removal commits' messages say what went and why.
- The game: 42,847 shaders (22,352 pixel, 15,850 vertex, 4,125 compute, 500 mesh, 20 task; no geometry, no tessellation, NVN2 only); modules main,
  subsdk0 (movies, all AV1) and sdk; its profile keys in game.tsv (enc_thread on, replay_ahead on, fps_target 60, the frame step stub).
- Goals: the game as well as possible on a Mac (frame pacing, power, load times, memory, the picture), a faithful copy (its own resolution and
  60 fps; no mode above them as a goal), root-cause fixes, no workarounds. The user's Mac: <model, memory>.
- The commits are in the mirror <mirror> (blobless: `git -C <mirror> show <sha>` fetches contents as it reads them; skip binary diffs).
  `python3 <repo>/dm/tools/upstream_sync.py list` tags their files (gone = not in this tree).
- Already reviewed: <repo>/dm/tools/upstream_ledger.tsv (some commits build on ones listed there, or on ones never taken: say which).
- Game files, for evidence (read only, write nothing there): <build folder> (shader libraries, extracted modules, logs); the run log
  ~/Library/Logs/nxnative/0400486024DEC000.log.

## Your commits (<area>)
<sha subject, one per line, with what to look at for each: "does the game use X?", "is Y still in this tree?">

## For each commit report
1. What it changes and why: the bug or the cost, the game it was found on, the numbers.
2. Whether it applies here. Is the code it touches present (cite path:line), removed, or already here under another shape? Does it depend on a
   commit this tree does not have? Does this game reach the path (its shaders, its modules' imports, its note, its log)? Is the measurement
   from another device, and does the same mechanism exist on the Mac?
3. Verdict: ported / partial (which parts) / docs / no-op / skip / deferred, with the benefit for the game on the Mac.
4. Effort and risk: the diff's size, the conflicts with the fork, a chain change (every shader translated again), a rewriter change (modules
   rewritten), the tests to bring along.

End with a short ranked list of the most valuable items. Keep the report under about 1,200 words, with SHAs and file paths.
```

Areas that worked:
- the game's own commits and the docs;
- NVN memory, residency and textures;
- submission, presentation, the frame step and the window;
- the shader chain, packing, the rewriter and the host's services.

Give the docs commits' agent the instruction to read the README diffs in full: the findings matter more than the diff.
