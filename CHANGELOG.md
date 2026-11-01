# pico-1943

An emulator of the arcade games 1943: The Battle of Midway and 1943 Kai: Midway Kaisen (Capcom, 1987) for RP2350 boards with DVI/HDMI output.

# General Info

[Binaries for each configuration and PCB design are at the end of this page](#downloads___).

[See the readme for the supported boards, the game data and the controls](https://github.com/PicoPlus-devel/pico-1943#readme)

> [!IMPORTANT]
> An **RP2350** board is required. The original RP2040 (Pico 1) is not supported.

# Changelog

## v0.2

- **No more dots or lines in the picture with Overclock on.** On boards with HSTX video, some TVs and monitors showed small dots or short dotted lines while Overclock was on. The picture is now clean, and the built-in USB port can still be used for controllers and USB drive mode. Thanks to @szuping for the hint that led to this fix.
- **The Video Clock Fix setting has been removed.** It is no longer needed. On a board where it was turned on, a USB controller on the built-in USB port works again.
- **Overclock** now runs the board at 384 MHz instead of 378 MHz.
- **No scanlines on boards with PicoDVI output.** With *Tate mode* off they did not display well, so the scanline screen mode is no longer offered on these boards. A saved scanline setting switches to the mode without scanlines.
- **USB keyboard in the menus.** Enter selects and Esc goes back.
- Fixed: on the Adafruit Fruit Jam and other boards with PIO-USB, USB controllers were often not found at start-up.

## v0.1.2

- ~~**New setting: Video Clock Fix** (Pico 2 or PicoNES PCB, Adafruit Metro RP2350, Murmulator M2 and Olimex RP2040-PICO-PC). Turn it on if your TV or monitor shows small dots or lines in the picture with Overclock on. A USB controller can then no longer be used; use a NES, SNES or Wii controller instead.~~ Removed in v0.2.
- ~~On the Adafruit Fruit Jam and Feather RP2350 the Video Clock Fix is always on.~~

## v0.1.1

- Fixed: on boards without PSRAM, such as a Pico 2 on the PicoNES PCB, start-up stopped with an "Out of memory" error when the ROM sets were on the SD card as unzipped files, the way updateAll installs them.

## v0.1

Initial release.

- 1943 and 1943 Kai with sound, on every RP2350 board of this family. With both ROM sets on the card, a menu at start-up chooses the game.
- The game ROMs are not included. Copy MAME's `1943.zip` to `/roms/arcade/1943` and `1943kai.zip` to `/roms/arcade/1943KAI` on the SD card, as they are or unzipped. The US, European and Japanese versions of 1943 all play. Missing files are named on screen.
- On boards without PSRAM, a game is copied into the flash memory of the board the first time it is started. This happens once and takes a short while; the board restarts for it.
- Two players can play at the same time, the second on a second controller.
- **Tate mode** setting: the picture is turned upright for a normal screen, or shown unrotated for a monitor turned on its side, either way round.
- **Overclock** setting on boards with HSTX video output (for example the Fruit Jam): 378 MHz instead of 252 MHz. The board restarts to apply it.
- Can be started from pico-bootLoader, where it appears in the Arcade category.
- The release includes updateAll.exe, which installs the ROM sets on the SD card from a Windows computer. The readme explains how to use it.

<a name="downloads___"></a>
## Downloads by configuration

The ROM set installer for Windows: [updateAll.exe](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/updateAll.exe)

Binaries for each configuration are listed below. Only RP2350-based boards are supported; no RP2040 / Pico 1 binaries are provided.

Wiring and assembly are the same as for the NES emulator; see the [pico-infonesPlus documentation](https://github.com/PicoPlus-devel/pico-infonesPlus#setup) for the RP2350 boards listed below.

### Standalone boards

| Board | Binary |
|:--|:--|
| Adafruit Metro RP2350 | [pico1943_AdafruitMetroRP2350_arm.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_AdafruitMetroRP2350_arm.uf2) |
| Adafruit Fruit Jam | [pico1943_AdafruitFruitJam_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_AdafruitFruitJam_arm_piousb.uf2) |
| Adafruit Feather RP2350 with TLV320DAC3100 | [pico1943_AdafruitFeatherRP2350_TLV320DAC3100_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_AdafruitFeatherRP2350_TLV320DAC3100_arm_piousb.uf2) |
| Waveshare RP2350-PiZero | [pico1943_WaveShareRP2350PiZero_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_WaveShareRP2350PiZero_arm_piousb.uf2) |

### Breadboard

| Board | Binary |
|:--|:--|
| Pico 2 | [pico1943_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_AdafruitDVISD_pico2_arm.uf2) |
| Pimoroni Pico Plus 2 | [pico1943_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_AdafruitDVISD_pico2_arm.uf2) |

### PicoNES PCB (PCB required)

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [pico1943_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_AdafruitDVISD_pico2_arm.uf2) |
| Pimoroni Pico Plus 2 | [pico1943_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_AdafruitDVISD_pico2_arm.uf2) |

PCB: [pico_nesPCB_v2.6.zip](https://github.com/PicoPlus-devel/pico-infonesPlus/releases/latest/download/pico_nesPCB_v2.6.zip). [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus#pcb-with-raspberry-pi-pico-or-pico-2-and-pimoroni-pico-plus-2)

3D-printed case: [thingiverse.com/thing:6689537](https://www.thingiverse.com/thing:6689537). When the board is fitted on male headers, use the latest top cover; the older covers assume a Pico soldered flat and leave no room for the USB cable.

### PicoNES Mini PCB, Waveshare RP2350-Zero (PCB required)

| Board | Binary |
|:--|:--|
| Waveshare RP2350-Zero | [pico1943_WaveShareRP2350ZeroWithPCB_arm.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_WaveShareRP2350ZeroWithPCB_arm.uf2) |

PCB: [Gerber_PicoNES_Mini_PCB_v2.0.zip](https://github.com/PicoPlus-devel/pico-infonesPlus/releases/latest/download/Gerber_PicoNES_Mini_PCB_v2.0.zip)

3D-printed case: [thingiverse.com/thing:7041536](https://www.thingiverse.com/thing:7041536)

### PicoNES Micro PCB, Waveshare RP2350-USBA (PCB required)

[Binary](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_WaveShare2350USBA_arm_piousb.uf2)

PCB: [Gerber_PicoNES_Micro_v1.2.zip](https://github.com/PicoPlus-devel/pico-infonesPlus/releases/latest/download/Gerber_PicoNES_Micro_v1.2.zip)

[Build guide](https://www.instructables.com/PicoNES-RaspberryPi-Pico-Based-NES-Emulator/)

### Pimoroni Pico DV

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [pico1943_PimoroniDVI_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_PimoroniDVI_pico2_arm.uf2) |
| Pimoroni Pico Plus 2 | [pico1943_PimoroniDVI_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_PimoroniDVI_pico2_arm.uf2) |

### SpotPear HDMI

For more info about the SpotPear HDMI see https://spotpear.com/index/product/detail/id/1207.html and https://spotpear.com/index/study/detail/id/971.html.

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [pico1943_SpotpearHDMI_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_SpotpearHDMI_pico2_arm.uf2) |

### Murmulator M1

For more info about the Murmulator see https://murmulator.ru/.

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [pico1943_MurmulatorM1_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_MurmulatorM1_pico2_arm.uf2) |

### Murmulator M2

For more info about the Murmulator see https://murmulator.ru/.

| Board | Binary |
|:--|:--|
| Murmulator M2 | [pico1943_MurmulatorM2_arm.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_MurmulatorM2_arm.uf2) |

### Olimex RP2040-PICO-PC

| Board | Binary |
|:--|:--|
| Olimex RP2040-PICO-PC with a Pico 2 | [pico1943_OlimexPicoPC_arm.uf2](https://github.com/PicoPlus-devel/pico-1943/releases/latest/download/pico1943_OlimexPicoPC_arm.uf2) |
