# AGENTS.md — gambatte_3ds

Onboarding and debugging guide for agents working on **gambatte_3ds**, a Nintendo 3DS port of `libretro/gambatte-libretro` (GB/GBC) inside the modernized 3DS UI/GPU/NDSP shell from `matbo87/snes9x_3ds` (RetroAchievements and Online Updater intentionally omitted).

> **Workspace boundary:** Do **not** modify sibling `../gpSP_3ds/` unless explicitly asked.

---

## 1. Target & Repository Map

- **Target hardware:** Nintendo 3DS family (`APP_SYSTEM_MODE="64MB"`, `APP_SYSTEM_MODE_EXT="Legacy"` in `Makefile`; `resources/AppInfo`: `CTR-P-GB3D`, `0x3851`; targeted primarily at New 3DS / New 3DS XL / New 2DS XL).
- **3DS shell (`source/3ds*.cpp/.h`, `source/png_utils.*`, `source/shader_screen.v.pica`):** UI/menu, PICA200 renderer, NDSP audio mixer thread (`32,768 Hz`), async SD save worker, LCD VTotal pacing, config/settings, cheats, and thumbnails.
- **Core integration (`source/3dsimpl.h`, `source/3dsimpl.cpp`):** Owns `GAME_MAIN` (`256x256` `GPU_RGB565` VRAM texture with `160x144` active region), `g_fbLinear` (`linearAlloc`), `blipper` 64x stereo audio decimation (`~2,097,152 Hz` -> `32,768 Hz`), DMG palettes, GBC color correction, interframe blending, `160x144` RGB8 screenshots, `80x72` swizzled RGB565 thumbnails, battery `.sav` + RTC, and RetroArch `.cht` Game Genie / GameShark support.
- **Vendored core (`source/gambatte/`):** `libretro/gambatte-libretro` core (`libgambatte/src/{,mem,sound,video}/*.cpp`, `libretro/blipper.c`, `libretro/gambatte_log.c`). Local fix in `source/gambatte/libgambatte/include/gambatte.h:27` adds `#include <cstdint>` for GCC 13+ hosts.
- **Custom citro3d (`patches/citro3d.patch` -> `libs/citro3d`):** Clones/patches `devkitPro/citro3d` `v1.7.1` to add `C3Di_RenderQueueIsDone()` and the vertex uniform dirty-bound optimization. Always update `patches/citro3d.patch`, never `libs/citro3d` directly (`Makefile:225-237` resets `libs/citro3d` when the patch changes).
- **Custom branding assets (`resources/`, `gfx/`, `romfs/gfx/`):** Custom GBC `48x48` icon (`resources/icon.png`), 2D HOME hover banner (`resources/banner.png` -> `resources/banner.bnr` via `bannertool` with silent 22050 Hz PCM WAV), 3-column parallax splash (`gfx/splash_{logo,center,left,right}.png` -> `romfs/gfx/splash.t3x`), GBC bezel (`romfs/gfx/overlay.png`), blurred GB/GBC box-art game background (`romfs/gfx/background_game_screen.png`), and GBC touch-screen background (`romfs/gfx/background_second_screen_{us,pal}.png`).
- **Host tests (`tests/test_core.cpp`):** Native Linux tests covering Gambatte GB/GBC execution on a synthetic ROM + APU audio via `blipper`, save-state round-trip, Game Genie / GameShark application, `StereoRingBuffer`, and `.cht` parsing/classification/serialization.

---

## 2. Build, Crash Symbolization & Host Tests

### 3DS Release Build (Docker `devkitpro/devkitarm`)

`Makefile` produces `output/gambatte_3ds.elf`, `output/gambatte_3ds.smdh`, `output/gambatte_3ds.3dsx`, and `output/gambatte_3ds.cia`:

```sh
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/src" -w /src devkitpro/devkitarm \
  bash -c "git config --global --add safe.directory '*' && make release"
```

When changing `resources/icon.png`, `resources/banner.bnr`, `gfx/splash_*.png`, or `romfs/gfx/*.png`, delete `romfs/gfx/splash.t3x` and `output/gambatte_3ds.{3dsx,cia,smdh}` before `make release` so `tex3ds`, `smdhtool`, `3dsxtool`, and `makerom` repackage the updated assets (`Makefile` only lists `.elf`/`.smdh` as prerequisites of `.3dsx`/`.cia`).

### Symbolize Luma3DS Crash Addresses

Note: `devkitpro/devkitarm` does not put ARM binutils on default `PATH` in non-login shells; invoke `/opt/devkitpro/devkitARM/bin/arm-none-eabi-*` directly:

```sh
docker run --rm -v "$PWD:/src" -w /src devkitpro/devkitarm \
  /opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line -f -C -e output/gambatte_3ds.elf <addr>
```

### Host Self-Tests (Linux)

Compiles into `/tmp/gambatte_host_obj` (using bash arrays so spaces in `$PWD` work safely), runs all 3 suites, and cleans up `/tmp/gambatte_host_obj`:

```sh
rm -rf /tmp/gambatte_host_obj && mkdir -p /tmp/gambatte_host_obj && (
  cd /tmp/gambatte_host_obj
  S="$OLDPWD/source"
  T="$OLDPWD/tests"
  G="$S/gambatte"
  INC=(-I"$S" -I"$G/common" -I"$G/libgambatte/include" -I"$G/libgambatte/src" -I"$G/libgambatte/libretro" -I"$G/libgambatte/libretro-common/include")
  DEFS="-DVIDEO_RGB565 -D__LIBRETRO__"
  i=0
  for f in \
    "$G"/libgambatte/src/*.cpp \
    "$G"/libgambatte/src/mem/*.cpp \
    "$G"/libgambatte/src/sound/*.cpp \
    "$G"/libgambatte/src/video/*.cpp; do
    i=$((i + 1))
    g++ -O2 -std=gnu++17 -w "${INC[@]}" $DEFS -c "$f" -o "g_$i.o"
  done
  gcc -O2 -std=gnu99 -w "${INC[@]}" $DEFS -c "$G/libgambatte/libretro/blipper.c" -o blipper.o
  gcc -O2 -std=gnu99 -w "${INC[@]}" $DEFS -c "$G/libgambatte/libretro/gambatte_log.c" -o gambatte_log.o
  g++ -O2 -std=gnu++17 -Wall -Wextra -Werror "${INC[@]}" $DEFS "$T/test_core.cpp" *.o -o test_core
  ./test_core
) && rm -rf /tmp/gambatte_host_obj
```

---

## 3. Hard Architecture Invariants & Pitfalls

1. **Core build defines & rumble callback:** Always compile core/frontend callers with `-DVIDEO_RGB565 -D__LIBRETRO__` (selects 16-bit `video_pixel_t` in `gambatte.h:32-33` and `Cartridge::loadSavedata`), and define `void cartridge_set_rumble(unsigned active)` in the frontend (`source/3dsimpl.cpp:45-47`).
2. **Compiler warnings (`Makefile:77-96`):** `STRICT_WARNINGS=1` (`-Werror`) applies to `source/3ds*.cpp`; `source/gambatte/` compiles with `-w`. Keep `-Wno-register` out of strict flags that break C builds if C files are added.
3. **PICA200 frame & texture rules:** `C3D_SyncDisplayTransfer` must run **outside** `C3D_FrameBegin..C3D_FrameEnd` after `gpu3dsWaitForRenderQueue()` (`source/3dsimpl.cpp:708-712`). Because `DISPLAY_TRANSFER_FLAGS` uses `GX_TRANSFER_FLIP_VERT(0)`, `GAME_MAIN` (`256x256`) uses inverted V coordinates (`ty0 = 256`, `ty1 = 112` in `source/3dsimpl.cpp:1130-1131, 1182-1183`).
4. **Exact NDSP audio rate match (`source/3dsimpl.cpp:379-380`, `source/3dssound.h:10-12`):** `blipper_new(32, 0.85, 6.5, 64, 1536, NULL)` decimates Gambatte's native `~2,097,152 Hz` sample rate by `64` to exact `32,768 Hz` matching `SND3DS_SAMPLE_RATE`.
5. **Custom citro3d checkout:** Never edit `libs/citro3d` directly; store changes in `patches/citro3d.patch`.
6. **Built-in bezel geometry & touch BG safe zone (`source/3dsui_img.cpp:27-30`, `source/3dsimpl.cpp`):** `BEZEL_INNER_WIDTH = 222`, `BEZEL_INNER_HEIGHT = 200` (centered transparent cutout `x=89..310, y=20..219` in `400x240` `romfs/gfx/overlay.png`). `img3dsGetBuiltInBezelBounds()` fits scaled game viewports inside `222x200` only when the built-in `romfs:/` bezel is active, `GameScreen == GFX_TOP`, `ScreenStretch != Full`, and `GameOverlayAutoFit == false`, leaving custom SD overlays untouched. `background_second_screen_{us,pal}.png` (`400x240`) renders centered on the `320x240` bottom screen (`sx0 = -40`), so main visual subjects must stay within `x = 40..360`.

---

## 4. Real-Hardware Status & Branding Notes

- **Verified working on real 3DS hardware:** GB and GBC ROM loading, rendering, audio, and gameplay work on hardware after the GPU/VRAM fixes (`0c0a7af`). Custom icon/bezel/touch BG (`6c86a56`) and HOME banner/splash/in-game BG (`c939961`) were built cleanly with `make release` (new HOME banner & splash await visual check on real 3DS).

### Fixed: Startup GPU NULL Dereference & Unsynchronized VRAM Clear

1. **Unbound shader at startup (`source/3dsgpu.cpp:418-455`, `source/3dsgpu.h:59-62`, `libs/citro3d/source/base.c:307`):** Because `SPROGRAM_SCREEN == 0` (along with `SGPU_STATE_DISABLED`, `ALPHA_TEST_DISABLED`, `ALPHA_BLENDING_DISABLED`), `gpu3dsInitialize()` zeroing `GPU3DS` without calling `gpu3dsResetState()` caused `gpu3dsApplyRenderState()` on the first splash draw in `showMenu()` to skip `C3D_BindProgram()` (leaving `ctx->program == NULL`) and keep `DepthTest` enabled, crashing in `C3Di_UpdateContext()` (`ctx->program->geometryShader`).
   - **Applied fix:** `gpu3dsResetState();` is now called at the end of `gpu3dsInitialize()` (`source/3dsgpu.cpp:452`).
2. **Unsynchronized `gpu3dsClearTexture()` (`source/3dsimpl.cpp:236-239, 517`, `source/3dsgpu.cpp:670-679`):** `clearFramebuffers()` (called during `impl3dsLoadROM()` right after `menu3dsRunRomLoadingDialog()` frames) now calls `gpu3dsWaitForRenderQueue()` before `gpu3dsClearTexture(&GPU3DS.textures[GAME_MAIN], 0)` (`C3D_SyncMemoryFill`).

### Dead-Code Note

- `link.ld` and `-T $(LDSCRIPT)` in `Makefile:91-92, 353` are unused SuperFX/GSU remnants from `snes9x_3ds` (not needed by Gambatte).

### Logging & Secondary Post-ROM-Launch Checks

If issues appear on hardware after passing the menu:

1. **Unbuffered SD logs (`source/3dslog.cpp:12,16`):** `LOG_BUFFER_IN_GAME` defaults to `true`, keeping in-game logs in a 32 KiB RAM buffer until `emulatorLoop()` exits. Set `LOG_BUFFER_IN_GAME = false` (and optionally `FORCE_DEBUG_LOGS = true`) when chasing in-game crashes/hangs. Log path: `sdmc:/3ds/gambatte_3ds/debug_v1.0_session.log`.
2. **Empty frame-2 GPU submission (`source/3dsimpl.cpp:691-728`, `source/3dsmain.cpp:2310-2341`):** `emulatorLoop()` starts with `firstFrame = true; skipDrawing = true;`. Frame 1 draws `impl3dsSceneRender` and emulates with `skipDrawingFrame = true` (`g_pendingDraw = false`). Frame 2 calls `gpu3dsFrameBegin(0, false)` followed by zero `C3D_FrameDrawOn` / draw calls and `gpu3dsFrameEnd()` (also ignoring `gpu3dsFrameBegin`'s `bool` return value).
3. **LCD VTotal pre-render guard (`source/3dsmain.cpp:2271-2305`, `source/3dslcd.cpp:48-62`):** Because the top screen starts in 2D mode, `gpu3dsSetTopMode()` (`source/3dsgpu.cpp:386-400`) returns `false`, skipping the `gpu3dsFrameBegin(C3D_FRAME_SYNCDRAW, false)` pre-render right before `lcd3dsSetEmulationRate(TICKS_PER_FRAME_GB)`. Also verify `paceFrameOnVBlankCounter` vs `C3D_FrameCounter` and `snd3dsMixSamples()` (`svcFlushProcessDataCache` on `device3DS.secondaryCore`).
