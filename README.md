# Ghoulies PC Recomp: Grabbed by the Ghoulies, recompiled for PC

This is a native Windows build of **Grabbed by the Ghoulies** (original Xbox, 2003). The game's code is statically recompiled to C and runs on a PC reimplementation of the Xbox kernel, the GPU (NV2A, on Direct3D 11) and audio.

It is built on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nznet (MIT, with xemu-derived parts under LGPL-2.1), using the toolkit changes from [Orta PC Recomp](https://github.com/SeveerJ/Orta_PC_Recomp). Many GPU behaviours are matched against [xemu](https://xemu.app).

> **Status:** test build for playtesters. The game is playable, including cutscenes, challenges and the Reaper. See [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).

## You need your own copy of the game

This repository contains **no game code or assets**: no XBE, no disc files, no recompiled game code and no textures. You supply files from your own disc. Only the **North American (US) release** is supported. Setup checks the game's `default.xbe` before it does anything.

## Requirements

- Windows 10 or 11, 64-bit
- A DirectX 11 graphics card
- An Xbox controller or any XInput pad
- Your own copy of Grabbed by the Ghoulies (US)

## Repository layout

| Path | What it is |
|---|---|
| `xboxrecomp-main/` | The xboxrecomp toolkit with this project's changes. It contains the recompiler (`tools/`) and the runtime (`src/`): kernel, NV2A GPU on Direct3D 11, audio, input and Bink video. |
| `ghoulies/` | The game project. `main.c` handles settings.ini and start-up. `recomp_manual.c` holds hand-written replacements, such as real-time frame timing and the Bink copy wrapper. `overlay.cpp` is the F1 settings menu. Also here: the build scripts and the playtest package template. |
| `ghoulies/seeds/` | Function addresses found while playing, which the recompiler needs. They are addresses only, with no game code. |
| `ghoulies/third_party/imgui/` | Dear ImGui (MIT) |
| `installer/` | Setup.exe source and the script that builds the game on the player's PC |
| `docs/` | Known issues and changelog |

## Installing

Download `GhouliesPC_Setup.exe` from the Releases page and run it.

- Setup looks for your disc image (`.iso`) by itself, in Downloads, Desktop, Documents, Videos, your user folder and then each drive. If it doesn't find it, press **Disc image...**, or **Folder...** for an unpacked copy with `default.xbe` in it.
- It checks that the disc is Grabbed by the Ghoulies, North American release.
- It downloads Python, the capstone disassembler and the llvm-mingw compiler (about 300 MB) into `%LOCALAPPDATA%\Ghoulies PC Setup`. It then translates and compiles the game, which takes about 20-30 minutes, installs it where you chose, and deletes that work folder.
- At the end it prepares the graphics shaders the game needs, so the first start is quick.
- The install folder gets `setup_log.txt`. Send it with any problem report. If Setup stops part-way, run it again: it reuses what it already downloaded and compiled.

## Settings and files

- **Launcher:** shown the first time the game starts, and whenever Shift is held while it starts: resolution, display mode, widescreen, VSync, anti-aliasing, supersampling, frame rate, volume, sound effects, speakers and rumble.
- **F1 menu** while playing: resolution, widescreen, anti-aliasing, supersampling, frame-rate cap, VSync, volume and rumble. Everything is saved to `settings.ini` next to the game.
- **Anti-aliasing:** Off, Smooth scaling, or Smooth scaling + FXAA in four strengths: fast, quality, high and ultra (`AntiAliasing = 0` to `5` in `[Display]`).
- **Supersampling:** draws the game at 1.5x, 2x or the most (2880 lines) of your screen's height and shrinks it to fit, for the cleanest edges (`Supersampling = 0` to `3`). It is the heaviest on the graphics card.
- **Surround:** `Speakers = 5.1` in `[Audio]` (or the launcher / F1 menu) gives 5.1 sound, with sounds behind you on the rear speakers. Takes effect when the game starts.
- **60 fps:** the game keeps real-time speed at any frame rate (`FrameRateCap = 60`, or 0 for no cap).
- **Alt+Enter** switches fullscreen. **F11** saves a frame capture for bug reports.
- **Data folder** next to the game: saves (`Data\UserData`, `Data\TitleData`) and the game's cache (`Data\Cache`). If the game folder can't be written to, it goes in `%LOCALAPPDATA%\Ghoulies PC` instead. Deleting `Data\Cache` is safe.
- **Shaders:** compiled graphics shaders are kept in `shadercache` next to the game. Setup prepares them, so the first start is quick. Deleting the folder is safe; the next start rebuilds it.
- **Problem reports:** each run writes `session_log.txt` next to the game, and keeps the previous run's log as `session_log_previous.txt`. Send these with any report.

To make the installer yourself, `installer/build_setup.sh` builds the `Setup.exe` stub (clang and lld-link, no C runtime). Then `python installer/make_release.py GhouliesPC_Setup.exe` appends the sources to it.

## Building by hand

You need Windows 10/11 with Visual Studio 2022 (Desktop C++), CMake 3.20+ and Python 3 with `capstone`.

1. Copy `default.xbe` and the rest of the disc's files from **your own copy of the game** into `xboxrecomp-main/game_files/`.
2. Copy the three files from `ghoulies/seeds/` into the same folder.
3. From a Developer PowerShell for VS 2022, in `ghoulies/`, run `powershell -ExecutionPolicy Bypass -File .\ghoulies_loop.ps1`. It disassembles the game and translates it to C in `ghoulies/src/recomp/gen/`; this output is never committed. It then builds with CMake into `build64/` and starts the game.

The toolkit's own guide, `xboxrecomp-main/docs/GETTING_STARTED.md`, explains the pipeline in depth.

## Licence

Code from xboxrecomp keeps its MIT licence (`xboxrecomp-main/LICENSE`). The xemu-derived files keep their LGPL-2.1-or-later licence and notices (`xboxrecomp-main/NOTICE`, `xboxrecomp-main/LICENSES/`). Dear ImGui is MIT (`ghoulies/third_party/imgui/LICENSE.txt`).

Grabbed by the Ghoulies is © Microsoft / Rare. This project is not affiliated with or endorsed by them.
