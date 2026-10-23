# Third-party code in `ps5port/`

The port is distributed under the same licence as the game (see `LICENSE.txt` at the root of the repository).
It builds on, or carries code from:

| Piece | Where | Licence |
| --- | --- | --- |
| SDL2 (built from the PS5 fork, with the changes in `sdl2/`) | `sdl2/` patches and sources | zlib |
| PS5 OpenGL SDK's SDL2 integration (window adapter, build files) | `sdl2/` | GPL-3.0-or-later, BlackBearReloaded |
| PS5 OpenGL SDK (Mesa on AGC), used as a prebuilt library | `.deps/` (not tracked) | GPL-3.0-or-later and the licences of Mesa and its dependencies |
| Native app tooling: startup code, linker layout, converter, heap, the clean-room libc shim (`runtime/libc.prx`) | `tooling/`, `tools/`, `runtime/` | GPL-3.0-or-later, BlackBearReloaded |
| Sandbox exit: `ps5_unlock`, the Lapy elevation client and helper build script | `app/`, `tools/build-lapy-helper.py` | GPL-3.0-or-later; the Lapy helper itself is MIT (`Lapy-MIT.txt` is packaged with it) |
| PS5 payload SDK | `.deps/` (not tracked) | GPL-3.0 |

The game and its own dependencies keep their licences, listed in `license/`.

DevilutionX itself is distributed under the Sustainable Use License (see `LICENSE.md` at the root of the repository).
