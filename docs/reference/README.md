# Subsystem references

How the host meets each part of the game, and every failure met so far, one entry per mechanism: symptom, cause, fix (file and function), where it was found. This project is a fork of nxnative, which runs many Switch games: an entry found there on another game is kept when the code it explains is still here, with that game named in plain text; the entries of the subsystems this fork removed (Vulkan, OpenGL, iOS, other engines' containers and allocators, the older movie decoders) are gone. Start from the [symptom index](../agents/symptoms.md) when you have a log line or a visible result; the workflows that lead here are in [docs/agents](../agents/README.md).

| File | Covers |
|---|---|
| [packing.md](packing.md) | The packing script: the NUP, build tools, RomFS images, the launcher's metadata, external drives, derived profiles |
| [rewriter.md](rewriter.md) | The NSO rewriter `patch_nso.py`: jump tables, exclusive windows, patch area reach, cuts and observation gates, functions without unwind entries, stack and system registers, relocations |
| [memory.md](memory.md) | Guest layout, heap and application pool, virtual address memory and the alias region (the SDK's node area), engine allocators, mapping SVCs, running out of memory |
| [threads.md](threads.md) | Thread creation and cores, address arbitration, yields and spin waits, events |
| [services.md](services.md) | SDK calls and system services: sm connections, file system and saves, add-on content, input, applets, display and operation mode, accounts and network, settings and fonts, C library and time, modules |
| [movies.md](movies.md) | The Switch 2's `nn::codec` AV1 decoder on VideoToolbox and dav1d, and the game's movie player |
| [audio.md](audio.md) | The audio renderer and its command lists by SDK, AudioOut and devices, codecs, timing |
| [frame-rate.md](frame-rate.md) | The host's frame step, engine frame limiters, present intervals and display rates, CPU cost |
| [nvn-objects.md](nvn-objects.md) | NVN object sizes, memory pools, command memory, queues and syncs, CPU-written textures, NVN entry points |
| [rendering.md](rendering.md) | The replay on Metal: textures, passes and clears, blend / depth / stencil, vertex input, uniforms and ordering, conditional rendering |
| [shaders.md](shaders.md) | Shader containers, control sections and code, translation, bindings and handles, pipelines and caches |
| [game-state.md](game-state.md) | Game behaviour that looks like a host bug but is not |

A new general finding goes into the file and section it belongs to, in the same form, with a line in the symptom index; a fact about one game goes into that game's note.
