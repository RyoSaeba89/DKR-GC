# DKR-GC — Diddy Kong Racing, native on the Nintendo GameCube

A native GameCube port of *Diddy Kong Racing* (N64), built on top of
[DavidSM64/Diddy-Kong-Racing](https://github.com/DavidSM64/Diddy-Kong-Racing),
the 100 % decompilation of the game.

It is not an emulator and not a recompilation. The decompiled C is compiled
for PowerPC with devkitPPC, and everything the N64 provided underneath it is
reimplemented against libogc2: libultra's scheduler, threads, message queues,
PI DMA, VI, AI, controller, Controller Pak and EEPROM; the F3DDKR display-list
interpreter on GX; and the N64 audio microcode's sixteen opcodes on the CPU.

**Status: alpha 0.0.1.** It boots, draws and runs at the nominal frame rate on
a real PAL console. Nothing is stubbed — the port handles every graphics and
audio opcode the game emits. It is also not finished: see
[Known issues](#known-issues).

---

## This repository ships no game data

**None of Nintendo's or Rare's data is here, and none is in the release
binary.** The decompiled source is Rareware's game logic reimplemented as C by
the upstream project and released under CC0 (see [LICENSE.md](LICENSE.md)); the
graphics, audio, models, tracks, text and level data are not, and are not
distributed in any form. `dkr.assets` is a ROM, byte for byte, and never
appears here or in a release.

You must supply your own copy of the game.

---

## Running it

You need:

- a GameCube with a way to run homebrew — [Swiss](https://github.com/emukidid/swiss-gc) on an SD Gecko, an SD2SP2, or a modchip;
- your own **Diddy Kong Racing (U) (V1.0)** ROM, 12 MB, `md5 4f0e07f0eeac7e5d7ce3a75461888d03` (`sha1 0cb115d8716dbbc2922fda38e533b9fe63bb9670`);
- `dkr.dol` from the [latest release](../../releases).

Put this on the card:

```
sd:/dkr/dkr.dol        <- from the release
sd:/dkr/dkr.assets     <- your own ROM, renamed
```

and launch `dkr.dol` from Swiss. The port also looks for the image on
`carda:/dkr/` and `cardb:/dkr/`, so an SD Gecko in either slot works.

Tested on a PAL console. NTSC is handled by the same code path but has not been
run on hardware.

### The log

The port writes everything it knows to `sd:/dkr/dkr.log` — a heartbeat every
60 retraces with frame timing, display-list coverage, audio counters, the ARAM
read path, and the game thread's blocking point. If it crashes, the register
dump and a stack trace go to `sd:/dkr/dkr.crash` and are shown on screen at the
next boot. That log is the whole debugging story of this port; attach it to any
issue.

---

## Building from source

You need a ROM here too — the build extracts the asset image from it.

```sh
# devkitPro with devkitPPC and libogc2, plus zlib from portlibs
make -f Makefile.gc
make -f Makefile.gc dist        # -> dist/dkr/{dkr.dol,dkr.assets}
```

`Makefile.gc` is the port's build; the upstream N64 `Makefile` is untouched and
still works. See [README.decomp.md](README.decomp.md) for the decompilation's
own setup, which is what produces `include/asset_enums.h` and the asset image.

Knobs worth knowing (all `make -f Makefile.gc VAR=value`):

| | |
|---|---|
| `GC_EMBED_ASSETS` | `1` links the asset image into the `.dol` (12 MB bigger). **Release builds use `0`** — a `1` build contains the ROM and must never be redistributed. |
| `GC_DEBUG` | `1` turns on the port's tracing and the heartbeat. On in the release build; it is what makes the log useful. |
| `GC_MAIN_POOL_MB` | the game's heap, in MB. Default 4. |
| `GC_AUDIO_FX` | let the game enable its reverb. Default 1. |
| `GC_MEMCARD` | `0` disables the whole storage subsystem — no probing, no mounting, EEPROM in RAM. |

---

## How it works

[`PORTING.md`](PORTING.md) is the full dossier — around 2 500 lines, a dated
section per finding, written as the port was made. It is in French. The short
version:

- **`platform/gc/ultra/`** reimplements libultra on libogc2. The scheduler is
  the interesting one: it intercepts the RSP graphics and audio tasks the game
  submits and services them on the CPU and GX instead of on a signal processor.
- **`platform/gc/gfx/`** is the F3DDKR display-list interpreter: `G_VTX`,
  `G_TRI`, `G_SETCOMBINE` → TEV, `G_RDPSETOTHERMODE`, texture conversion and
  caching, the perspective divide moved onto the GP.
- **`platform/gc/audio/`** is the N64 audio microcode's ABI — all sixteen
  opcodes, including `A_POLEF` — running on the CPU, feeding libogc's AI.
- **The asset image lives in ARAM**, not main memory: 12 MB against MEM1's 24
  is too much to hold, and ARAM is otherwise idle. `osPiStartDma` becomes an
  ARAM DMA, which is the same operation the N64 was doing.
- **`platform/gc/gc_gzip.c`** hands the game's DEFLATE streams to zlib, because
  the original inflate loop exists only as 781 lines of MIPS assembly.

Both `ignored:` and `aud-ign:` in the heartbeat are empty: there is no opcode
the port drops.

## Known issues

- **One asset does not decompress.** Exactly one, once per session, in the
  menus. The bytes in the buffer do not occur anywhere in the ROM, so it never
  received asset data at all. Instrumented, not yet solved.
- **The audio has not been judged by ear** on hardware since the mixer bug that
  made it unlistenable was fixed. Latency is about 112 ms.
- **The ground reads as a flat light surface**, which stops shadows from being
  legible.
- **A card labelled "1" is drawn behind each character** on the selection
  screen.
- **A rare freeze after several presses of START** in the menus, not yet
  reproducible.
- Time Trial ghosts save to the emulated Controller Pak, but the full
  save-power-off-reload path has never been walked end to end.

## Credits

- [DavidSM64](https://github.com/DavidSM64) and every contributor to the
  [Diddy Kong Racing decompilation](https://github.com/DavidSM64/Diddy-Kong-Racing) — none of this exists without it.
- [Extrems](https://github.com/extremscorner) for libogc2.
- `aluzed/sm64-port-gc`, the reference this port read for every GX and audio
  ABI question.
- *Diddy Kong Racing* is © Nintendo / Rare. This project is not affiliated with
  either, distributes none of their data, and requires you to own the game.

## License

The port, like the decompilation it builds on, is released under
[CC0 1.0](LICENSE.md).
