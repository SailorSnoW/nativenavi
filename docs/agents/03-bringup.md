# 03 — Running the game in the host

## The loop

`B` is the game's build folder, `<NUP folder>/0400486024DEC000_build` (or the `--work-dir` given), and `VER` the packed version (`v131072`).

```bash
python3 scripts/build_app.py "<NUP>" --platform mac    # once: unpack, rewrite, shaders, host; its last line is the command that runs the game
# after a change to the rewriter or the profile's patches.py: rewrite the modules
for m in $(python3 dm/games/game.py 0400486024DEC000 modules); do python3 dm/tools/patch_nso.py "$B/extract/$VER/ExeFS/$m" "$B/patched_$VER" $m; done
# after a change to the host
make -C dm/host dmhost
```

The development build is described in [A Mac-only build for development](../development.md#a-mac-only-build-for-development). The command it prints runs `$B/tools/dmhost` on `$B/patched_$VER` from `$B/run`, with `DM_ROMFS`, `DM_SAVE`, `DM_TEMP`, `DM_MSL_DIR` and the other paths set; `make -C dm/host dmhost` writes `dm/host/dmhost`, so run that one in its place, or run the packing script again, which reuses what did not change. `DM_WINDOW=1` opens a window and `DM_AUDIO=1` plays sound: leave them out for a headless run, driven with `DM_KEYS` or `DM_CTL` ([05](05-debugging.md#scripted-input-and-thread-scheduling); the game's own route and helper scripts are in [its note](../../dm/games/0400486024DEC000/README.md#driving-it-on-the-mac)).

Give a scripted run a time limit, for example with `perl -e 'alarm <seconds>; exec @ARGV' <the command>`, and send its output to a log file. A run that hits the limit ends with `CRASH sig=14`: that is the alarm, not a crash. Logs may contain non-UTF-8 bytes: use `grep -a` / `LC_ALL=C` when filtering.

## Reading the log

| Marker | Meaning |
|---|---|
| `>>> nnosInitialize`, `>>> nnMain` | Boot stages reached |
| `[RED] …` | Something unimplemented or wrong, printed once per kind. Collect them with `grep -aF "[RED]" log \| sort \| uniq -c` after every run |
| `UNIMPL <name>` | A forced NVN / SDK stub was called |
| `[nvn] frame N: … \| name=count …` | With `DM_NVN_COUNT=1`: per-frame NVN call counts (calls made on the frame thread; command buffers recorded elsewhere replay later) |
| `[longframe]` | A long frame, with submit / GPU / pipeline / upload breakdown |
| `memory up to frame N: resident … GPU allocated …` | Memory report (RSS, Metal allocated size). Printed when memory moves; steady growth here is a leak. A watchdog exits at 24 GB (`DM_MEM_CAP_GB` sets another limit). Also: memory pools and how many are resident (pools join the residency set on first GPU use), finalized pool buffers still alive, and the storage-sharing texture table |
| `CRASH sig=… pc=… lr=…` + registers + `bt:` + `stack scan` + thread dump | Crash report: see [05](05-debugging.md) |
| `thread 0x10xx 'name' … waits:<state> addr=…` | Thread dump: what each guest thread waits on (`address` = address arbiter, `mutex` = mutex with holder, `condvar`, `sync object`, `running/not waiting` = running); a thread waiting in an SVC also gets a `guest stack:` line, the words on its guest stack that point into a module (its callers when the code keeps no frame pointers) and x19–x28 |

Useful switches: `DM_FS=1` (file opens, and not-found probes), `DM_SVC=1` (memory SVCs and other syscalls), `DM_THR=1` (thread SVCs), `DM_WATCH=<s>` (dump all threads after s seconds), `DM_WATCH_STALL=<s>` (a red line, a dump and an exit when no frame is presented for s seconds).

## Failure catalogue

The failures met so far are filed by subsystem, one entry per mechanism (symptom, cause, fix, where it was found): [packing](../reference/packing.md), [rewriter](../reference/rewriter.md), [memory and address space](../reference/memory.md), [threads](../reference/threads.md), [services and SDK calls](../reference/services.md), [movies](../reference/movies.md), [audio](../reference/audio.md), [frame rate](../reference/frame-rate.md), [NVN objects](../reference/nvn-objects.md) and [game state](../reference/game-state.md); graphics in [rendering](../reference/rendering.md) and [shaders](../reference/shaders.md). Many entries were found by nxnative on other games and are kept because the code they explain is still here. A new failure is often one of them in a new form, so search the [symptom index](symptoms.md) for the assertion, `[RED]` text, abort site or visible result first, then read the entry.

A new finding goes to one place: a mechanism of the host or the tools as an entry in its reference (with a line in the symptom index, written by `python3 dm/tools/docs_symptoms.py`), a fact about the game in its note ([dm/games/0400486024DEC000/README.md](../../dm/games/0400486024DEC000/README.md)).
