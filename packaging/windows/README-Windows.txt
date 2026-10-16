The Order: 1886 for PC (order1886_pc)
======================================

No game files are included. You need your own copy of The Order: 1886 as a folder containing
eboot.bin, sce_module and sce_sys. Tested title ID: CUSA00035, version 01.02. Other builds may
work, but are not guaranteed.

Requirements
- Windows 10 (1903 or newer) or Windows 11, 64-bit.
- A 64-bit processor with AVX (Intel Sandy Bridge or newer, AMD FX or newer).
- A graphics card with Vulkan 1.3 and an up-to-date driver.
- Nothing else: Python and the libraries are inside this folder.

Starting
1. Extract this folder anywhere, for example C:\Games\order1886-windows (not inside the zip).
2. Open "The Order 1886.exe". If Windows shows "Windows protected your PC", click More info,
   then Run anyway: the program is not digitally signed.
3. Click "Choose the game folder", then Browse... next to Game folder, and select the folder
   with eboot.bin.
4. Back on Home, adjust the quick settings if you like, then click PLAY. The first start takes
   longer while the game image is prepared and the shaders compile.

The particle fix (VSync off) is included and applied automatically. The port's menu (Insert on
the keyboard, or R3 + L2 on the controller) has the frame rate limit (30/60), the resolution,
the performance overlay and the other settings.

While playing
- The first visit of each area can stutter (texture streaming); the first QTE attempt can fail
  and passes on the retry. Particles (bubbles, splashes, blood) update at half the frame rate.
- Keyboard: WASD move, arrows camera, Space Cross, Left Shift Circle, E Square, Q Triangle,
  1/3 L1/R1, R/F L2/R2, Z/C L3/R3, I/K/J/L d-pad, Enter Options, Backspace pause menu.

Next time
- "Play The Order 1886.exe" starts the game straight away with the saved settings. Make a
  desktop shortcut to it, or add it to Steam with "Add a Non-Steam Game".

Data
- Saves, shader cache and the log: user\ next to The Order 1886.exe (keep it when updating).
- Settings: order1886.ini next to The Order 1886.exe; launcher options in %APPDATA%\gow3-launcher.
- Generated files (prepared game image, patches): out\.

Problems
- The game does not open: check the log and report it with the GPU, driver and game build.
- Known issues are tracked in the project's README and issues.

Credits
- This port is a fork of FelipePrado0/gow3_pc, based on Supermedo/bloodborne_pc, with graphics
  workarounds following the TdlifePeng/shadPS4 1886 branch and the shadPS4 project.
- This project is not affiliated with Sony Interactive Entertainment or Ready at Dawn.
  The Order: 1886 is a trademark of Sony Interactive Entertainment.
