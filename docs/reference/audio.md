# Audio

The audio renderer (`dm/host/hle_audren.c`, and `dm/host/rust/src/audren.rs` for what reads the SDK's data), AudioOut, audio devices and codecs. Each entry: symptom, cause, fix, the games it was found on. The [symptom index](../agents/symptoms.md) lists every symptom.

## Audio renderer

### Running the SDK's own renderer on the CPU

Check whether the SDK carries the renderer: list the `sdk` module's exports for `nn::audio::server::` and `nn::audio::dsp::CommandListProcessor`. If they are there (Super Mario Bros. Wonder, SDK 16), do not write a mixer:

- switch a copy of `AudioRendererParameter` to rendering device = CPU (+0x2c = 1) and execution mode = manual (+0x30 = 1) in `GetAudioRendererWorkBufferSize` and both `OpenAudioRenderer` overloads;
- call the SDK originals (`guest_sym` returns the module's own code even for a forced symbol);
- drive `ExecuteAudioRendererRendering` from a guest thread (`dm_guest_thread_spawn`).

The open then goes through `CreateAudioRendererManagerByDfc`; no IPC.
An SDK without the renderer in the process (SDK 1.3.1 has only the client of `audren:u`) is not served: `ar_in_process` logs `[RED] audren: the SDK has no renderer of its own` once, and the work size, open and close calls go to the SDK as the game made them.
For a replaced DSP renderer the size query keeps the service's work-buffer contract; a larger local CPU representation is a backend cost. See [A sound engine's heap too small for its renderer and players](#a-sound-engines-heap-too-small-for-its-renderer-and-players).

### SDK 8's longer `AudioRendererParameter`

- **Symptom:** abort with Result 0x499 in `audio::GetAudioRendererWorkBufferSize`.
- **Cause:** SDK 8's `AudioRendererParameter` is one word longer than SDK 16's: its revision sits at +0x38. The host copies the game's parameter before switching it to CPU rendering with manual execution, and copied only 0x38 bytes, so `CheckValidRevision` read garbage.
- **Fix:** it copies 0x40 now.
- **Found on:** The Legend of Zelda: Link's Awakening (SDK 8.2.0).

### A renderer opened without an event

- **Symptom:** the game is silent; only the game's updates render (catch-up frames that nothing plays). The log has `audren: OpenAudioRenderer (no event)` and no `render thread started`.
- **Cause:** the game opens the renderer with `OpenAudioRenderer(handle, parameter, work, size)`, the form with no system event, and paces its own updates, but the renderer must still run every 5 ms. The host used to start its render thread only for the form with an event.
- **Fix:** both forms go through one open that starts the thread (`hle_audren.c` `ar_open`).
- **Found on:** Balatro (SDK 18.3.2, LÖVE's OpenAL Soft; one boot).

### A game with more than one renderer

- **Symptom:** `[RED] audren: a second renderer (the host drives only one)` with different parameters from the first open; later, `catch-up renders so far` in the thousands (7,625 in 6 minutes), about 10% of the first renderer's frames.
- **Cause:** a game may open several renderers, as on Horizon. The host kept one set of globals: the second renderer got no render thread, but `AudioRenderSystem::Update` is one hook for all of them, so each update of the second renderer found nothing rendered since the last, rendered a catch-up frame of the second renderer and queued it in the first one's output, in place of a frame of its own.
- **Fix:** `hle_audren.c` keeps each renderer's state in `ar_t`: its render thread, output, catch-up queue, counters and upsampler history. Two renderers play through two outputs, which the sound card mixes. An SDK call that names only the `AudioRenderSystem` finds its renderer by `ar_of_sys`. One lock still serializes all rendering, since `capture` keeps what it learns from the first lists (whether one has walked whole, the sink's ID) for all renderers; a frame takes about 0.02 ms. Catch-up renders went from 7,625 to 0 or 1 in a run. `dm/host/tests/t_audren_layout.c` checks the lookup and the upsampler's separate history.
- **Found on:** Mario & Luigi: Brothership (SDK 12.3.2): one renderer at 48 kHz, which sounds during the movie, and Unreal's mixer at 32 kHz.

### Two renderers overwriting each other's queued blocks

- **Symptom:** with two renderers open, one plays single 5 ms frames between silences and its sound effects are missing, while the other has an occasional all-zero 5 ms block; no underrun and no `[RED]` line, and the AudioOut `appended` and `released` counts stay balanced. Cross-correlating the outputs finds one renderer's waveform on the other.
- **Cause:** AudioOut queues a block's pointer and copies its samples when the sound card asks (`h_au_Append`, `pull` in `hle_audio.c`). `render_main` kept its four blocks in function-level statics, left out when the renderer state moved into `ar_t`, so every render thread filled the same four. A block queued for one output could be rewritten by the other renderer's thread before it played. A second renderer's start also set the size of the blocks to 0 under the first one's output, whose `b->size - a->pos` could then underflow on a part-played block.
- **Fix:** the blocks are in `ar_t`, one set per renderer. The slots are static and `CloseAudioRenderer` joins the render thread before a slot is reused, so a block outlives its output. `t_audren_layout` queues every block of one renderer, renders the other, and checks the first one's samples.
- **Found on:** title `01007ED02424E000` (not in the [games table](../../dm/games/README.md#games); [issue #1336](https://github.com/seedds/nxnative/issues/1336), on an iPad Pro M1). Mario & Luigi: Brothership also opens two renderers; it has not been run with the fix.

### The SDK's open with an event calls the open without one (`t_in_open`)

- **Symptom:** `audren: OpenAudioRenderer` logged twice right after the first open, with the same work buffer and "original device 1 mode 1", and a second render thread (until 2026-11-04 the second log line was `[RED] audren: a second renderer`, which now means [more than one renderer](#a-game-with-more-than-one-renderer)).
- **Cause:** not a second renderer: SDK 17.5.3's `OpenAudioRenderer` with an event opens it by calling the form without one through the PLT (`sdk+0x1e489c`), which the loader binds to the host's hook since that form has one ([A renderer opened without an event](#a-renderer-opened-without-an-event)), so `ar_open` ran twice for one renderer. The inner call started the render thread before the outer one had created the event, and the outer call took the renderer for a second one.
- **Fix:** a thread-local flag (`t_in_open`) hands the inner call straight to the SDK with the host's CPU / manual copy of the parameter, and only the outer call starts the thread (`hle_audren.c`).
- **Found on:** ROMANCE OF THE THREE KINGDOMS 8 REMAKE (SDK 17.5.3).

### The renderer event as the service gives it: a kernel event

- **Symptom:** the frame loop stops for good as a level loads while audio keeps rendering. The main thread waits in `os::WaitSystemEvent` under `CAudioMemoryPool::Shutdown`, and a sound thread of higher priority waits on the same condition variable.
- **Cause:** with the DSP service (device 0, what the game asks for) `OpenAudioRenderer` attaches the service's renderer event, a kernel readable event (`AttachReadableHandleToSystemEvent`, auto-clear). With CPU rendering and manual execution, which the host switches to ([Running the SDK's own renderer on the CPU](#running-the-sdks-own-renderer-on-the-cpu)), it creates a local one instead (`CreateSystemEvent(event, AutoClear, false)`, SDK 4: `sdk+0x190b6c`). Signalling a local auto-clear event wakes one waiter, the one of highest priority. A kernel event wakes every waiter, and each one tries `ResetSignal`. Donkey Kong Country: Tropical Freeze waits on the event from two threads: its sound thread (priority 29, `main+0x971e8`) every frame, and the main thread while it detaches an audio memory pool (`main+0x6fa8c`: `RequestDetachMemoryPool`, then wait and update until `IsMemoryPoolAttached` is false). The sound thread took every wake, and the main thread never woke again.
- **Fix:** when the game opened the renderer for the service, the host replaces the SDK's local event with an inter-process auto-clear one (`DestroySystemEvent`, then `hle_bind_system_event(event, 1)` in `ar_open`), and the game's event waits on a copy of its read end. That event is the renderer's own, not the game's ([The renderer event belongs to the renderer, not the game](#the-renderer-event-belongs-to-the-renderer-not-the-game)). The SDK's renderer signals through `os::SignalSystemEvent`, which handles either kind. A game that opens a CPU renderer itself keeps the SDK's local event.
- **Found on:** Donkey Kong Country: Tropical Freeze (SDK 4.4.0): the first level's load after the intro cinematic.

### The renderer event belongs to the renderer, not the game

- **Symptom:** the game exits with 42 on its first rendered audio frame: `Break#1 reason=0` in `os::SignalSystemEvent`, called from the public `ExecuteAudioRendererRendering` on the host's render thread (`render_main`).
- **Cause:** with the DSP service (what the game asks for), no SDK's `OpenAudioRenderer` keeps a pointer to the game's `SystemEventType`. It stores NULL in the handle's event slot and attaches the service's readable handle to the object, so the game may destroy the object or reuse its memory once the open returns. The slot is `handle[1]` in SDKs 3, 5 and 12 (SDK 3 `sdk+0x17e004`, SDK 12 `sdk+0x4d2b4`) and `*handle[1]` in SDK 18. The CPU / manual open the host switches to ([Running the SDK's own renderer on the CPU](#running-the-sdks-own-renderer-on-the-cpu)) stores the object in that slot instead (SDK 3 `sdk+0x17dffc`, SDK 12 `sdk+0x4d274`), and `ExecuteAudioRendererRendering` signals it after every render. LEGO Star Wars: The Skywalker Saga passes an event on its stack, and Little Nightmares reuses the object's memory. The first render found a state byte (+0x28) that was no longer the event's, and the SDK aborted.
- **Fix:** `ar_open` gives the SDK's open an event of the renderer's own (`g_rev` in `hle_audren.c`, alive until `CloseAudioRenderer`, which destroys it), whatever slot the SDK keeps it in. The game's object gets a copy of that kernel event's read end (`hle_dup_readable_handle`), attached as managed and auto-clear, as the service's copied handle is. The renderer never touches the game's object again. The host's binding no longer sits at the game's address either, where a later service event at the same storage would have found it (`hle_bind_system_event` returns early for an address it already holds). Tests: `t_audren_work` (the game's event is overwritten after the open, including the SDK 3 to 12 slot) and `t_system_event` (the copied read end).
- **Found on:** LEGO Star Wars: The Skywalker Saga (`010042D00D900000`, 1.10.0, SDK 12.3.6; issue #632), Little Nightmares (`01002FC00412C000`, 1.0.4, SDK 3.5.1; PR #804), both from provisional profiles. DRAGON BALL FighterZ (`0100A250097F0000`, SDK 4.5.0; issue #771) aborts at the same place, and SDK 5.4's open and `ExecuteAudioRendererRendering` are the same code as SDK 3's and 12's. None of the three is here: the fix awaits their reporters' runs.

### The renderer's update lock (`update_lock`)

- **Symptom:** none of its own; command generation races the game's `Update` on SDKs whose lock is not at SDK 16's offset.
- **Cause:** `AudioRenderSystem::Update` holds a mutex in `AudioRenderSystem`; `GenerateCommand` does not take it, so the host does (the auto-mode system thread does the same in `SendCommandToDsp`). Which mutex depends on the SDK: `nn::os::LockMutex(MutexType *)` at +0x1d0 in SDK 4.4 and at +0x1d8 in 6.4–8.2, `SdkMutexType::Lock` at +0x1d8 in 10.4–11.4, +0x1f0 in 13.3–16.2, +0x1f8 in 17.5 and +0x208 in 18.3 (the `sdk` builds of the games on hand). From SDK 19 `Update` reads the system tick and calls `UpdateImpl`, which takes it ([The update lock read off `UpdateImpl`](#the-update-lock-read-off-updateimpl)). The host took SDK 16's for every SDK before 20, which wrote a lock word into another field and excluded nothing.
- **Fix:** `update_lock` in `hle_audren.c` reads it off the SDK's own `Update`: the register the prologue adds the offset to and moves into x0 before its first `bl`, whose target (through its PLT slot, or straight) must be `SdkMutexType::Lock`. A miss in `Update` goes on to `UpdateImpl`; a miss in both keeps the caller's offset (+0x2d0) and logs a red line. This fork reads only that form: the `nn::os::LockMutex` lock of the SDKs before 10 is not recognized, and neither is their `AudioRenderSystem` ([SDK 20's `AudioRenderSystem` layout](#sdk-20s-audiorendersystem-layout) is the one read). Test: `t_audren_layout`.
- **Found on:** Mario Kart 8 Deluxe (4.4), the first game it was plainly wrong for.
- **Check:** checked with sound on Big Brain Academy: Brain vs. Brain (11.4, +0x1d8), DRAGON QUEST XI S (7.3, `MutexType` +0x1d8) and Super Mario Odyssey (7.3, which is silent for its first minute with either lock).

### The render thread's route through the public `ExecuteAudioRendererRendering` (SDK 20)

- **Symptom:** `CommandListProcessor::Process` crashes now and then (SIGSEGV under `render_main`, once at 60 s and once at 134 s in the shrine); every `RequestUpdate` renders a catch-up frame.
- **Cause:** the public `ExecuteAudioRendererRendering` (`sdk+0x221e90`) reaches `AudioRendererImpl::ExecuteAudioRendererRendering` (`sdk+0x22ca88`, `x0 += 0x60` and a jump through the PLT) through the vtable, so the host's takeover of the `Impl` function was never called. The SDK's own `AudioRenderSystem::ExecuteAudioRendererRendering` ran instead (no update lock), every `RequestUpdate` rendered a catch-up frame because the host's render counter never moved, and the two threads wrote the same command buffer.
- **Fix:** the host takes over `AudioRenderSystem::ExecuteAudioRendererRendering`, the PLT target (`hle_audren.c`); catch-ups fell from one per update to 3 per run. It is the fork's only takeover of the render call: the one of the `Impl` function, which older SDKs reached, is gone.
- **Found on:** The Legend of Zelda: Breath of the Wild (SDK 20).
- **Note:** the same SDK's other `AudioRenderSystem` layout is in [SDK 20's `AudioRenderSystem` layout](#sdk-20s-audiorendersystem-layout).

### A renderer below 48 kHz with no upsampler on the CPU

- **Symptom:** SIGSEGV at address 0x18 in `UpsamplerManager::Allocate` on the render thread.
- **Cause:** a DeviceSink below 48 kHz is upsampled by the SDK's upsampler, and `AudioRenderSystem::SetUpRenderResourceSet` allocates the `UpsamplerManager` only for the DSP (`sdk+0x21eb6c`: rate 0x7d00 and rendering device +0x48 == 0); with the host's CPU rendering the manager pointer (`AudioRenderSystem+0x150`, the render context's +0x20) is null.
- **Fix:** the host answers `Allocate` on a null manager with no upsampler, so the generator skips the upsample command (`sdk+0x225b40`), and resamples the DeviceSink's stereo downmix 3:2 itself (a 16-tap windowed sinc in three phases, `up_3_2` in `audren.rs`), 240 samples per 5 ms frame for the 48 kHz output.
- **Found on:** DRAGON QUEST VII Reimagined: rate 32000, 160 samples per frame, `GenerateSinkCommands` `sdk+0x225a80`.
- **Note:** until then the output code assumed an upsampled buffer at 32 kHz that no game had produced.

### A sound engine's heap too small for its renderer and players

- **Symptom:** the main thread spins at start-up in a resource wait (a `YieldThread` loop, no crash), right after `OpenAudioRenderer`.
- **Cause:** the DSP service and an application's local SDK server use different command/state representations. The host forwarded the local CPU server's work-buffer requirement as the guest's DSP-service requirement. Larger local command structures could then exhaust a fixed sound heap that fits the service's budget. Changing only the device / execution fields in the local sizing routine does not reveal this difference: it still sizes the same compiled structures. The game note records the comparison against Horizon's actual sizing code and the resulting allocation failure.
- **Fix:** `ar_service_work_size` in `hle_audren.c`, through `work_size` in `audren.rs`, computes the Horizon service budget through renderer revision 15, with revision gates for graph/splitter storage, effect states, performance frames and command storage. A DSP caller receives that size. On open the host independently asks the SDK for its CPU/manual requirement and supplies a page-aligned, zeroed backend buffer if it exceeds the caller's capacity; a buffer below the declared service requirement is rejected. Both open forms share the allocation, including a nested no-event open. Failed opens free it; close joins the render thread, closes the SDK renderer, then frees it. Explicit CPU renderers keep their SDK contract. A sufficient caller buffer is reused; revisions beyond the verified service model retain the SDK size. No firmware is loaded or distributed at runtime, and no game's heap constants change.
- **Found on:** Hyrule Warriors: Age of Calamity.
- **Check:** `make -C dm/host/tests GAME=<id> audren-work` checks independently obtained Horizon sizing fixtures, both public parameter layouts, uninitialized padding, unsupported revisions, nested opens, undersized buffers, failure cleanup, caller-owned storage and close lifetime. The sizing implementation also matched 360 seeded parameter sets executed through Horizon's original machine code across REV1–REV15. The game note records the original-heap reproduction and audio check.

## Command lists by SDK

### Incomplete startup lists decide nothing

- **Symptom:** no sound after boot; `audren: no DeviceSink (ID 0x13) in the command list` and a malformed-command error persist even when later command lists are valid.
- **Cause:** the host's render thread can generate commands before the game's first `RequestUpdate`. Mario Kart 8 Deluxe's first list ended in a zeroed third command, so none of the supported header layouts could walk it. `capture` nevertheless latched the default layout and consumed the one-time sink scan. Later SDK 6 headers were read as SDK 8–20 headers, and SDK 4's DeviceSink was never found. Retrying only header detection still leaves the sink scan consumed.
- **Fix:** a list decides nothing unless it walks whole. Until the first list walks, `capture` (`audren.rs`) returns a silent frame before the sink scan and logs `audren: command list N commands long does not walk` once, not as a red line; the first valid list then finds its sink. This fork reads one header layout ([SDK 21's](#sdk-21-dsp-command-header-without-a-magic)), so no layout is selected any more: the rule protects the one-time sink scan and the malformed-list report. `t_audren_layout` checks stereo PCM with valid-first, empty and incomplete startup lists and the SDK's short first lists.
- **Found on:** Mario Kart 8 Deluxe (SDK 4.4.0; [PR #331](https://github.com/seedds/nxnative/pull/331)).

### Malformed later command lists and bounded histogram logging

- **Symptom:** ASan reports a stack-buffer-overflow in `capture` while logging the first full command histogram; a malformed list can also consume sink discovery or return PCM before its invalid tail is checked.
- **Cause:** a short valid list selected the header layout without running the histogram. A later malformed list entered the histogram scan, which stopped at its first invalid command but still logged an uninitialized string or adopted a sink from the valid prefix. Separately, accumulating `snprintf`'s untruncated return value advanced the next write past the 512-byte string.
- **Fix:** validate the entire command list on every capture before histogram, sink discovery or PCM output. Keep discovery pending for rejected lists, initialize the histogram string, and stop appending at the last complete entry that fits.
- **Check:** `t_audren_layout` covers malformed first, middle and trailing commands after the first list that walks, recovery to valid stereo PCM, and 64,000 synthetic commands whose histogram exceeds the log buffer. ASan reproduced both the uninitialized-string read and the truncated-append write before the fix.
- **Found on:** synthetic command lists from the [issue #313 static-analysis audit](https://github.com/seedds/nxnative/issues/313#issuecomment-6543080671); no gameplay failure was attributed to these logging defects.

### The final mix is taken from the DeviceSink's mix buffers

- **Symptom:** the CPU processor drops the final mix.
- **Cause:** `dsp::SendCommandBuffer`, `dsp::Wait` and `dsp::Signal` are stubs in the application build, and DeviceSink (command 0x13 in SDK 16, 0x14 from SDK 20) is a no-op in `CommandListProcessor::Process`.
- **Fix:** take the output after `Process` from the mix buffers the DeviceSink command names (layout in `audren.rs`).

### DeviceSink fields read without a bound

- **Symptom:** none seen: no SDK writes such a list. A DeviceSink input index outside the renderer's mix buffers, or a command with the sink's ID too short for the sink's fields, read memory past the mix buffers or past the command list; now `[RED] audren: DeviceSink input N names mix buffer M, of K: no sound`, `[RED] audren: the DeviceSink command (ID 0x14) is 0x40 bytes, too short for its inputs: no sound` or `[RED] audren: the command list's K mix buffers of N samples at 0x... cannot be read: no sound`, once each, and silent frames.
- **Cause:** the C took the sink's six `s16` input indices as they came (`mix + index × samples`) and read the sink's fields at +0x120 to +0x138 of any enabled command with its ID, whatever its size. The list's header names how many mix buffers there are (+0x10), which the C never read. Ocarina of Time's sdk (22.2.8) shows that it is their count: `CommandListProcessor::Setup` (`sdk+0x25b3f0`) copies it, and `ClearMixBufferCommand` (`sdk+0x259970`) zeroes that many buffers of `samples` words at the list's address every frame. `GetMixBuffer` (`sdk+0x258930`) itself takes a signed index with no bound.
- **Fix:** `capture` moved to Rust (`audren.rs`, [Rust in the host](../development.md#rust-in-the-host)): the mix buffers are one slice of count × samples words, which the FFI entry point makes only for a non-null, word-aligned address, and each input must name a buffer inside it. A sink command must hold its fields. The game's lists play as before: the same first-frame histogram, `DeviceSink inputs 6 (0 1 …)`, now with the header's count, `(78 buffers)`: the game's parameter asks for 72, and the list counts 78.
- **Check:** `audren.rs` `a_sink_input_outside_the_mix_buffers_gives_no_sound`, `a_command_with_the_sinks_id_too_short_for_its_fields_is_not_read`, `random_lists_never_panic`, and Miri over the FFI entry points (`audren_ffi.rs`).
- **Found on:** the move of `capture` to Rust, by reading the C; the buffer count checked in Ocarina of Time's sdk and in its own lists.

### Unaligned command headers during layout probing

- **Symptom:** UBSan reports a misaligned `uint32_t` load in `cmd_ok` while `t_audren_layout` probes SDK 4/6 lists (issue #409).
- **Cause:** interpreting the command ID as another layout's size can advance the candidate pointer off word alignment. Typed header loads then have undefined C behavior even though arm64 supports the machine load. A wrong size could also take the next header outside the generated list.
- **Fix:** `cmd_size` decoded its multibyte wire field with `memcpy`, as `cmd_id` and the magic check did for the layouts that had a 16-bit ID and a magic. `cmd_ok` verified that the complete header and command fit before consuming them; malformed-command diagnostics do not dereference a rejected header. This fork reads only SDK 21's layout and probes none, but a list can still be incomplete and a size in it unaligned: since the readers moved to Rust (`command` in `audren.rs`), every field is read through a bounds-checked little-endian accessor.
- **Check:** `audren.rs` `commands_are_whole_or_refused` checks a deliberately unaligned header, every truncated header/body length and invalid signed sizes; `t_audren_layout` checks stereo PCM, including recovery from incomplete startup lists. The parser's minimum is the 0x10-byte header (`CMD_HEADER`). Its UBSan alignment suppression was removed.
- **Found on:** the sanitizer audit's SDK 4/6 fixtures, representing Mario Kart 8 Deluxe and Fire Emblem: Three Houses; no game crash was attributed to this undefined load.

### SDK 8: command IDs numbered differently

- **Symptom:** no music and no sound effects, only the movies' sound (the log repeats `audren: first-frame commands …` every frame and never prints `DeviceSink inputs`).
- **Cause:** SDK 8 numbers the DSP commands differently: its command list goes 4×6, 7×24, 11×2, 16×1, 21×1 where SDK 20's goes 7×6, 11×24, 15×2, 20×1, 25×1, and the output command (DeviceSink, the same 0x148-byte layout with device name `MainAudioOut`, six inputs) is 0x10, not SDK 16's 0x13. The host never found it and played silence.
- **Fix:** when the expected ID is missing from the first full command list, `capture` (`audren.rs`, then `histogram`) takes the one command of DeviceSink's size with 1 to 6 inputs as the sink and logs the ID it adopted. That size was 0x148 bytes then; this fork reads only SDK 21's layout, where it is 0x140 (`SINK_SIZE`), so the rule now serves a later SDK that renumbers its commands, not SDK 8's own lists. `t_audren_layout`'s `test_sink_id` checks it.
- **Found on:** The Legend of Zelda: Link's Awakening (SDK 8.2.0).

### SDK 20's `AudioRenderSystem` layout

- **Cause:** in SDK 20 the getters the host used (`GetExecutionMode`, `GetRenderingDevice`, `GetCommandBufferWorkBufferSize`) are inlined away, `GenerateCommand` takes and returns a `Span<std::byte>` (the generated list comes back in x0 / x1), the device and mode are words at +0x48 / +0x4c, the command buffer is at +0xf8 / +0x100, and `UpdateImpl` locks the mutex at +0x2d0 instead of +0x1f0. The DeviceSink command became ID 0x14 (SDK 20 inserted a command before it) with the same layout.
- **Fix:** `render_once` (`hle_audren.c`) reads this layout, and in this fork no other: the Span version of `GenerateCommand` is looked up by name, and an SDK without it stops at `audren: missing SDK symbol`.
- **Found on:** The Legend of Zelda: Breath of the Wild.
- **Note:** the render thread's call takes another route in this SDK too: see [The render thread's route through the public `ExecuteAudioRendererRendering` (SDK 20)](#the-render-threads-route-through-the-public-executeaudiorendererrendering-sdk-20); the update lock at +0x2d0 is in [The renderer's update lock (`update_lock`)](#the-renderers-update-lock-update_lock), and SDK 22 moved it ([The update lock read off `UpdateImpl`](#the-update-lock-read-off-updateimpl)).

### The update lock read off `UpdateImpl`

- **Symptom:** `CRASH sig=11 addr=0x20` at `nn::audio::server::AddressInfo::GetReference` (`sdk+0x249874`) on the render thread, under `CommandGenerator::GenerateDataSourceCommand` and `render_once`, minutes into a movie. The `AddressInfo` the crash register points at is all zeros, though the function had just read a non-null memory pool pointer from it. Or `[RED] audren: no update lock read off AudioRenderSystem::Update or UpdateImpl`, on an SDK whose code the scan does not recognize.
- **Cause:** the Span layout took the lock at a fixed +0x2d0, which holds for SDK 19.3 to 21.4. SDK 22 moved it: `UpdateImpl` in 22.2.8 (`sdk+0x238530`) and 22.2.5 locks the `SdkMutexType` at +0x2d8 (`add x19, x0, #0x2d8; …; mov x0, x19; bl SdkMutexType::Lock`), as `SendCommandToDsp`, `Start`, `Stop` and `FinalizeImpl` do. Locked at +0x2d0, the render excluded nothing, and the game's `RequestUpdate` rewrote a voice while its commands were being generated. From SDK 19 `Update` reads the system tick first and calls `UpdateImpl`, so a scan of `Update` finds no lock.
- **Fix:** `update_lock` (`hle_audren.c`) reads `Update`, then `UpdateImpl`, and a miss in both is a red line. The start-up line names what it took: `update lock SdkMutexType at +0x2d8`. Test: `t_audren_layout`.
- **Found on:** The Legend of Zelda: Ocarina of Time (Switch 2, `0400486024DEC000`, update v131072; its profile is in pull request #1299): the second run through its opening movie crashed, and the run with the offset read off `UpdateImpl` played the opening through to Kokiri Forest with sound. Checked statically on the `sdk` builds on hand: SDK 19.3 to 21.4 read +0x2d0, as before. SDK 22.2.0 (Fortune's Weave, EA SPORTS FC 27) is not on hand.

### SDK 21 DSP command header without a magic

- **Symptom:** no sound; red `audren: command 0 in the command list is malformed (first word 0x181901 size 0)`.
- **Cause:** SDK 21's DSP command header has no magic: +0x00 enabled (bit 0), +0x01 command ID, +0x02 size (s16), +0x04 estimated time, +0x08 node ID (`CommandListProcessor::Process`, `sdk+0x24fd50`), and every command body moved 8 bytes forward. DeviceSink is still ID 0x14 but 0x140 bytes: input count +0x128, input indices +0x12c, upsample buffer +0x120 (written at `sdk+0x23a7b0`). `AudioRenderSystem` keeps SDK 20's offsets (update lock +0x2d0).
- **Fix:** `capture` (`audren.rs`) reads this layout, each walk bounded by the generated list's length: `command` for the header, `SINK_SIZE`, `SINK_UPSAMPLE`, `SINK_INPUTS` and `SINK_INDEX` for the DeviceSink. It is the fork's only layout (SDK 21 on); the walks of the layouts with a magic are gone, so an SDK 20 list [does not walk](#incomplete-startup-lists-decide-nothing) and plays silence. Its first lists hold 4 commands or fewer, and their sink plays before the first full list's sink scan.
- **Found on:** Rhythm Heaven Groove (SDK 21.4.0, one boot).

## AudioOut and devices

### An AudioOut opened at a rate other than 48 kHz

- **Symptom:** the game quits before its first frame with `FATAL AudioOut sample rate 44100 not supported (48 kHz only)`, `CRASH sig=6` in `abort` under `au_open_at`, exit 134. Nothing is drawn.
- **Cause:** audout takes only 0 (the default) or 48000 in `AudioOutParameter.sampleRate` and refuses any other rate with `ResultInvalidSampleRate`, 0x699 (module 153, description 3; yuzu's `audio_out_system.cpp` and Ryujinx's `AudioOutputSystem.cs` agree). A game written for that asks for its own rate first and opens again with the defaults when refused, then reads the output's rate back. The host stopped the game instead of refusing the open.
- **Fix:** `au_open_at` refuses another rate as audout does: a `[RED]` line with the rate and the call site, and 0x699, before anything is allocated. The game then runs the path it runs on a console. Accepting the rate and converting it instead would run a path no console runs.
- **Found on:** Mega Man Legacy Collection (SDK 3.7.0): `main+0x9a3c8` opens at 44.1 kHz, `main+0x9a3cc` tests the result and opens again with a zeroed parameter, then reads the output's properties for its buffer sizes; it plays at 48 kHz with 0 underruns. Reported in [PR #2723](https://github.com/seedds/nxnative/pull/2723), which accepted the rate.
- **Check:** `t_audio`'s `test_rate_refused`.

### An AudioOut or OutputStream opened with an unsupported channel count

- **Symptom:** `[RED] AudioOut channel count N: audout takes 1, 2 or 6, the open fails with 0x1499`, or `[RED] OutputStream channel count N: not supported (1, 2 or 6), the open fails`. Before the refusal, an output opened with more than eight channels wrote past a stack block on the silent sink's thread (`null_sink`) once it started.
- **Cause:** the silent sink pulls each 5 ms block into a scratch block sized for `DM_AUDIO_SINK_CHANNELS` (8: the renderer's six channels and their stereo fold) but pulled as many channels as the output has, and the open took whatever count the guest's `AudioOutParameter` or `StreamFormat` named. audout itself takes 1, 2 or 6 channels (0 in the parameter is the default, 2) and refuses any other count with `ResultInvalidChannelCount`, 0x1499 (module 153, description 10).
- **Fix:** `au_open_at` refuses a guest count other than 1, 2 or 6 (`au_channels_ok`) as audout does: a `[RED]` line with the count and the call site, and 0x1499, before anything is allocated; 0 is still stereo. `os_open` refuses the same counts for an OutputStream, 0 included, with a `[RED]` line and a failure, as it does for another sample format or rate (what the SDK answers there is not known). The renderer's own output keeps its eight channels, and a `_Static_assert` holds the sink's block to at least six.
- **Found on:** the audit of this fork; no game triggered it.
- **Check:** `t_audio`'s `test_channels_refused` and `t_output_stream`.

### Periodic diagnostics outside the AudioUnit callback

- **Symptom:** RealtimeSanitizer reports stdio locking in `au_render` at its 2,000-callback diagnostic boundary. A busy log writer can block that callback even when its PCM queue is full.
- **Cause:** `LOG` takes `stderr`'s stream lock and formats its message on the calling thread. The callback previously called it directly for its frame-count, queue-depth and underrun statistics.
- **Fix:** the callback publishes a bounded snapshot for the existing audio watchdog, which copies ready reports under the output-registry lock and writes them after unlocking. Its local copies own every log argument; a slow writer holds neither an output pointer nor the registry lock. The single-slot mailbox uses release/acquire ownership; a busy writer leaves the callback accumulating later windows instead of waiting or overwriting unread data. Reports retain the output ID, total callbacks, frame range, average queue depth and underrun/zero-padding counts, with the actual window length when several windows merge. The release-event notifier remains independent of diagnostics, so an event-driven game can keep refilling PCM while logging is blocked. Close stops the producer and takes any remaining snapshot under the same registry lock before freeing the output.
- **Found on:** the [synthetic real-time audio audit in issue #313](https://github.com/seedds/nxnative/issues/313#issuecomment-6523656102). No particular game's audible failure is attributed to this finding.
- **Check:** `t_audio_stats` calls the real callback with synthetic PCM and traps callback-thread stdio, including its stream lock. With the watchdog's writer deliberately blocked, each PCM buffer still triggers its guest release event and is refilled correctly; queued and merged reports preserve their fields. Close drains a second pending snapshot while the watchdog holds only a local copy, which it logs after the output was freed. Statistics alone cause no guest event. The fixture needs no sound device or game files.

### AudioOut release notification can sleep with pending work

- **Symptom:** PCM is consumed correctly and its buffer reaches the released queue, but the guest release event never arrives. The notifier is asleep with `notify_pending=1` and `notify_armed=0`.
- **Cause:** the notifier cleared `notify_armed` with a release-only store before checking `notify_pending`. A producer could publish pending work and see the old armed value, leaving the notifier to wake itself; without acquiring that producer's exchange, the notifier could read stale pending state and sleep. Apple-silicon code generated a store-release followed by acquire-PC loads, which permit this ordering.
- **Fix:** disarm with an acquire-release exchange. If the producer's armed exchange precedes it, the notifier acquires the pending publication. Otherwise the producer sees zero and posts, or finds the notifier already re-armed with its own semaphore post. No timeout, polling, callback logging or audio pacing changes are needed.
- **Found on:** shared CI's `t_audio_stats` ASan/UBSan fixture, reproduced locally without a sound device or game. This is a general AudioOut mechanism; no gameplay reproduction is claimed.
- **Check:** the existing blocked-diagnostics cases remain intact. The fixture also renders and individually reclaims 100,000 event-driven PCM buffers, so releases cannot be hidden by bulk notification coalescing. The old notifier naturally strands a release in this case; diagnostics distinguish event delivery, PCM contents and returned-buffer identity without retrying the failed wait.

### Stopping an AudioOut releases its pending buffers

- **Symptom:** FMOD waits for buffer release after stopping an AudioOut, hanging the game during a scene transition.
- **Cause:** the host stopped its backend but left unplayed and partially played buffers in the pending queue, so no callback could release them.
- **Fix:** after stopping a started backend, `au_release_pending` moves the pending buffers to the released queue in order, resets the partial-buffer position and signals the client's release event and the host waiter. Calling Stop on an already stopped output preserves buffers queued for its next Start.
- **Found on:** Pokémon Brilliant Diamond: reported on 1.0.0 in [PR #190](https://github.com/seedds/nxnative/pull/190); the 1.3.0 bring-up passed that transition before this fix.
- **Check:** `t_audio_spsc` covers a partially consumed buffer, queue wraparound, release notification, repeated Stop and buffers queued before Start, then runs its 100,000-buffer producer/consumer stress test.

### The Mac's default I/O buffer

- **Symptom:** clicks and dropouts on the Mac, heard as clipping; the log's `audio #N device callbacks` lines show hundreds of underruns per 2000 callbacks. Closing an output can also log `[RED] audio device buffer: ... (status 560947818)` (`!obj`).
- **Cause:** the Mac's default output asks for 512 device frames per callback (10.7 ms at 48 kHz; 557 frames of the 48 kHz stream when the device runs at 44.1 kHz), more than a game that keeps only a few small blocks queued has ready every time. Unicorn Overlord on a MacBook Pro's speakers: 250–290 underruns in every 2000 callbacks at 48 kHz (0.1–0.2 s of zeros), about 1,000 at 44.1 kHz. Whether a game underruns depends on how much it queues: 13 Sentinels (2.8 blocks queued, like Unicorn Overlord's 3.0, but larger ones) had none at 512 frames.
- **Fix:** the Mac sets its output unit's device buffer to about 5 ms of device frames (`kAudioDevicePropertyBufferFrameSize`, 240 at 48 kHz, 221 at 44.1 kHz), within the device's supported range (`au_buffer` in `hle_audio.c`). It reapplies this when the default output changes and when the same device's output sample rate changes; `CurrentDevice` alone does not notify a rate change. An unchanged buffer is not written again, since a write can itself notify the format listener. Both listeners are removed before uninitializing or disposing the unit, including a failed open: disposal otherwise calls the listener after the device has gone (`!obj`). The log reads back the actual frame count. `DM_DBG_AU_FRAMES=N` asks N frames instead.
- **Found on:** Unicorn Overlord (Asian release 1.05 on the Mac).
- **Check:** [PR #262](https://github.com/seedds/nxnative/pull/262) reports Unicorn Overlord at 240 frames: 0 underruns in 10,000 callbacks; 13 Sentinels stays at 0. A silent Core Audio probe on a Mac mini reproduced the disposal error and the stale buffer after a rate change, then verified the fixes: default output switched between USB at 48 kHz and speakers at 44.1 kHz with callbacks continuing, and the same speakers at 44.1 / 96 / 48 kHz used 221 / 480 / 240 frames. The original default output and device rates were restored. `make -C dm/host/tests GAME=<id> audio-device` checks setup, device and rate notifications, scope filtering, buffer limits, synchronous notifications, the diagnostic override, absent devices, close and failed-open listener cleanup without audio hardware.
- **Open:** the listed HDMI output was not retained as the system default during the probe, so switching to it remains unvalidated. The probe verifies silent host playback; gameplay listening after these listener changes has not been repeated.

### Underruns in a run without a window

- **Symptom:** a windowless Mac run logs intermittent audio underruns. A long frame can occur around the same time.
- **Cause:** macOS normally starts a shell-launched host with an unspecified task role. XNU caps interactive QoS at the legacy level for that role, even when the host requests interactive QoS for guest threads. Under CPU contention the thread that feeds AudioOut can miss the time it has to refill the game's queue. [XNU task policy](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/task_policy.c#L770-L821).
- **Fix:** when `DM_WINDOW` is unset, the Mac front end requests `TASK_DEFAULT_APPLICATION` to lift the QoS ceiling. Set `DM_WINDOWLESS_ROLE=0` to keep the existing role for an A/B run. If macOS refuses the request, the host logs the Mach error and starts the guest.
- **Check:** `t_front_role` always checks the opt-out and the installed role. The `xcode-27` runner on macOS 27.0.1 set the role but kept interactive priority at 20. `taskpolicy -c utility` reproduced that result on an Apple M6 with macOS 27.2.1, where an ordinary launch went from 31 to 46. The test skips the priority increase assertion when both readings agree below the legacy ceiling. [GitHub sanitizer job](https://github.com/seedds/nxnative/actions/runs/44516888710/job/135542380500), NieR role comparison.
- **Only for an app's apptype:** XNU applies the role's ceiling only to a task whose apptype is an app's (`TASK_APPTYPE_APP_DEFAULT` or `APP_NONUI`). Every other apptype gets `USER_INITIATED` whatever its role ([`task_policy.c`](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/task_policy.c), `task_policy_update_internal_locked`). A task takes its parent's apptype (`task_policy_create`), so a host or a test started under a process that launchd spawned as a daemon reads 37 before and after the role. On an Apple M2 Pro with macOS 27.2.1, the host tests run from Rex's terminal server (`launchctl print`: `spawn type = interactive (4)`) failed `t_front_role` at 37 -> 37. The same binary spawned as an app read 31 -> 46, and a background daemon's child read 4 -> 4. The test now runs its checks in a child spawned as an app (`posix_spawnattr_setprocesstype_np`, `POSIX_SPAWN_PROC_TYPE_APP_DEFAULT`), the type Terminal's shells inherit.
- **Open:** a host run from such a terminal keeps the `USER_INITIATED` ceiling, which the role cannot lift. Whether that leaves the AudioOut thread enough time under contention has not been measured.
- **Found on:** NieR:Automata The End of YoRHa Edition.

### An output device that stops calling back

- **Symptom:** sound stays silent after the output device stops calling the AudioUnit back without stopping it; a game whose audio thread waits for its buffers back (Wwise's poll-only output, FMOD) then waits forever.
- **Cause:** the host still thinks the output plays, so nothing releases its buffers. Found on iOS, which does this for Siri, an alarm, a call or another app's audio; the Mac's HAL does it for a moment across a device switch ([Stereo only on a 5.1 system](#stereo-only-on-a-51-system)).
- **Fix:** a watchdog in `hle_audio.c` (`au_watchdog`) switches any started output whose device has not called back for 200 ms to the silent sink, retrying the AudioUnit after 1, 2, 4, 8 s. A start that fails falls back the same way instead of aborting. nxnative's two other layers, the iOS front end's observers of audio session interruptions and of the media services reset, went with iOS.
- **Check:** the log shows `the device has not called back for N ms` and `AudioUnit restarted`. `tests/t_audio.c` (in `dm/host`) scenario 3 stops a playing AudioUnit behind the host's back.

### A blocked audio device start freezes the game

- **Symptom:** a game stops advancing while the Mac's selected output refuses to play. A process sample shows `AudioOutputUnitStart` waiting in the HAL, with the game's audio renderer waiting for a released PCM buffer.
- **Cause:** Start and the watchdog's retries stopped the silent sink and waited for the device while holding the output registry lock. A driver could block there for seconds or return success without delivering any callbacks.
- **Fix:** the silent sink paces the guest at 48 kHz while a host worker starts the device. Until the watchdog sees a callback after startup and joins the silent sink, the device returns silence. A device that fails to deliver its first callback within one second is stopped and retried with backoff. Each output can start independently. A Close during a start that the driver still holds returns at once: it unregisters the output, and the start's worker stops and disposes the unit and frees the output when the driver returns (waiting for it in Close held the game's thread as long as the driver blocked).
- **Check:** `tests/t_audio_device.c` (in `dm/host`) blocks the mock driver's `AudioOutputUnitStart`: Start returns and the queued buffer is released, callbacks play silence until the handoff, and suspend, a unit replacement (`au_reopen`) and Close each run while a start is still blocked.
- **Open:** `AudioOutputUnitStop` still runs under `g_aos_lock` (Stop, suspend, a unit replacement, the watchdog's stall path). A driver that blocked there as it does in Start would hold the lock; not seen so far.
- **Found on:** Splatoon Raiders, with an unavailable HDMI audio output on the Mac ([issue #1030](https://github.com/seedds/nxnative/issues/1030)).

### AudioUnit construction before output publication

- **Symptom:** TSan reports `au_create` writing `a->au` while `au_watchdog` reads it under `g_aos_lock` when a second real output opens (issue #410).
- **Cause:** `au_open_at` put the output into the lifecycle/watchdog registry before joining its AudioUnit construction helper. The registry lock did not order the helper's writes.
- **Fix:** construct and join while the output is private, then publish it under `g_aos_lock`. Suspend, resume, route-change and watchdog readers only discover the completed output. Construction remains outside that lock because joining the helper can yield the guest core.
- **Check:** `t_audio_device` asserts from inside AudioUnit creation that the pending output is absent from the registry; it failed before the fix. `t_audio` exercises real output lifetimes and watchdog recovery under TSan with the `au_create` suppression removed.
- **Found on:** host unit tests; no particular game's failure was attributed to this race.

### Audio across a background round trip on the Mac

`DM_AUDIO=1 DM_AUDIO_MUTE=1` takes the real Core Audio path with the output silenced, and `DM_OE_SCRIPT="bg=1@60,bg=0@66"` runs a whole background round trip (focus, audio take-over, guest clock freeze, graphics suspend; `script_bg` in `hle_oe.c`). Send `dmhost` `SIGSTOP` / `SIGCONT` between the two to imitate a suspended process. The silent sink that stands in while in the background does not make up the time the process was suspended (it used to pull seconds' worth at once and log hundreds of underruns).

### Several outputs at once (`audio #N`)

- **Symptom:** combined numbers mislead: one fully fed output plus one fed by nobody look like "every other callback underruns, 2.0 blocks queued".
- **Cause:** a game may open several outputs. Engage opens one AudioOut from Wwise (`main+0x177d924`, no release event, the game polls); on the phone it opened a second one later, and the Mac runs never did. Unity's FMOD also has an output (`main+0x4aec0c` / `0x4aec4c`).
- **Fix:** the host logs each output as `audio #N`: the open with its call site, Start / Stop / Close (the first 32 per output), and the callback statistics normally every 2000 of that output's callbacks, through the [watchdog](#periodic-diagnostics-outside-the-audiounit-callback).
- **Found on:** Fire Emblem Engage.

### Checking audio without listening

`DM_AUDIO_DUMP=file` writes every PCM block appended to an output as it is (16-bit, 48 kHz; the channel count is the one in that output's `OpenAudioOut` log line, eight for the renderer's output: its six channels and their stereo fold). RMS per second, clipping share, spectral flatness (music ≈ 0.1–0.5, white noise ≈ 0.85) and the strongest FFT peaks (they should land on musical notes) tell real music from noise or silence.

### Audio device output volume and names

- **Symptom:** an abort in `sm::Initialize` under `audio::GetAudioDeviceOutputVolume` (the game thread, `main+0xee8edc`).
- **Fix:** the volume `SetAudioDeviceOutputVolume` sets is kept and read back (`hle_audren.c`).
- **Found on:** DRAGON QUEST VII Reimagined (SDK 19.3.1, Unreal Engine 4.27: `GetAudioDeviceOutputVolume`).
- **Note:** the device calls are taken over under the names the game's SDK has (`ListAudioOutputDeviceName`, `GetActiveAudioOutputDeviceName`, `GetActiveAudioDeviceChannelCountForOutput`). The older SDKs' names of the same calls, and `GetAudioOutName`, are not hooked in this fork.

### The output device notification under its SDK 21 and 22 name

- **Symptom:** an `nn::diag` abort with Result 0xe15 on an audio thread at start, in `audio::CreateUserClientSessionByHipc` under `audio::AcquireAudioOutputDeviceNotification`, after the line `sf: the game connects to service aud:u itself`.
- **Cause:** SDK 21 and 22 name the output device notification `AcquireAudioOutputDeviceNotification` (older SDKs: `AcquireAudioDeviceNotificationForOutput`, and SDK 7's `AcquireAudioDeviceSwitchNotification`). SDK 22.2.0's version (`sdk+0x1f1e00`) opens a user session with `aud:u`, which the host does not serve, asks it for a read end and attaches it AutoClear. It keeps no per-process flag (the service keys each acquire by its event) and returns a Result.
- **Fix:** `hle_audren.c` takes the name over, first for SDK 21 (Splatoon Raiders); the same takeover serves SDK 22, and it is the only name of this call the fork takes over. The event is bound, signaled on a host output-route change, and the Result is 0. Signals use its binding lifetime, so destroying and reusing the same event storage cannot redirect an old audio subscription. The takeover keeps the older names' once-per-process flag, so a second acquire returns 0x499 where SDK 22.2.0's code would bind again. No game has been seen to acquire twice.
- **The release:** `ReleaseAudioOutputDeviceNotification` (`sdk+0x1f2ac0` in Nintendo Switch Sports Resort's sdk) asks `aud:u` through the device session coordinator, then destroys the event. The game destroys the event itself only if it is still initialized afterwards (`main+0x3af11a0`). `hle_audren.c` takes it over: it destroys the event it bound (which drops the binding) and clears the flag, so a later acquire binds again. With nothing acquired it does nothing. `t_audren_work` covers acquire, the refused second acquire, release and acquire again.
- **Found on:** Fire Emblem Fortune's Weave (Switch 2, SDK 22.2.0, `main+0xe7b9c`); Nintendo Switch Sports Resort (`04008E001F098000`: the release, `main+0x3af119c`).

### No audio input devices

- **Symptom:** SDL3's audio driver lists the input devices (AudioIn) when its audio starts. `[RED] sf: the game connects to service audren:u itself`, then `nn::diag abort: Result 0xe15` under `audio::AcquireAudioDeviceNotificationForInput` (`sdk+0x5633c`).
- **Cause:** the host has no microphone. The input device-change notification goes through the audio device service, like the output one the host answers.
- **Fix:** an AudioIn does not open: `OpenAudioIn` without a buffer event, the one AudioIn call this fork takes over, logs the refusal once and fails (`ai_open` in `hle_audio.c`). What nxnative also answered here for other games (`ListAudioIns` with no device, the open with an event and the default-device open, the calls on an open AudioIn, `AcquireAudioDeviceNotificationForInput`) is not taken over and would reach the SDK's services. A microphone is served only through [input streams](#virtual-audio-input-streams-sdk-2122).
- **Found on:** Absolum (SDK 18.3.2, FNA on .NET NativeAOT with SDL3; one boot each); Portal 2 (SDK 13.3.0: its audio thread at start, `main+0x19f37c`).

### Virtual audio input streams (SDK 21/22)

- **Symptom:** `[RED] sf: the game connects to service auddmg:u itself, which the host does not serve`, then `game called nn::diag abort: Result 0xa99` at `sdk+0x1f61ac` under `audio::ListVirtualInputDevices`. On main before real input support, the safe empty list also kept the game from hearing a microphone.
- **Cause:** the game lists virtual input devices when its audio starts (`main+0xf48124`) and opens the first with `audio::OpenInputStream` if there is one. The host intercepts those SDK symbols because it has no `auddmg:u` service.
- **Fix:** the Mac app offers an off-by-default microphone setting when a packed module imports the supported polling open. If enabled, the app requests macOS consent before it starts the host, including when the settings window is hidden. When enabled and authorized, one AUHAL unit feeds bounded queues for the polling streams. The callback uses preallocated buffers, downmixes and resamples the device to 48 kHz mono PCM16, and applies gain and a soft limiter. A full queue drops incoming frames. Device changes rebuild capture off the callback thread, including returning from a missing device while streams stay open. If disabled or denied, the list is empty and opens fail. Legacy AudioIn, event-based streams, and guest stream statistics remain open. [#2796](https://github.com/seedds/nxnative/issues/2796)
- **Found on:** [The Legend of Zelda: Ocarina of Time](../../dm/games/0400486024DEC000/README.md) (SDK 22.2.8). [PR #1822](https://github.com/seedds/nxnative/pull/1822) reports Mac gameplay with built-in and USB inputs, including resampling from 44.1 kHz. Star Fox imports the same calls.

### `OpenAudioOut` without a release event (SDK 22)

- **Symptom:** `game called nn::diag abort: Result 0xe15, call site sdk+0x1f8704`, i.e. inside `nn::audio::CreateAudioOutManagerByHipc+0xb4`, before any audio device opened.
- **Cause:** SDK 22 calls **`nn::audio::OpenAudioOut(AudioOut*, const char*, const AudioOutParameter&)`** (`sdk+0x1f7914`, `nn::audio::OpenAudioOut(AudioOut*, const char*, const AudioOutParameter&)`'s failure path) — the overload without the `SystemEvent` the host implemented (`_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPKcRKNS0_17AudioOutParameterEPNS_2os11SystemEventE`). With no hook the SDK's own IPC path ran and the audout service is not there.
- **Fix:** `hle_audio.c` hooks this overload (`h_au_Open4`), which opens through `au_open_at` with no release event, so the output polls its released buffers. The log then reads `OpenAudioOut … , no release event (the game polls)`. In this fork it is the only AudioOut call the host takes over: the overloads with an event and the default-device opens are gone, and Start, Stop, Append and the rest of that API would run the SDK's own.
- **Found on:** Well Dweller (SDK 22.2.0); the game's first `AudioOut` is the "DeviceOut" form, the second its 48 kHz music output.

### SDK 21's OutputStream on a virtual output device

- **Symptom:** `[RED] sf: the game connects to service auddmg:u itself, which the host does not serve`, then `game called nn::diag abort: Result 0xa99` under `nn::audio::GetDefaultVirtualOutputDevice` (`sdk+0x221520`, `audio::detail::GetVirtualOutputDeviceProperties`).
- **Cause:** SDK 21 has a second output interface beside AudioOut: `GetDefaultVirtualOutputDevice` names a virtual device, `OpenOutputStream` opens a stream on it with a `StreamFormat` {sample format, channels, sample rate} (5, float), a capacity in frames and, in one overload, a system event; the game writes a period at a time with `WriteOutputStream` once `GetOutputStreamFreeFrameCount` covers it, waiting on the event with a short timeout between writes. States as games read them: 0 not open (the SDK's own answer for a null stream), 1 stopped, 2 started: a game's close path stops a stream in state 2 and purges and closes one in state 1, so a wrong answer leaves it open for good. `GetOutputStreamStatistics` returns a 24-byte struct through x8. Every call reaches the service.
- **Fix:** `hle_audio.c` opens one AudioOut per stream, its release event the stream's: each write becomes a 16-bit interleaved block (float clamped and scaled), the free count is the capacity less the frames still queued, `Purge` drops the queued blocks, and the statistics report nothing. The open without an event, `StartOutputStreams` and `SetOutputStreamGain` are not taken over in this fork. A channel count other than 1, 2 or 6 [is refused](#an-audioout-or-outputstream-opened-with-an-unsupported-channel-count). Input streams have their own [capture path](#virtual-audio-input-streams-sdk-2122). The default virtual device is `AudioVirtualOutput`, a name of the host's.
- **The frames are interleaved:** `WriteOutputStream` takes L, R, L, R, although the game's code that zeroes an unfilled tail (`main+0xad1684`) steps by a period per channel. Read as planar, each 10 ms block's first half played as left and its second half as right, each at half speed: all sound an octave low (voices very deep) with a jump at every block (crackling). In `DM_AUDIO_DUMP` that shows as a spectrum mirrored about 12 kHz (33-39 % of the energy above 20 kHz where 0.00 % belongs) and channels whose samples alternate between two signals (lag-1 autocorrelation 0.18, lag-2 0.99). `t_output_stream` checks the order.
- **A larger statistics result ends the game:** Star Fox keeps its 24 bytes at `sp` with its stack canary right behind them (`main+0xbce9b90`), so a 32-byte result ended in `__stack_chk_fail` on the audio thread (`CRASH sig=4`, `lr=main+0xbce9f98`).
- **Focus:** Star Fox closes its stream when its window loses focus and opens a new one when it regains it; 14 rounds went by with the sound intact.
- **Found on:** Xenoblade Chronicles 2, Nintendo Switch 2 Edition (`0400E95004038001`) (SDK 21.4.4, one or two 2-channel streams at 48 kHz, 10 ms periods, 40 ms capacity; the second when the active device is `AudioExternalOutput`); Star Fox (Wwise 2024.1, SDK 21.4.4).

### The virtual output device's properties, and the OutputStream open without flags

- **Symptom:** `[RED] sf: the game connects to service auddmg:u itself, which the host does not serve`, then `game called nn::diag abort: Result 0xa99, call site sdk+0x224d1c` (`audio::detail::GetVirtualOutputDeviceProperties`), right after `GetDefaultVirtualOutputDevice`.
- **Cause:** before each OutputStream open the game asks for the default virtual output device's properties with `nn::audio::GetVirtualOutputDeviceProperties` (`main+0x5eda90`, `main+0x7a0414`); the SDK's version asks the device manager service for the device's 0x228-byte profile. It then opens the stream with the `OpenOutputStream` overload that takes no flags (`main+0x5edb34`), which the host did not take over either.
- **The properties:** `VirtualDeviceProperties` is 0x110 bytes, filled from the profile at `sdk+0x224cd4`: the name asked about (+0x00, 0x100 bytes); the profile's +0x218 at +0x100, the device's shortest stream as a duration in ns (`OutputStreamImpl::Open` refuses a capacity under ceil(duration x rate / 1e9) frames with 0x899, `sdk+0x225a94`; the input stream open the same); and the profile's +0x108 at +0x108, a 32-bit word nothing in the sdk reads. Both of the game's callers, and Donkey Kong Bananza's two (`main+0x57cadc`, `main+0x187cee4`), only test the Result.
- **Fix:** `hle_audio.c` answers for its one device, `AudioVirtualOutput`: the name, a shortest stream of 10 ms (one period as Xenoblade Chronicles 2 and Star Fox write it; the host's own open sets no minimum) and 0 in the unidentified word. Any other name gets a red line and a failure. The overload without flags opens like the others. `t_output_stream` covers both.
- **Open:** what the +0x108 word means; no caller seen reads it.
- **Found on:** Nintendo Switch Sports Resort (`04008E001F098000`; SDK 20+).

### The AudioOutBuffer handed to the SDK is its own 40-byte object

- **Symptom:** a game that resamples its own audio plays, in one block of every four, the sound of four blocks earlier: Metro 2033 Redux (44.1 kHz to 48 kHz in the game's own resampler) re-appended 518 of its 2,095 loud 8192-byte blocks (24.7 %) still holding the contents of the block 4 appends back, so that audio left 170.6 ms behind; after the fix 0 of 1,989, with no block appended twice or lost and no underruns.
- **Cause:** the host declared `nn::audio::AudioOutBuffer` as 24 bytes, data at +0x00, capacity at +0x08, data size at +0x10. The SDK's object is 40 bytes, `{next, buffer, buffer_size, data_size, data_offset}`, the audout service's own buffer format: data at +0x08, capacity at +0x10, data size at +0x18 (`SetAudioOutBufferInfo`, `sdk+0x80f80`; the same in Hades's SDK, `sdk+0x142ab0`). The SDK's `AudioBufferList` links buffers through `next` at +0x00, where the old layout wrote the data pointer. The host does not take `GetAudioOutBufferBufferSize` over, so the SDK's own (`sdk+0x80fb0`) read the host's data-size field as the capacity and answered with the previous block's: 2047 frames of the 2048 the resampler had room for. On every fourth block the resampler then wrote nothing, and the game appended that block at full length holding the earlier contents.
- **Fix:** the host's buffer description is the SDK's object: one definition, `aout_buf` in `dm/host/hle.h`, five fields at the SDK's offsets with a `_Static_assert` on them, which `hle_audio.c`'s AudioOut and the blocks `hle_audren.c` appends to it both use. The descriptor functions (`SetAudioOutBufferInfo` in both overloads, `GetAudioOutBufferDataPointer`, `GetAudioOutBufferBufferSize`, `GetAudioOutBufferDataSize`) run in the sdk, as AudioIn's do, so no accessor can read a layout of the host's; they are the same plain stores and loads in the SDKs of Metro 2033 Redux (SDK 8.2.0), Hades, Fire Emblem: Three Houses (which also has the overload without a buffer size, storing only +0x08 and +0x18) and PRAGMATA (Switch 2). `GetAudioOutBufferBufferSize` answers 8192 on every block.
- **Found on:** Metro 2033 Redux (SDK 8.2.0, 4A Engine).
- **Check:** `t_audio`'s buffer-layout case hands the device callback an object written at the SDK's own offsets, its `next` linked and its data size 4 bytes short of its capacity: the callback must play exactly the data size from the data pointer, release the buffer and leave `next` alone. Under the 24-byte layout it would read `next` as the data.

## Output channels

The output route decides how many channels the device takes: six when the device's current stream format has six or more (`dm_audio_output_channels`, `hle_audio.c`), otherwise stereo. Games read it from `GetActiveAudioDeviceChannelCountForOutput`, and each output AudioUnit follows it. The in-process renderer queues both its six channels and its original stereo fold (`hle_audren.c`).

### Stereo only on a 5.1 system

- **Symptom:** on a Mac connected over HDMI to a 5.1 TV, receiver or soundbar, a game that writes six channels plays in stereo: its center, LFE and back channels come out of the front pair as a downmix. The game is told the device has two channels.
- **Cause:** `GetActiveAudioDeviceChannelCountForOutput` reported two channels on every route, and every six-channel output was downmixed to stereo before it reached Core Audio.
- **Fix:** the host reports the route's count, and a six-channel output on a six-channel route goes to the unit as it is, with the unit's input layout set to `MPEG_5_1_A` (front-left, front-right, center, LFE, back-left, back-right). Core Audio then maps each channel to its speaker in the device's own layout. On HDMI that order puts the LFE before the center (`L R LFE C Ls Rs …`), so six channels sent without a layout would play the center on the LFE. On a stereo route the host downmixes a six-channel output with the renderer's default coefficients; Core Audio's own 6 → 2 would keep only the front pair. A mono or stereo output goes as the guest writes it, an output opened with a channel count of 0 is stereo, and any other count [is refused](#an-audioout-or-outputstream-opened-with-an-unsupported-channel-count).
  - `dm_audio_route` reads a cache initialized on the calling host stack (all HLE takeovers already switch to it). The Mac listens to the system default output and that device's stream format, including when no AudioUnit is open; opens and resume also refresh it. A changed channel count or device identity publishes the cache before scheduling the guest's output-device notification off the audio registry lock.
  - The Mac's route is the device's stream format: a device set to six or more channels (in Audio MIDI Setup, or by an HDMI sink's EDID) is a six-channel route. The host does not change a device's format.
  - A route change while an output plays queues a check off the notifying thread. Each six-channel guest output, or host DeviceSink output, whose unit no longer fits the route gets a replacement unit (`au_reopen`). The silent sink paces queued audio meanwhile; a start still held by the driver retains its old unit until it returns. A failed replacement is tried again on the next route change or resume, and resume checks changes made during suspension.
  - The in-process renderer never freezes its format to the opening route. Each queued sample contains six channels followed by a stereo fold computed from the original int32 mix before resampling and clamping. The device callback selects six or two, even halfway through a queued block. This preserves current main's stereo PCM at both 48 kHz and 32 kHz: folding already clamped surround can destroy cancellation (front-left -30000 plus center 60000 gave -6834 instead of 12420). Guest AudioOuts retain the count the game opened; their device-change event lets the game query the new count and reopen when needed.
  - `DM_AUDIO_DEVICE` pins the Mac's output to a device by name, for tests. BlackHole 2ch reads as stereo; a six-channel virtual device needs BlackHole 16ch (its first six channels carry the 5.1 signal).
- **Still seen:** on a device switch the HAL stops calling the unit before it notifies (about 230 ms when switching between BlackHole devices). The watchdog logs `[RED] audio #N: the device has not called back` and the silent sink covers the gap, so 0.1–0.2 s of the game's audio is not heard. Spatial Audio is not done: the system's binaural rendering of six-channel PCM over AirPods Pro 2 (`setSupportsMultichannelContent`, `isSpatialAudioEnabled` on the route) is not tried, and Apple does not document which formats it accepts. The route is the device's first output stream; a device whose first stream is stereo stays stereo.
- **Checked:** the current-main rework adds route changes without an open unit, selecting both representations mid-block, and destroyed/rebound notification lifetimes. Stereo PCM hashes match the pre-surround output at 48 kHz and 32 kHz over three frames including clipping and resampler history. `t_audio_device` (mock units: six channels reach the device unchanged with the layout set, the downmix values and its clamp, the route's count, a route change both ways, a failed new unit made again at the next change, stereo and mono outputs left as they are) and `t_audren_layout` (the six-channel resampler, the sink's six inputs in order, a mono sink on the front pair). `dm/tools/audio_channel_check.py` plays test tones (440, 660, 880, 60, 1320, 1760 Hz for front-left, front-right, center, LFE, back-left, back-right) through the host on a named device and captures that device's input in-process (`DM_AUDIO_CAPTURE`): the check asks for each channel's own tone 12 dB above the others, and for the downmix within 1.5 dB of the host's coefficients (`--stereo`). PR #2234 author's measurements: on BlackHole 16ch each channel carried its tone at 0.200, with the others more than 100 dB down; on BlackHole 2ch the downmix came within 0.05 dB. With BlackHole 16ch set to HDMI's layout, the captured center tone came out on channel 4 and the LFE on channel 3. `t_audio_live` with `default=` and `switch=` moved the output between BlackHole 16ch and 2ch while it played: after each switch the new device carried the 5.1 signal or the downmix (center and back at 0.141, LFE at 0.050 for 0.2 tones).
- **Current-main rework validation:** MacBook Pro M4 Pro, macOS 27.2.1, Xcode 27.2: audio/output-stream/SPSC/statistics/device/renderer fixtures pass, including ASan/UBSan on the changed frame and route boundaries. A real stereo AudioUnit consumed an eight-value 100 ms silent DeviceSink block in 101.7 ms. Only built-in stereo speakers were available: the rework has no new physical six-channel confirmation. Synthetic capture CPU time per 5 ms frame (median of three 10,000-frame batches, `-O1`, compared with main `2925d7af`): 0.37 to 1.19 us at 48 kHz and 1.61 to 10.61 us at 32 kHz; these measure capture, not a game's total CPU. The four renderer slots and capture scratch reserve 566,688 more bytes (0.54 MiB); active resident memory depends on the renderers opened. Keeping both representations adds neither a polling thread nor per-frame allocation.
- **Found on:** PR #2234's host probe on a Mac over HDMI to an LG TV, passed over eARC to a Sonos system (the TV reads as 8-channel 48 kHz PCM in HDMI's order): `t_audio_live` with `DM_AUDIO_LIVE_PCM` (a spoken name on each channel and a 45 Hz tone on the LFE) played each from its own speaker. Not yet checked in a game.

## Codecs

### Hardware Opus decoders built on the SDK's software decoders

- **Symptom:** an abort in `sm::Initialize` under the hardware Opus decoder's C interface (`codec::GetHardwareOpusDecoderWorkBufferSize`).
- **Cause:** the hardware decoders are a service (`hwopus`), which the host does not serve.
- **Fix** (`hle_codec.c`): the single-stream C interface (`GetHardwareOpusDecoderWorkBufferSize` with its option argument, `InitializeHardwareOpusDecoder`, `DecodeOpusInterleavedWithHardware`, `ResetHardwareOpusDecoder`, `FinalizeHardwareOpusDecoder`, the sample rate and the channel count) is forwarded to the `sdk`'s software `OpusDecoder` C interface, with a host-held decoder state per game object (`objmap`); a decoder the host did not initialize stops the host. That is all this fork keeps. The multi-stream decoder, the two C++ classes, the two-argument work size and SDK 21's decoder manager, which nxnative served for other games, are not taken over; the one manager call the game's modules name, `GetHardwareOpusDecoderManagerFreeIoBufferCount`, is refused with a `[RED]` line and no free buffer. `t_codec_opus` checks the forwarding and the refusal.
- **Found on:** The Legend of Zelda: Tears of the Kingdom (1.4.3, the single-stream C interface).

## Timing and catch-up

### At least one rendered frame between two updates

- **Symptom:** the light limiter crashed on a null state pointer (`ApplyLightLimiterEffect`, `sdk+0x228494`).
- **Cause:** an effect's "needs init" parameter state is set only in the update where the game flags it new; the next update copies the game's value over it. Mario updates twice in a row at start-up; with a real audio device the first frame came after both.
- **Fix:** the `AudioRenderSystem::Update` hook renders a catch-up frame first when nothing was rendered since the last update.
- **Found on:** Super Mario Bros. Wonder.
- **Check:** `DM_AUDREN_TRACE=1` logs every update with the number of frames rendered since the previous one.

### A sound driver that never waits on the renderer event

- **Symptom:** the sound runs about 50 times faster than the output; the loop runs 11,000 updates a second on one core.
- **Cause:** Square Enix's `SQEX::Sd::Driver::Core::CoreAudioOut` (its loop at `main+0x442c5c0`) appends a wave buffer, mixes the next block on the same thread, calls `RequestUpdate` and takes the next released buffer. It waits on the renderer event only when no buffer is released. Every update there finds no frame rendered since the last one and renders a catch-up frame, which releases the buffer just appended, so the loop never waits. When a full catch-up queue discarded the frame, the loop ran unchecked.
- **Fix:** an update with a full queue waits (at most 50 ms) for the render thread to play one queued frame, so catch-up frames keep real time (`hle_audren.c`). The render thread then only plays queued frames, and the driver's updates render every frame. A queued frame does not signal the renderer event (the SDK signals it in each render): signalled, an ordinary game woke, updated with nothing rendered since and rendered the next catch-up frame, so every frame became one (Big Brain Academy: Brain vs. Brain, about 38,000 catch-up renders instead of 37).
- **Found on:** DRAGON QUEST XI S: Echoes of an Elusive Age – Definitive Edition.
- **Check:** `audren: rendered 12000 frames, N of them not silent` shows whether a game's sound reaches the output.

### Burst renders when a device callback releases multiple blocks

- **Symptom:** villager dialogue crackles while music and sound effects play cleanly. On a 44.1 kHz / 256-frame device, `catch-up renders so far` grows by about 277 per 2,000 renders. `DM_AUDREN_TRACE=1` shows two renders between updates or two updates 0.02 ms apart followed by an 11.6 ms gap.
- **Cause:** a CoreAudio callback can return several 5 ms blocks together. `render_main` refilled them back to back, so the game's audio thread could coalesce two renderer event signals or wake twice before it could update between renders.
- **Fix:** after prefill, `render_main` spaces fresh renders at least 4.5 ms apart, using the output's returned blocks to set the number due. The timed wait releases the catch-up queue lock. A catch-up published during the wait takes priority over a fresh render, and its slot stays occupied until the PCM is copied into the output block.
- **Found on:** Animal Crossing: New Horizons. The reporter confirmed clean dialogue on an M4 with 44.1 kHz Bluetooth audio using the original pacing patch. [Issue #2981](https://github.com/seedds/nxnative/issues/2981) · [PR #2983](https://github.com/seedds/nxnative/pull/2983)
- **Check:** `t_audren_layout --cadence` publishes a catch-up from a concurrent `RequestUpdate` during the paced wait and checks 2,000 ordered PCM blocks at each of 44.1 kHz / 256 frames and 48 kHz / 512 frames. At 48 kHz / 512 frames, The Binding of Isaac: Rebirth on an M6 had one catch-up in the first 6,000 renders, down from 1,227 on main, with zero underruns in the first 2,000 device callbacks on both builds.

### Late audio producer wakeups despite fast rendering

- **Symptom:** constant static behind otherwise clean sound; the output settles near one queued block, with 12–38 underruns per 2,000 callbacks on speakers and up to 275 on AirPods, although rendering averages about 0.07 ms of each 5 ms block.
- **Cause:** `render_main` had ordinary time-sharing policy. Its guest-thread startup already requests interactive QoS, and AudioOut already signals a host semaphore when a block is released, but neither declares the audio producer's real-time scheduling requirement. Aggregate performance-core usage in the report does not establish the render thread's core placement.
- **Fix:** after output setup, the in-process renderer's worker requests `THREAD_TIME_CONSTRAINT_POLICY` with a 5 ms period/deadline and a requested 1 ms computation budget, and restores standard policy before teardown. A rejected request is logged and playback continues with its existing policy. `DM_AUDREN_RT=0` retains ordinary scheduling for A/B measurements. This is host-bound for every in-process renderer; guest-created audio threads are unchanged. The 4.5 ms fresh-render spacing remains necessary for [burst refills](#burst-renders-when-a-device-callback-releases-multiple-blocks).
- **Found on:** [The Legend of Zelda: Ocarina of Time](../../dm/games/0400486024DEC000/README.md), [issue #3059](https://github.com/seedds/nxnative/issues/3059). Two reporters observed underruns. On an M4, macOS 27.0.1, nichq50 reported four queued blocks and zero underruns across 16,000 callbacks on speakers and AirPods after applying only the policy patch; the attached text file is the patch, not a post-fix log.
- **Check:** `t_audren_layout` sets, reads and resets a real pthread's Mach policy, checks setup/teardown scope, and verifies intact PCM with policy rejection and opt-out. Its cadence fixtures retain 2,000 ordered blocks without simulated underruns at 44.1 kHz / 256 frames and 48 kHz / 512 frames; these do not simulate OS wakeup delay. On an M6 with macOS 27.2.1, XNU raised the requested 1 ms computation to 2.5 ms (half the constraint), consistent with [XNU's policy implementation](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/thread_policy.c). This is a budget, not CPU time consumed.
- **Live validation:** Isaac's audio scheduling checks cover real 48 kHz USB output and 44.1 kHz built-in speakers on an M6, including the limits of its CPU/energy samples. Breath of the Wild's scheduling validation covers SDK 20 and isolates a screenshot-associated frame-time flag with a matched policy-off/on comparison.
- **Open:** the power impact, including on the affected M4. The 44.1 kHz crackle reported in [issue #2796](https://github.com/seedds/nxnative/issues/2796#issuecomment-6998635365) lacks underrun measurements, so its overlap remains unconfirmed.
