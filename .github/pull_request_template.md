<!-- The title is the commit summary: Conventional Commits, `type(scope): summary`, never a `game` or `oot` scope. CONTRIBUTING.md says how a change gets in. -->

## Why

<!-- The cause it fixes, or what it is for. Link the issue. -->

## Checked

<!-- The host build, the tests the change touches, a run of the game: which version, which Mac, which route. Name what could not be run. -->

- [ ] Nothing from the game is committed (RomFS, code modules, shaders, dumps).
- [ ] A change to a shader chain tool comes with `dm/gfx/nvn/dmsh_chain.h` (`python3 dm/gfx/tools/dmsh_chain.py --header`).
- [ ] A change a front end must act on has a `Front-ends:` line in its commit message.
