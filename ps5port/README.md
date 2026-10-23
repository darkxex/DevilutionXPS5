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

- **Output.** Vulkan through RADV (Mesa's AMD driver, ported to the console) on `VK_KHR_display`: full screen at
  **3840x2160, about 60 Hz** (the mode closest to `RENDER_WIDTH` x `RENDER_HEIGHT` in `app/ps5_entry.c`). The game
  draws in software at its own resolution (720p by default) and the GPU scales it. Each frame goes up through a mapped
  staging buffer and a GPU copy, so the CPU does no tiling.
- **Build.** `build_ps5.py` cross-builds the core with its own `CMakeLists.txt` as the PC version, using the PS5 Clang
  wrapper (`tooling/prospero-clang18`). `tooling/ps5-cc` / `ps5-c++` are the compiler drivers cmake sees; they turn link
  steps into a `prospero-lld` call with the native app layout and link RADV into the title. `ps5-native-tool` converts
  the result to `eboot.bin`.
- **SDL2.** Built from [ps5-payload-dev/SDL](https://github.com/ps5-payload-dev/SDL) with the changes in `sdl2/`: a
  video driver that takes the game's window as the full-screen output, a small `SDL_Render*` driver on Vulkan
  (`ps5vk.c` is the presenter), and the app folder as `SDL_GetBasePath()`.
- **`app/`.** The process entry: sandbox exit (LegacyJB / etaHEN / OnionHEN request files, then the Lapy helper),
  the language (the console's, until the game's settings say otherwise), the display mode, and `--save-dir` /
  `--config-dir` pointing at `/data/DevilutionX/`.
- **Settings.** Fullscreen, Fit to Screen and Frame Rate Control are hidden (the console is always full screen and
  synchronised to the TV), and Nearest Pixel is the default scaling quality. `Source/options.cpp` does this under
  `DEVILUTIONX_FIXED_DISPLAY`, which only this port defines.

## Build

Needs `clang-18`, `ld.lld-18`, `cmake`, `ninja`, `smpq`, `python3`, `git` and `zip` (and `glslangValidator` only to
regenerate `sdl2/ps5vk_shaders.h` with `tools/make-vk-shaders.sh`). The dependencies go to `.deps/` and are not
tracked: the PS5 payload SDK and zlib (`tools/setup-native-dependencies.sh`), the Lapy helper sources
(`tools/build-lapy-helper.py`), and the RADV archive (`libvulkan_radeon.ps5.a`, with the Vulkan headers) in
`.deps/native/radv-release/` plus the SDK platform layer (`libps5platform.a`, `include/ps5platform`) in the SDK's
`target/`, both from the [PS5 Vulkan](https://github.com/mihawk-99/PS5_Vulkan) project. Those two are not downloaded
by a script yet.

```
python3 ps5port/build_ps5.py sdl         # SDL2 for the console, into .deps/native/sdl2
python3 ps5port/build_ps5.py configure   # cmake configure (cross build of the core)
python3 ps5port/build_ps5.py build
python3 ps5port/build_ps5.py package     # dist/PPSA98514 and dist/PPSA98514.zip
python3 ps5port/build_ps5.py deploy --ip <console>   # FTP to /data/homebrew/PPSA98514
```

The port version is `contentVersion` in `sce_sys/param.json`. The informational log lines of the Vulkan presenter are
compiled out; build the SDL driver with `-DPS5VK_DEBUG` to get them in the klog (`nc <console> 3232`).
