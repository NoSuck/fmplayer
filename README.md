# Fmplayer (beta)
PC-98 FM driver emulation (very early version)
![gtk screenshot](/img/screenshot_gtk.png?raw=true)
![gtk toneviewer screenshot](/img/screenshot_gtk.toneview.png?raw=true)
![w2k screenshot](/img/screenshotw2k.png?raw=true)

## Current status:
* Supported formats: PMD, FMP(PLAY6)
* PMD: FM, SSG, Rhythm, ADPCM, PPZ8(partially) supported; PPS, P86 not supported yet
* FMP: FM, SSG, Rhythm, ADPCM, PPZ8, PDZF supported
* This is just a byproduct of reverse-engineering formats, and its emulation is much worse than PMDWin, WinFMP
* FM always generated in 55467Hz (closest integer to 7987200 / 144), SSG always generated in 249600Hz and downsampled with sinc filter (Never linear interpolates harmonics-rich signal like square wave)
* FM generation bit-perfect with actual OPNA/OPN3 chip under limited conditions including stereo output when 4 <= ALG (Envelope is not bit-perfect yet, attack is bit-perfect only when AR >= 21)
* SSGEG, Hardware LFO not supported
* PPZ8: linear interpolation only (same as PMDWin/WinFMP, much better than original ppz8.com which only did nearest-neighbor interpolation)

## Installation/Usage (not very usable yet)
### gtk
Uses gtk3, portaudio
```
$ cd gtk
$ autoreconf -i
$ ./configure
$ make
$ ./fmplayer
```
Reads drum sample from `$HOME/.local/share/fmplayer/ym2608_adpcm_rom.bin` (same format as MAME).
Currently needs `$HOME/.local/share/fmplayer/font.rom` to display titles/comments.

### win32
Releases:
https://github.com/takamichih/fmplayer/releases/

Uses MinGW-w64 to compile.
```
$ cd win32/x86
$ make
```
Reads drum sample from the directory in which `fmplayer.exe` is placed.
Uses DirectSound (WinMM if there is no DirectSound) to output sound. This works on Windows 2000, so it is  theoretically possible to run this on a real PC-98. (But it was too heavy for my PC-9821V12 which only has P5 Pentium 120MHz, or on PC-9821Ra300 with P6 Mendocino Celeron 300MHz)
