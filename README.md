# Conker-PS3-Port

Conker's Bad Fur Day for the PlayStation 3 (homebrew, PSL1GHT).

Conker-PS3-Port is a native PS3 port of [Conker's Bad Fur Day: Recompiled](https://github.com/sciaschi/CBFD-Recompiled),
Sean Ciaschi's static recompilation of the Nintendo 64 game (2001). In a static recompilation, the game's own code
is translated into C with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and compiled for another processor:
here, the PS3's Cell. What the game expected from the N64 is rebuilt on the PS3's own hardware:
- graphics: the N64's display lists are interpreted on an SPU and drawn by the RSX, in widescreen and at up to
  four times the N64's resolution;
- sound: the N64's audio microcode runs on an SPU;
- the controller (with rumble), the saves and the timing of the N64's operating system.

It runs on a PS3 with custom firmware (tested on Evilnat 4.92), at a steady 30 frames a second like the
original, with full sound and the multiplayer modes for up to four players. The whole game has been played
through on a real console.

You need your own copy of the game: no game data (graphics, sound, levels or text) is included in this
repository or in the package. Everything the game shows and plays is read from your own ROM.

---

> **Updating from 1.0?** Install 1.1 over it: your saves, settings and copy of the ROM are kept. Installing
> brings back the package's plain icon on the XMB: to get the Conker logo back, start the game and let the
> intro play until the title screen once, then quit. (From 1.1 on the game keeps a copy of the logo, and
> puts it back by itself the next time a newer version is installed and started.)

## What's new in 1.1

- **Smoother picture**: the frames now always reach the screen in the order they were drawn. In 1.0 one
  was sometimes shown before the frame that came before it, a small step back and forth that made movement
  and camera turns stutter, though the game itself ran at full speed.
- **Multiplayer**: up to four PS3 controllers, each with rumble; split screen in widescreen (with the
  circle wipe and fades across the whole screen); and the crash back to the XMB when starting a
  multiplayer game is fixed.
- **Higher-quality sound**: the game's audio is resampled to the PS3's output with a windowed-sinc filter
  (clearer high notes, less aliasing).
- **XMB icon**: kept through updates (see above).

## Download and install

1. Download `ConkersBadFurDay_PS3_1.1.pkg` from the [Releases page](https://github.com/MattyKen619/Conker-PS3-Port/releases).
2. Copy it to the PS3 (a USB drive, or FTP to `/dev_hdd0/packages/`) and install it from the XMB:
   **Package Manager > Install Package Files**. It appears as "Conker's Bad Fur Day" (title ID `CNKR00001`).
3. Put your ROM where the game can find it (see below) and start the game.

## What you need

| File | Where it goes | Notes |
| --- | --- | --- |
| Conker's Bad Fur Day, North America (US), as `.z64`, `.n64` or `.v64` (any file name) | the top folder of a USB drive, for the first start; or `/dev_hdd0/data/conker/` over FTP | The `.z64` has SHA-1 `4cbadd3c4e0729dec46af64ad018050eada4f47a`. The European version and ROM hacks that change the game's code don't work. |

On the first start the game finds the ROM, checks it and copies it to the hard drive (this takes a few
seconds and happens once). After that the USB drive isn't needed.

## Settings

A settings screen comes up each time the game starts:

| Setting | Choices |
| --- | --- |
| Resolution | 432 lines (fastest), **648 lines** (the default), 864 lines (overclock recommended) |
| Cutscenes | Widescreen, or 4:3 with black bars at the sides |
| Picture | Fill the screen, or with the N64's borders |

In the game, **Select** steps through the resolutions. 864 lines is sharper but asks more of the RSX: busy
scenes can drop below 30 frames a second on a PS3 whose RSX runs at the standard speed. The picture is
widescreen on a 16:9 TV and 4:3 on a 4:3 TV.

## The XMB icon

The package comes with PSL1GHT's standard icon. The first time the game's title screen is shown, the game
replaces it with the title screen's own "Conker's Bad Fur Day" logo, taken from your ROM. The new icon appears
once you have quit the game. Every player's icon comes from their own copy of the game, and the repository
holds no artwork.

Installing the package again (an update) brings the stand-in back. The game keeps a copy of the icon it
made with the saves (`/dev_hdd0/data/conker/ICON0.PNG`) and puts it back as soon as it starts. Updating from
1.0, which kept no copy, the icon is made again the first time the title screen is shown.

To use your own pictures instead, put them in `release/` before building: `ICON0.local.png` (a 320 x 176
icon) and `PIC1.local.png` (a 1920 x 1080 background for the XMB). The package uses whichever of these files
exist. They are ignored by git: keep publishers' artwork out of the repository.

## Controls (DualShock 3)

| DualShock 3 | N64 |
| --- | --- |
| Left stick | Control stick |
| Right stick | C buttons |
| Cross | A |
| Circle | B |
| Square | C-right |
| Triangle | C-up |
| L2 or R2 | Z |
| L1 | L |
| R1 | R |
| D-pad | D-pad |
| Start | Start |
| Select | Change the resolution (controller 1) |

Controllers 1 to 4 are the N64's four ports, for the multiplayer modes: turn the extra controllers on
before choosing multiplayer. Rumble works on every controller (the game sees a Rumble Pak). To quit, press the PS button and choose Quit Game. Quitting
doesn't save: the game saves at its own save points, as on the N64.

## Saves

The game's save (the N64 cartridge's EEPROM), the settings, the copy of your ROM and the XMB icon's copy are kept in
`/dev_hdd0/data/conker/`, outside the game's folder: they stay when the game is deleted from the XMB or
updated. To remove everything, delete that folder as well.

## Known issues

- The 1080-line resolution and 60 frames a second (in-between frames) aren't in this version: they work,
  but not well enough yet. They are planned for 2.0.
- Tested on Evilnat 4.92 CFW only. PS3HEN hasn't been tried.
- Only the US ROM works.

## Reporting a problem

[Open an issue](https://github.com/MattyKen619/Conker-PS3-Port/issues) and say where in the game it happened
and what you saw. If the game crashed or stopped with an error, it writes
`/dev_hdd0/data/conker/crash_report.txt`: please attach it (copy it off with FTP or a file manager). The
file only exists after something has gone wrong.

## Planned for 2.0

- **Texture packs**: GLideN64 `.htc` high-resolution texture packs, such as the community's Conker pack
  (each player supplies their own pack).
- **1080 lines, faster**: the full 1080-line picture at a steady 30 frames a second.
- **60 frames a second**: the in-between frames, with their remaining glitches fixed.
- **Stereoscopic 3D** on 3D TVs.
- **Anti-aliasing** (smoothed edges) at 648 and 864 lines.
- **Picture options**: a choice between the N64's own look (dither, its output blur, its texture filtering)
  and a clean, sharp picture.

## Building

Requires the [ps3toolchain](https://github.com/ps3dev/ps3toolchain) and [PSL1GHT](https://github.com/ps3dev/PSL1GHT)
(built and tested with GCC 13.2 for the PPU and 9.5 for the SPU), Git, Python 3, CMake, Ninja and a C++
compiler for your computer (to build N64Recomp), and your US ROM.

```bash
export PS3DEV=/usr/local/ps3dev
export PSL1GHT=$PS3DEV
export PATH=$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin:$PATH

git clone https://github.com/MattyKen619/Conker-PS3-Port.git
cd Conker-PS3-Port
scripts/setup_upstream.sh "/path/to/Conker's Bad Fur Day (U).z64"
make -C game -j8
make -C runtime -j8
make -C app RELEASE=1 -j8 pkg
```

`setup_upstream.sh` fetches CBFD-Recompiled at the commit this port is made from, applies this repository's
PS3 patches to it and to N64Recomp and N64ModernRuntime, and recompiles your ROM's code into C in
`upstream/` (never commit that folder: it is made from the game). The output is `app/conker_bfd.pkg`.

`make -C app pkg` without `RELEASE=1` builds the test version instead ("Conker PS3 (graphics test)", title ID
`CNKR00005`): it keeps a log and its files in `/dev_hdd0/tmp/conkerps3/`, takes screenshots, has the 1080-line
and 60 fps settings (L3 switches the frame rate), and R3 saves the game's memory and the frames being drawn,
which the development tools replay on a computer.

## How it works

- `ps3/host/`: the PS3 side of the program: start-up, the settings screen, the runtime's callbacks (sound,
  controller, video timing), the widescreen and frame-interpolation hooks into the game, the XMB icon, and the
  crash report.
- `ps3/gfx/`: the N64's graphics. `n64gfx` interprets the display lists (the F3DEX2 microcode's commands and
  the RDP's), `rsx_backend` draws them with the RSX, and `combiner_program` turns the N64's colour combiner and
  blender setups into RSX fragment programs as the game uses them. `fpasm/` is PSL1GHT's fragment program
  assembler, run on the console.
- `ps3/spu/gfx/`: the display list interpreter running on an SPU, which sends the RSX a ready-to-draw stream.
- `ps3/spu/audio/`: the N64's audio microcode (recompiled by RSPRecomp) running on an SPU.
- `ps3/compat/`, `ps3/include/`: what the runtime needs from a C/C++ library that PSL1GHT doesn't have.
- `patches/`: the changes to CBFD-Recompiled, N64Recomp and N64ModernRuntime for the PS3 (big-endian memory,
  threads, saves, and the graphics thread's work kept in the order it was sent).
- `game/`, `runtime/`, `app/`: the builds of the recompiled game code, the runtime and the program.

## Credits

- [Conker's Bad Fur Day: Recompiled](https://github.com/sciaschi/CBFD-Recompiled): Sean Ciaschi and its
  contributors (MIT). This port is built on it.
- [Conker decompilation](https://github.com/mkst/conker): mkst and its contributors.
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) and [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime):
  Wiseguy and their contributors (MIT).
- [PSL1GHT](https://github.com/ps3dev/PSL1GHT) and [ps3toolchain](https://github.com/ps3dev/ps3toolchain): the ps3dev
  team. The fragment program assembler in `ps3/gfx/fpasm/` comes from PSL1GHT's cgcomp (MIT, see `ps3/gfx/fpasm/`).
- [miniz](https://github.com/richgel999/miniz): Rich Geldreich (MIT).
- How the N64 behaves was checked against [RT64](https://github.com/rt64/rt64), [angrylion-rdp-plus](https://github.com/ata4/angrylion-rdp-plus)
  and the F3DEX2 disassembly in [F3DEX3](https://github.com/HackerN64/F3DEX3)'s history.
- [Pikmin-PS3-Port](https://github.com/MattyKen619/Pikmin-PS3-Port): this README's layout, and the XMB icon made from
  the player's own game.
- Testing on real hardware: MattyKen619.
- Conker's Bad Fur Day: Rare, published by Nintendo (2001).

This project is not affiliated with or endorsed by Rare, Microsoft or Nintendo. Conker's Bad Fur Day and Conker
are trademarks of their respective owners.

## License

MIT (see `LICENSE`), like CBFD-Recompiled and N64Recomp, except where a file says otherwise. No game data is
included or distributed.

## AI disclosure

This port's code was written with Claude (Anthropic), working with MattyKen619, who tested every build on a PS3
and guided the work. The game code itself comes from your ROM, through CBFD-Recompiled.
