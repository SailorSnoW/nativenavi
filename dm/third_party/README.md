# dm/third_party: third-party code

This is code written by others, shipped with the repository so there is nothing to download. Each component keeps its original license, and the license file sits in its own directory.

| Directory | What it is | Version and changes | Where it is used | License file |
|---|---|---|---|---|
| `dav1d/` | The AV1 decoder dav1d: the C sources, headers and arm64 NEON assembly an arm64 build compiles, with the configuration meson generates in `config/` and per-bit-depth wrappers of its templates in `config/tmpl/` | 1.5.4 (upstream commit `54706fc`), unmodified | Host: decodes the games' AV1 movies where VideoToolbox has no AV1 decoder (`dm/host/hle_movie_av1.c`). `dm/host/Makefile` builds it into `gen/dav1d/libdav1d.a` | `dav1d/COPYING` (BSD 2-Clause) |
| `ryujinx/` | The shader translation library from Ryujinx, three projects: `Ryujinx.Graphics.Shader`, `Ryujinx.Common`, `Spv.Generator` | Ryujinx 1.3.2, with the local changes listed in `ryujinx/README.md` | Referenced by the shader translation tool `dm/gfx/tools/dmsh_xlate`, which translates the original Switch shaders to SPIR-V | `ryujinx/LICENSE.txt` (MIT); `ryujinx/src/Spv.Generator/LICENSE` is that subproject's own license text |

A few more notes:

- `dav1d/README.md` and `ryujinx/README.md` record where each came from and what it is used for.
- Four pieces of third-party code live outside this directory: `dm/gfx/nvn/include/nvn.h` (MIT, author named in the file header), the layout size algorithm in `dm/gfx/nvn/nvn_tex.c`, ported from deko3d (zlib license), and two adapted from MacNativeGames (MIT, license text in `macnativegames/LICENSE`): in `dm/gfx/nvn/nvn_mtl.m` the GPU widening of 8-bit indices (`wide_*`) and the in-pass clear triangles (`clear_color_tri` / `clear_ds_tri`), from its `widen_u8_gpu` and `clear_quad` / `clear_ds_quad`, and the Tears of the Kingdom visibility-counter lowering in `dm/gfx/tools/dmsh_xlate/Program.cs` (`VertexCounterLowering`).
