# Conker-PS3-Port

Conker's Bad Fur Day for the PlayStation 3 (homebrew, PSL1GHT).

Conker-PS3-Port is a native PS3 port of [Conker's Bad Fur Day: Recompiled](https://github.com/sciaschi/CBFD-Recompiled),
Sean Ciaschi's static recompilation of the Nintendo 64 game (2001). In a static recompilation, the game's own code
is translated into C with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and compiled for another processor:
here, the PS3's Cell. What the game expected from the N64 is rebuilt on the PS3's own hardware:
- graphics: the N64's display lists are interpreted on an SPU and drawn by the RSX, in widescreen and at up to
  five times the N64's resolution (1080 lines), at 30 or 60 frames a second, with high-resolution texture packs
  and stereoscopic 3D on 3D TVs;
- sound: the N64's audio microcode runs on an SPU;
- the controller (with rumble), the saves and the timing of the N64's operating system.

It runs on a PS3 with custom firmware (tested on Evilnat 4.92), at a steady 30 frames a second like the
original (or 60 with in-between frames), with full sound and the multiplayer modes for up to four players. The
whole game has been played through on a real console.

You need your own copy of the game: no game data (graphics, sound, levels or text) is included in this
repository or in the package. Everything the game shows and plays is read from your own ROM.

---

> **Updating?** Install 2.0 over 1.x: your saves, settings and copy of the ROM are kept. Installing
> brings back the package's plain icon on the XMB. From 1.1 or 1.2, the game puts the Conker logo back by
> itself: start it once (the settings screen is enough), then quit. From 1.0, start the game and let the intro
> play until the title screen once, then quit.

## What's new in 2.0

- **60 frames a second**: in-between frames, worked out from the frames before and after (the game itself
  still runs at its own speed).
- **1080 lines**, and speed-ups (the slowdown near the top of Poo Mountain is gone).
- **Texture packs**: GLideN64 `.htc` high-resolution texture packs, such as the community's Conker pack (each
  player supplies their own; see below).
- **Stereoscopic 3D** on 3D TVs (HDMI 3D, 720p frame packing), with settings for the depth, what sits at the
  screen's depth, and ghost cancellation (for TVs that let a faint copy of the other eye's picture through).
- **Anti-aliasing** (smoothed edges), and a choice of **look**: clean and sharp, or like the N64 (its texture
  filter, dither and soft video output).
- **Free camera**: the right stick can turn the camera freely, as in the PC version, for each player in split
  screen too.
- **B button on Circle or Square** (Circle is the default).
- **Quit with Select + Start** (held for a second).
- **A new settings screen**, on three tabs: Main, Extras and 3D settings.
- Fixed: the tops of the catfish's heads (without a texture pack) had purple specks.

## What's new in 1.2

- **Fixed: the crash in It's War**: the game stopped (back to the XMB, "abort() called" in the crash
  report) during the battle, around when the General knocks Conker out, and again on loading the save
  there. The port recognised some of the game's textures as new every frame (it compared parts of the
  N64's texture memory the picture doesn't use), decoded them again and again, and main memory ran out.
  Textures are now told apart by what they show, and the decoded ones are kept within a fixed amount of
  memory.
- **More useful crash reports**: `crash_report.txt` now names the error and says how much memory was free.

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

1. Download `ConkersBadFurDay_PS3_2.0.pkg` from the [Releases page](https://github.com/MattyKen619/Conker-PS3-Port/releases).
2. Copy it to the PS3 (a USB drive, or FTP to `/dev_hdd0/packages/`) and install it from the XMB:
   **Package Manager > Install Package Files**. It appears as "Conker's Bad Fur Day" (title ID `CNKR00001`).
3. Put your ROM where the game can find it (see below) and start the game.

## What you need

| File | Where it goes | Notes |
| --- | --- | --- |
| Conker's Bad Fur Day, North America (US), as `.z64`, `.n64` or `.v64` (any file name) | the top folder of a USB drive, for the first start; or `/dev_hdd0/data/conker/` over FTP | The `.z64` has SHA-1 `4cbadd3c4e0729dec46af64ad018050eada4f47a`. The European version and ROM hacks that change the game's code don't work. |

On the first start the game finds the ROM, checks it and copies it to the hard drive (this takes a few
seconds and happens once). After that the USB drive isn't needed.

### A texture pack (optional)

Put a GLideN64 texture pack for the US version (a `.htc` file) in `/dev_hdd0/data/conker/` over FTP. The first
time the game starts with it, it unpacks the pack once into `texture_pack.pack` in the same folder (this takes a
while); after that it starts as usual. The **Textures** setting switches between the pack and the game's own
textures. No texture pack is included: each player brings their own.  The one that has been tested is linked here https://www.n64textures.com/welcome/

## Settings

A settings screen comes up each time the game starts. **L1 / R1** change the tab, **Up / Down** choose a
setting, **Left / Right** change it, and **Start** (or Cross on "Start the game") plays.

**Main**

| Setting | Choices |
| --- | --- |
| Resolution | 432 lines (fastest), **648 lines** (the default), 864 lines, 1080 lines (overclock recommended) |
| Frame rate | **30 fps** (as on the N64), or 60 fps (in-between frames) |
| Textures | The game's own, or a texture pack (if one is there) |
| Anti-aliasing | **Off**, or on (4x at 432 and 648 lines, 2x at 864, not at 1080) |

**Extras**

| Setting | Choices |
| --- | --- |
| Cutscenes | **Widescreen**, or 4:3 with black bars at the sides |
| Picture | **Fill the screen**, or with the N64's borders |
| Look | **Clean** (sharper at angles), or like the N64 (its texture filter, dither and soft picture) |
| B button | **Circle** (Square is C-right), or Square (Circle is C-right) |
| Right stick | **C buttons** (as on the N64), or free camera |

**3D settings** (for 3D TVs)

| Setting | Choices |
| --- | --- |
| 3D | **Off**, or on |
| Depth | Lowest, Low, **Medium**, High, Highest |
| Screen depth | What sits at the screen's surface: Nearer (more comes out of the screen), **Normal**, Farther, Farther still |
| Ghost cancellation | **Off**, or 1 to 6: takes out the faint copy of the other eye's picture some 3D TVs let through (the blacks turn a little grey) |

The higher resolutions, 60 fps and anti-aliasing ask more of the RSX: busy scenes can drop below full speed
on a PS3 whose RSX runs at the standard speed. In 3D the game sends 720p frame packing (a full picture for
each eye) and draws at 648 lines, 30 frames a second, without anti-aliasing; the TV switches to 3D by itself.
The picture is widescreen on a 16:9 TV and 4:3 on a 4:3 TV.

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
| Right stick | C buttons, or the free camera (a setting) |
| Cross | A |
| Circle | B (or C-right, with the B button on Square) |
| Square | C-right (or B, with the B button on Square) |
| Triangle | C-up |
| L2 or R2 | Z |
| L1 | L |
| R1 | R |
| D-pad | D-pad |
| Start | Start |
| Select + Start (held for a second) | Quit the game (controller 1) |

Controllers 1 to 4 are the N64's four ports, for the multiplayer modes: turn the extra controllers on
before choosing multiplayer. Rumble works on every controller (the game sees a Rumble Pak). With the free
camera, each player's right stick turns their own camera. To quit, hold Select + Start, or press the PS button
and choose Quit Game. Quitting doesn't save: the game saves at its own save points, as on the N64.

## Saves

The game's save (the N64 cartridge's EEPROM), the settings, the copy of your ROM, the unpacked texture pack and the XMB icon's copy are kept in
`/dev_hdd0/data/conker/`, outside the game's folder: they stay when the game is deleted from the XMB or
updated. To remove everything, delete that folder as well.

## Known issues

- Tested on Evilnat 4.92 CFW only. PS3HEN hasn't been tried.
- Only the US ROM works.
- 3D: some 3D TVs let a faint copy of the other eye's picture through (ghosting), most on bright things against
  dark backgrounds. Lower depth, another screen depth or ghost cancellation make it less visible.

## Reporting a problem

[Open an issue](https://github.com/MattyKen619/Conker-PS3-Port/issues) and say where in the game it happened
and what you saw. If the game crashed or stopped with an error, it writes
`/dev_hdd0/data/conker/crash_report.txt`: please attach it (copy it off with FTP or a file manager). The
file only exists after something has gone wrong.

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
`CNKR00005`): it keeps a log and its files in `/dev_hdd0/tmp/conkerps3/`, takes screenshots, L3 switches the
frame rate (not in 3D), and R3 saves the game's memory and the frames being drawn (both eyes in 3D), which the
development tools replay on a computer.

## How it works

- `ps3/host/`: the PS3 side of the program: start-up, the settings screen, the runtime's callbacks (sound,
  controller, video timing), the widescreen and frame-interpolation hooks into the game, the free camera
  (`free_camera.cpp`, from CBFD-Recompiled's), the texture pack's unpacking, 3D's video mode, the XMB icon, and
  the crash report.
- `ps3/gfx/`: the N64's graphics. `n64gfx` interprets the display lists (the F3DEX2 microcode's commands and
  the RDP's), `rsx_backend` draws them with the RSX, and `combiner_program` turns the N64's colour combiner and
  blender setups into RSX fragment programs as the game uses them. `fpasm/` is PSL1GHT's fragment program
  assembler, run on the console. In 3D each frame image holds both eyes' pictures and every draw goes to both,
  seen from each eye's side. `texture_pack` reads GLideN64 texture packs, found by the textures' Rice checksums
  (`rice_hash`).
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
- Texture packs' checksums (`ps3/gfx/rice_hash.cpp`, GPL version 2 or later): as in CBFD-Recompiled's RT64
  patch and RT64's texture hasher, from [GLideN64](https://github.com/gonetz/GLideN64)'s GlideHQ (Hiroshi Morii)
  and Rice Video (Rice1964).
- The free camera: CBFD-Recompiled's mouse and right-stick camera (MIT), without the mouse.
- How the N64 behaves was checked against [RT64](https://github.com/rt64/rt64), [angrylion-rdp-plus](https://github.com/ata4/angrylion-rdp-plus)
  and the F3DEX2 disassembly in [F3DEX3](https://github.com/HackerN64/F3DEX3)'s history.
- [Pikmin-PS3-Port](https://github.com/MattyKen619/Pikmin-PS3-Port): this README's layout, and the XMB icon made from
  the player's own game.
- Testing on real hardware: MattyKen619.
- Conker's Bad Fur Day: Rare, published by Nintendo (2001).

This project is not affiliated with or endorsed by Rare, Microsoft or Nintendo. Conker's Bad Fur Day and Conker
are trademarks of their respective owners.

## License

MIT (see `LICENSE`), like CBFD-Recompiled and N64Recomp, except where a file says otherwise:
`ps3/gfx/rice_hash.cpp` and `rice_hash.h` (the texture packs' checksums) are under the GNU General Public
License, version 2 or later (see `LICENSE.GPL-2.0`). The package is built with them, so the program as a whole
is distributed under the GPL, version 2 or later; its complete source is this repository. No game data is
included or distributed.

## AI disclosure

This port's code was written with Claude (Anthropic), working with MattyKen619, who tested every build on a PS3
and guided the work. The game code itself comes from your ROM, through CBFD-Recompiled.
