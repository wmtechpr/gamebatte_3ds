# Gambatte for 3DS

A Nintendo 3DS port of the **Gambatte** Game Boy / Game Boy Color emulator (`libretro/gambatte-libretro`) built on the modernized 3DS UI and GPU/NDSP shell from [matbo87/snes9x_3ds](https://github.com/matbo87/snes9x_3ds) (itself derived from [bubble2k16/snes9x_3ds](https://github.com/bubble2k16/snes9x_3ds)).

Targeted primarily at **New 3DS / New 3DS XL / New 2DS XL** consoles.

## Features

- Accurate Game Boy & Game Boy Color emulation via bundled Gambatte (`VIDEO_RGB565`)
- 64x Kaiser-windowed sinc audio resampling via `blipper` into NDSP stereo output at `32,768 Hz` with a thread-safe stereo ring buffer (`Low` / `Normal` / `High` buffer depth & `100%`–`200%` volume boost)
- DMG palettes (`DMG Green`, `GB Pocket`, `Grayscale`), GBC LCD color correction (`Off`, `Accurate`, `Fast`), hardware mode override (`Auto`, `Force DMG`, `Force CGB`), and optional LCD interframe blending
- Hardware-accelerated PICA200 rendering with selectable scaling (`None 160x144`, `Fit 10:9`, `4:3`, `Full`), filtering (`Sharp`, `Smooth`, `Balanced`), scanlines, cropping, overscan, Top/Bottom screen swap, custom bezels/overlays, and custom game/second-screen backgrounds
- UI themes (`DarkMode`, `RetroArch`, `Original`), fonts (`Tempesta`, `Ronda`), ROM boxart/title/gameplay thumbnails, and directory listing cache
- 5 save state slots (`.1.frz`–`.5.frz`) with embedded `80x72` thumbnails, optional auto-savestate, async SD save worker, and periodic dirty-SRAM + RTC `.sav` saving
- In-game screenshots (PNG) and RetroArch `.cht` cheat support (Game Genie & GameShark codes)
- Per-game and global button remapping, turbo fire, Circle Pad D-pad binding, and hotkey combos

## Supported ROMs & SD Card Layout

Supported extensions: `.gb`, `.gbc`, `.dmg` (uncompressed; ZIP archives are not supported). ROMs can live anywhere on your SD card.

### Installation
- **`.cia`**: Install `output/gambatte_3ds.cia` with FBI.
- **`.3dsx`**: Copy `output/gambatte_3ds.3dsx` to `sdmc:/3ds/gambatte_3ds/` and launch via Homebrew Launcher.
- Requires dumped 3DS DSP firmware (`sdmc:/3ds/dspfirm.cdc`) for NDSP audio output.

Emulator data lives under `sdmc:/3ds/gambatte_3ds/`:
- `saves/` — battery `.sav` (+ RTC) files
- `savestates/` — save state slots (`.1.frz`–`.5.frz`) and `.auto.frz`
- `screenshots/` — timestamped `.png` screenshots
- `cheats/` — RetroArch `.cht` cheat files matched by trimmed ROM name
- `overlays/` — custom bezel `.png` files
- `backgrounds/game_screen/` & `backgrounds/second_screen/` — custom background `.png` files

## Building from Source

The Makefile clones `citro3d` `v1.7.1`, applies `patches/citro3d.patch` into `libs/citro3d`, builds RomFS/shader assets, and produces `.elf`, `.smdh`, `.3dsx`, and `.cia` under `output/`.

Using Docker (`devkitpro/devkitarm`):

```sh
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/src" -w /src devkitpro/devkitarm \
  bash -c "git config --global --add safe.directory '*' && make release"
```

Outputs:
- `output/gambatte_3ds.3dsx`
- `output/gambatte_3ds.cia`
- `output/gambatte_3ds.smdh`

### Host Self-Tests

Core execution, decimated audio output, save-state serialization, cheat parsing/classification, and the `StereoRingBuffer` can be tested natively on Linux without 3DS hardware (`tests/test_core.cpp`).

## Credits

- **Sindre Aamås & libretro contributors** for [Gambatte](https://github.com/libretro/gambatte-libretro) (GPLv2) and `blipper`
- **matbo87** for [matbo87/snes9x_3ds](https://github.com/matbo87/snes9x_3ds)
- **bubble2k16** for the original [bubble2k16/snes9x_3ds](https://github.com/bubble2k16/snes9x_3ds) codebase
- **devkitPro** (`devkitARM`, `libctru`, `citro3d`, `tex3ds`)
