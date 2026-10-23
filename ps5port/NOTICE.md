# Third-party code in `ps5port/`

It builds on, or carries code from:

| Piece | Where | Licence |
| --- | --- | --- |
| SDL2 (built from the PS5 fork, with the changes in `sdl2/`) | `sdl2/` patches and sources | zlib |
| Native app tooling: startup code, linker layout, converter, heap, the clean-room libc shim (`runtime/libc.prx`) | `tooling/`, `tools/`, `runtime/` | GPL-3.0-or-later, BlackBearReloaded |
| RADV link recipe and the AGC link stubs | `tooling/radv/`, `tooling/psbc/`, `tooling/ps5-stubs/` | GPL-3.0-or-later, Mihawk-99 (PS5 Vulkan) |
| RADV (Mesa's AMD Vulkan driver for the console) and the SDK platform layer, used as prebuilt libraries | `.deps/` (not tracked) | Mesa's own licences; GPL-3.0-or-later for the PS5 parts (PS5 Vulkan) |
| Sandbox exit: `ps5_unlock`, the Lapy elevation client and helper build script | `app/`, `tools/build-lapy-helper.py` | GPL-3.0-or-later; the Lapy helper itself is MIT (`Lapy-MIT.txt` is packaged with it) |
| PS5 payload SDK | `.deps/` (not tracked) | GPL-3.0 |

The game and its own dependencies keep their licences, listed in `license/`.

DevilutionX itself is distributed under the Sustainable Use License (see `LICENSE.md` at the root of the repository).
The conditions of that licence and of the GPL-3.0 parts above have not been reconciled for one combined binary, so
this port is offered for personal, non-commercial use only and without warranty.
