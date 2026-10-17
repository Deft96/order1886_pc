# The Order: 1886 for PC

A Work in progress (W.I.P) native Windows port of The Order: 1886 (PS4), running the game's
own x86-64 code directly with a reimplemented Orbis OS and a GNM-to-Vulkan translation layer.
No game files are included; you need your own copy of the game.

This repository: <https://github.com/Deft96/order1886_pc>

## Contents

- [Requirements](#requirements)
- [Starting](#starting)
- [Controls](#controls)
- [Status](#status)
- [Performance and compatibility](#performance-and-compatibility)
- [Data](#data)
- [Building from source](#building-from-source)
- [Tests](#tests)
- [How it works](#how-it-works)
- [Credits](#credits)
- [License](#license)

## Requirements

- Windows 10 (1903 or newer) or Windows 11, 64-bit.
- A 64-bit processor with AVX (Intel Sandy Bridge or newer, AMD FX or newer).
- A graphics card with Vulkan 1.3 and an up-to-date driver.

## Starting

1. Extract the folder anywhere (for example `C:\Games\The Order 1886`).
2. Open `The Order 1886.exe`. If Windows shows "Windows protected your PC", click More
   info, then Run anyway (The program is not digitally signed).
3. Choose the game folder (the one with `eboot.bin`, `sce_module` and `sce_sys`).
   Tested title ID: CUSA00035, version 01.02 (a folder with the 1.02 update applied works).
4. Click PLAY. The first playthrough prepares the game image and compiles the shaders, as normal.

## Controls

- PC Controls are still inherited from gow3_pc (NOT adjusted yet! - Controller is still recommended for the best experience).
- WASD: move · arrows: camera · Space: Cross · Left Shift: Circle · E: Square · Q: Triangle
- 1/3: L1/R1 · R/F: L2/R2 · Z/C: L3/R3 · I/K/J/L: d-pad · Enter: Options · Backspace: pause
- Insert (or R3 + L2 on the controller): the port's settings menu (frame rate, overlay...).

## Status

- Playable (Experimental/WIP): Intro, gameplay, QTEs, saves. Up to 60FPS might be possible with capable hardware, not tested yet. 30FPS is still the baseline/mainstream to aim for.
- Known issues: The first visit of an area stutters and hitches (streaming), the first QTE attempt can
  fail and passes on the retry checkpoint, heavy open areas can drop to the low 20s FPS (the submission
  cost — a single-threaded limit, until correctly implemented), and the depth of field stays blurry when aiming. Crashes (Game/GPU Driver), Visual artifacts, glitches and bugs are expected for now.

## Performance and compatibility

Reference testing on an AMD Radeon RX 6700 XT, Ryzen 7 5700X and 32 GB holds 30 FPS locked in
most scenes, rising to 60 in lighter areas, at native 1080p. Heavy open areas (the London
rooftops) can drop to the low 20s FPS — the single-threaded submission cost, not the GPU
(which sits at ~70% there). These are reference results, not a benchmark of every release or
a guarantee for other systems.

Tested from the intro through gameplay, QTEs and saves at native 1080p on the test system.
Crashes and visual artifacts have also been reported on other setups; see the known issues
in the release notes.

## Data

- Saves and shader cache are in: `user\` next to the exe. (Keep it when updating).

## Building from source

### Windows

Install Python 3.10 or newer from python.org, with Tk support, then install the launcher
dependencies in PowerShell:

```powershell
python -m pip install pillow iced-x86==1.21.0 pyinstaller
```

The following build commands use MSYS2 CLANG64.

1. Install MSYS2 (`winget install MSYS2.MSYS2`) and, in the **MSYS2 CLANG64** shell, the
   packages listed in [packaging/windows/README.md](packaging/windows/README.md).
2. Clone the repository with its submodules and build:

   ```bash
   git clone --recursive https://github.com/Deft96/order1886_pc
   cd order1886_pc
   bash build.sh
   ```

3. Start the launcher (in PowerShell, in the project folder):

   ```powershell
   python launcher/order1886_launcher_win.py
   ```

4. On **Game**, choose the game folder. Adjust **Graphics** and **Performance**,
   click **Save** for pending changes, then **PLAY**.

To play with the saved settings without opening the launcher's window (for a shortcut or Steam):

```powershell
python launcher/order1886_launcher_win.py --play
```

Or without the launcher at all:

```powershell
$env:ORDER1886_GAME_DIR = 'D:\path\CUSA00035'
python run.py
```

To package the release ZIP (after the build):

```bash
bash packaging/windows/package.sh
```

## Tests

Run these from the repository root after the build. The C++ checks below do not need a game dump.

### Python

On Windows, use PowerShell:

```powershell
python -m unittest discover -s tests -p "test_*.py"
```

### Windows C++ tests (MSYS2 CLANG64)

```bash
source ./msys2-env.sh
ninja -C out/gpu perf-overlay-test warmup-inbox-test graphics-settings-test
out/gpu/perf-overlay-test.exe
out/gpu/warmup-inbox-test.exe
out/gpu/graphics-settings-test.exe
```

More test targets are listed in `gpu/CMakeLists.txt`.

## How it works

The CPU side is native: the game's original executable runs directly on the PC's processor
(the PS4 uses the same x86-64 architecture), and a runtime written for this game replaces the
PS4 system libraries, without CPU emulation. The graphics are translated: they run on the
shadPS4 video core, which converts the PS4 GPU commands and shaders to Vulkan. Running the
game code directly on the CPU is also how shadPS4 works; what this project changes is the
system layer and the game-specific work (the sync-interval patch, the particle barriers, the
upload queue, the SRT filters, the launcher and the in-game menu). It is a fork of
FelipePrado0's Windows port of Supermedo's bloodborne_pc (see Credits).

The sync-interval patch and the in-game hooks find their sites by signature: the original
bytes around each one, wherever a release placed that code (`scripts/patches.py`). A site
that is not found leaves only that patch or hook off; the game starts without it, and the
log and the menu say why.

| Part            | Where                                    | What it does                                                                                                            |
|-----------------|------------------------------------------|-------------------------------------------------------------------------------------------------------------------------|
| Preparation     | `scripts/`                               | Reads `eboot.bin`, applies its relocations and links the game's own libc and libSceFios2 into one image                |
| Loader          | `src/probe.c`                            | Loads the image, applies the release defaults and starts the game's original code                                       |
| Runtime         | `src/runtime_*.c`                        | Reimplements the PS4 system functions: memory, threads, files, audio, video, controller, saves and services            |
| GPU             | `gpu/shadps4/`                           | The shadPS4 video core, translating the PS4 GPU commands and shaders to Vulkan, with the Order: 1886 fixes (SRT filters, upload queue, particle barriers, DMA) |
| In-game menu    | `gpu/shim/gow3_overlay.cpp`              | The Dear ImGui menu and performance overlay; settings in `gpu/shim/gow3_settings.cpp`                                  |
| Patches         | `patches/The_Order_1886.xml`, `scripts/patches.py` | Applied in memory at start, without changing the game files                                                            |
| Launcher        | `launcher/order1886_launcher_win.py`     | Settings and the play button                                                                                            |

More documentation: the parallel GPU work in `docs/` (the two-stage draw pipeline).

## Credits

This project is a fork of **FelipePrado0/gow3_pc** (the God of War III Remastered PC port),
which builds on **Supermedo's** Windows port of **deadinside28's** original:

- **deadinside28**: bloodborne_pc, the original native port.
- **Supermedo** (Mohammed Albarghouthi): bloodborne_pc for Windows, the fork this lineage comes from.
- **FelipePrado0**: gow3_pc for Windows, the direct base of this port (launcher, patches system, warm-up, draw pipeline).
- **TdlifePeng**: the shadPS4 `1886` branch with the The Order: 1886 graphics fixes this port follows (SRT filter, VSM hack, DMA).
- **shadPS4 team**: the shadPS4 video core, which does all the GPU and shader translation to Vulkan in this port.

Also used: LibAtrac9, FFmpeg, SDL3, Dear ImGui, sirit, magic_enum, miniz, xbyak, Vulkan Memory Allocator, FSR-Vulkan and the AMD FidelityFX SDK, MSYS2, LLVM and Pillow.

This project is not affiliated with Sony Interactive Entertainment or Ready at Dawn.
The Order: 1886 is a trademark of Sony Interactive Entertainment.

## License

GNU GPL v2 or later (`LICENSE`). Third-party components keep their own licenses.

## Wanna buy me a coffe? I'd appreciate it a lot!

https://ko-fi.com/odefas <3
