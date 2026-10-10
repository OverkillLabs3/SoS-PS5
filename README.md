# SonsOfSparta-PS5 Native

**God of War: Sons of Sparta for PS5, running on Windows through AnyPS5. The compatibility layer also builds and runs on Linux - see [Linux](#linux).**

Made possible by the [AnyPS5](https://github.com/boykopovar/AnyPS5) project. The source code is in this repository and the build is on the [Releases](../../releases) page.

![God of War: Sons of Sparta running on Windows at 60 FPS](screenshot.jpg)

## How to play
1. Download `SonsOfSparta-PS5-Native-0.4.1-win64.zip` and extract it anywhere.
2. Copy **your own** decrypted game files into the same folder as `SonsOfSparta-PS5.exe`:
   `eboot.bin` (or a decrypted `eboot.elf`), `sce_sys`, `sce_module` and `Media`.
3. Double-click `SonsOfSparta-PS5.exe`, check the settings and press **Play**.

Nothing to install. The very first start prepares the game from your files: a small window explains it and it takes about two minutes. Later starts are fast. Your own files are never modified.

Closing the game window ends it within a few seconds. Logs are in the `logs` folder.

## Highlights
- The game's own x86-64 code runs directly on your CPU, with no CPU emulation. The PS5 system libraries and GPU commands are reimplemented through a compatibility layer, similar to Wine and DXVK
- Playable from the logo through gameplay, with music and sound effects
- About 60 FPS in menus and gameplay on our test PC (RTX 5070 Ti), with optional 120 FPS and unlocked frame rate in the launcher
- Keyboard and any XInput/DualSense/DualShock gamepad; saves are kept next to the game

## Controls
Any XInput/DualSense/DualShock gamepad works. Keyboard, mouse and gamepad can be used at the same time.

| PS5 button | Keyboard | Mouse |
|---|---|---|
| Cross | Enter / Space | |
| Circle | C | |
| Square | | Left click |
| Triangle | I | |
| L1 | Q | |
| R1 | E / Alt | |
| L2 | (no key) | |
| R2 | | Right click |
| L3 / R3 | Shift / Ctrl | |
| D-pad | Arrow keys | Mouse wheel (up = Up, down = Down) |
| Left stick | W A S D | |
| Right stick | T F G H | |
| Options | Esc | |
| Touchpad | Backspace / Tab | |

F11 toggles fullscreen. The middle mouse button turns mouse-aim mode on or off (the mouse is captured and drives the right stick).

**Enable cheats** in the launcher turns the runtime cheat system on. It is off by default, and it is read when the game starts: to use cheats, tick it in the launcher and start the game again. When it is on, F1-F9 turn the cheats on and off, and every cheat starts off each time the game starts. F10 opens and closes the In-Game Cheat Menu on the right side of the screen, which is mouse and keyboard usable. Closing the menu with its X button or F10 does not turn any active cheat off. While the menu is open the game receives no controller, keyboard or mouse input. When the option is off, F1-F10 do nothing and the menu does not appear. F11 always toggles Borderless Fullscreen, and F12 is unused.

**Show in-game FPS** is a launcher Quality of Life option. It is off by default and draws the frame rate in the top-left corner of the game. It does not block any input, and it works with or without Enable cheats. The frame rate in the window title bar is separate and always shown.

## Display settings
The window that opens when you start `SonsOfSparta-PS5.exe` has two display settings. Press Play to save them; they are kept for later starts.

**Display mode:** Windowed or Borderless Fullscreen. With Borderless Fullscreen, the launcher switches the game window to fullscreen once it appears, the same as pressing F11. F11 still switches between the two while playing. The choice is stored in `launcher.ini`.

**Resolution:** the game draws its scene at 4K by default and scales it to your window, which is heavy on weaker graphics cards. To lower it, choose 2560 x 1440 or 1920 x 1080. The choice is saved in the game's own settings. The window size is not affected.

If you created `resolution.txt` with an earlier version, its value is preselected in that window. Once you press Play and the choice is saved, the file is renamed to `resolution.txt.old`.

## Known issues
- **First play builds shaders:** the first time you play, shaders are built while you play, so you may see visual glitches (missing or wrong-looking effects, brief stutters) until they are cached. This gets better on later runs.
- Rare crashes or freezes can still happen, mostly during the intro or loading. Just start the game again.
- The whole game has not been completed, so not all of it is tested. Later areas may have missing graphics, audio problems or crashes.
- Tested on an NVIDIA graphics card only. AMD and Intel are untested.

## Notes
- You must supply your own, legally obtained, decrypted copy of the game. No game files are included.
- **Tested with:** God of War: Sons of Sparta, title ID `PPSA28997`, version `01.008.001`. Other versions, regions, patches or repacked dumps have not been tested and may not work.
- SmartScreen may warn (*More info* → *Run anyway*). Verify with the checksum.
- Requires Windows 10/11 64-bit and a graphics card with a Vulkan 1.3 driver. About 25 GB of free space is needed for the game files you copy in.
- The packaged download is Windows only. Linux is supported from source: the compatibility layer builds and the game runs at 60 FPS, but there is no Linux launcher yet. See [Linux](#linux).

## Building from source

You need Windows 10/11, MinGW-w64 GCC 15.2 (winlibs, `x86_64-ucrt-posix-seh`), CMake 3.20 or newer, Ninja and Git. `g++`, `cmake` and `ninja` must be on your `PATH`.

```
git clone --recursive https://github.com/OverkillLabs3/SoS-PS5.git
cd SoS-PS5
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target libs relinker SonsOfSparta-PS5
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Version 0.4.1
```

The first configure downloads prebuilt FFmpeg libraries from GitHub, so it needs internet access. The first build takes a while.

The result is `release\SonsOfSparta-PS5-Native-0.4.1-win64`, the same layout as the download. Copy your own game files next to `SonsOfSparta-PS5.exe` as described above.

## Linux

The compatibility layer builds natively on Linux, and the game reaches its main menu and holds 60 FPS in
gameplay. **There is no Linux launcher and no packaged Linux build yet**, so this is a source-only path for
now. What the Windows launcher does for you on first start has no Linux equivalent in this repository:

- converting `eboot.bin` to an ELF, and converting each bundled `sce_module` to the `.prx.guest.prx` form the
  loader expects;
- building a stub library for the PS5 imports the compatibility layer does not implement, so the relink can
  resolve them;
- seeding the settings file, then relinking with `--registry --skip-syscall-check`.

Until those exist, running on Linux means doing that work yourself with your own tooling. The build itself is
supported and is what the rest of this section covers.

### Building

You need GCC with C++20, CMake 3.20 or newer, Ninja and Git. `g++`, `cmake` and `ninja` must be on your
`PATH`. Tested on Arch Linux with GCC 15.2, an NVIDIA card on driver 615.71.09, under a Wayland compositor.
AMD and Intel are untested. A Vulkan 1.3 driver is required, as on Windows.

```
git clone --recursive https://github.com/OverkillLabs3/SoS-PS5.git
cd SoS-PS5
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DANYPS5_BSYMBOLIC_PRX=ON
cmake --build build --target libs relinker
```

`--recursive` matters: the tree uses submodules, including `3rdparty/imgui`.

Keep `-DANYPS5_BSYMBOLIC_PRX=ON`. It makes each host library bind references to its own exports, the way a PE
DLL does. Without it, a game that ships its own `libc.prx` interposes the host libc instead (upstream AnyPS5
issue #476). The option defaults to off.

This builds the host libraries into `build/core/libs/libs/` and the relinker into
`build/core/relinker/relinker`. FFmpeg comes from the `3rdparty/ffmpeg-core` submodule, so the recursive clone
is what fetches it.

### Two settings worth knowing

- **`APS5_GC_WAIT_MS`** sets how long a guest exception round waits for the target thread. It is read on both
  platforms, and the default is 1000 ms. A game that parks threads pays that deadline on every round, so on
  Linux start-up to the main menu measured 1,635 s at the default against 227.7 s at 100 ms, 106.4 s at 20 ms
  and 86.3 s at 5 ms. Set it to `5`.
- **`shader_cache`** is written next to the game while you play. Deleting it costs about ten minutes of shader
  recompilation on the next run and the game looks hung at `FPS: 0.05` while that happens. Keep the folder.

### Frame rate

The engine reads **`APS5_FPS_CAP`** at the first flip: a number, accepted only between 30 and 1000, for example
`APS5_FPS_CAP=120`. Unset, or outside that range, leaves the default pacing, which follows the display refresh -
59.94 Hz on a 60 Hz mode. The value is read once, so set it before starting. On Windows the same variable can be
given through `debug_env.txt` next to the launcher, whose `KEY=VALUE` lines are loaded into the environment.

## License

GNU General Public License version 2. Parts of the code derive from other GPL projects and keep their original notices in the source files.

God of War is a trademark and copyright of its respective owners; this project is not affiliated with or endorsed by them.
