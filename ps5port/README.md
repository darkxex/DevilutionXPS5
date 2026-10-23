# DevilutionX for PS5 (native application)

Native PS5 application of [DevilutionX](https://github.com/diasurgical/devilutionX), title `PPSA98514`,
installed as a folder in `/data/homebrew/PPSA98514` and launched from the home screen. It lives only in this
folder: the game's core (`Source/`, `CMake/`, `3rdParty/`, `CMakeLists.txt`) is built unchanged, as the PC (Linux)
version. It is independent of `Packaging/ps5` (the payload-ELF port).

## Where the files go

| Where | What |
| --- | --- |
| `/data/homebrew/PPSA98514/` | The application (`eboot.bin`, `devilutionx.mpq`, `mods/hf.mpq`, `sce_sys/`). Written by the installer/FTP. |
| `/data/DevilutionX/` | **Your game files:** `diabdat.mpq` (Diablo) or `spawn.mpq` (shareware), `hellfire.mpq` with `hfmonk.mpq`, `hfmusic.mpq`, `hfvoice.mpq`, `hfbard.mpq`, `hfbarb.mpq`, `fonts.mpq`, translations, and `mods/`. The game also writes its saves and `diablo.ini` here. |

Copy the original game files with FTP before the first start.

## How it works

- **Output.** OpenGL 4.6 on the GPU through the PS5 OpenGL SDK (Mesa on AGC), full screen at **3840x2160, 60 Hz**.
  The game keeps its own resolution and the console scales it (`Upscale` is on).
- **Build.** `build_ps5.py` cross-builds the core with its own `CMakeLists.txt` as the Linux version, using the PS5
  Clang wrapper (`tooling/prospero-clang18`). `tooling/ps5-cc` / `ps5-c++` are the compiler drivers cmake sees; they
  turn link steps into a `prospero-lld` call with the native app layout. `ps5-native-tool` converts the result to
  `eboot.bin`.
- **SDL2.** Built from [ps5-payload-dev/SDL](https://github.com/ps5-payload-dev/SDL) with the changes in `sdl2/`: a
  window adapter that takes the game's window as the full-screen output, a small `SDL_Render*` driver on OpenGL, and the
  app folder as `SDL_GetBasePath()`.
- **`app/`.** The process entry: sandbox exit (LegacyJB / etaHEN / OnionHEN request files, then the Lapy helper), 1440p
  output mode, and `--save-dir` / `--config-dir` pointing at `/data/DevilutionX/`.

## Build

Needs `clang-18`, `ld.lld-18`, `cmake`, `ninja`, `smpq`, `python3`, `git` and `zip`. The PS5 payload SDK, the PS5
OpenGL SDK, zlib and the Lapy helper sources go to `.deps/` (not tracked; see `tools/setup-native-dependencies.sh`
and `tools/build-lapy-helper.py`).

```
python3 ps5port/build_ps5.py sdl         # SDL2 for the console, into .deps/native/sdl2
python3 ps5port/build_ps5.py configure   # cmake configure (cross build of the core)
python3 ps5port/build_ps5.py build
python3 ps5port/build_ps5.py package     # dist/PPSA98514 and dist/PPSA98514.zip
python3 ps5port/build_ps5.py deploy --ip <console>   # FTP to /data/homebrew/PPSA98514
```

The port version is `contentVersion` in `sce_sys/param.json`.
