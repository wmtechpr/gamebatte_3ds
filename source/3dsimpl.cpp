//=============================================================================
// Interface between the 3DS shell and the Gambatte (GB/GBC) core.
//=============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <vector>
#include <zlib.h>

#include "gambatte.h"
#include "blipper.h"

#include "3dscheat_parse.h"
#include "3dsfiles.h"
#include "3dsgpu.h"
#include "3dsimpl.h"
#include "3dsimpl_gpu.h"
#include "3dsinput.h"
#include "3dslog.h"
#include "3dsmenu.h"
#include "3dssave.h"
#include "3dssettings.h"
#include "3dssound.h"
#include "3dstimer.h"
#include "3dsui.h"
#include "3dsui_img.h"
#include "3dsui_notif.h"
#include "3dsutils.h"

#include "shader_screen_shbin.h"

//---------------------------------------------------------
// Globals declared in 3dsimpl.h
//---------------------------------------------------------
CoreScreenshot screenshot = {};
bool skipNextFpsUpdate = false;
CoreRomInfo coreRomInfo = {};
CoreCheatData coreCheats = {};

void cartridge_set_rumble(unsigned active) {
    (void)active;
}

//---------------------------------------------------------
// Constants & core state
//---------------------------------------------------------
static const int GAME_TEX_DIM = 256;
static const size_t GAME_FB_LINEAR_BYTES = GAME_TEX_DIM * GAME_TEX_DIM * sizeof(u16);
static const size_t GAME_FRAME_PIXELS = CORE_SCREEN_WIDTH * CORE_SCREEN_HEIGHT;

// Native ~2 MiHz samples per frame (~35112) + headroom for scanline phase & LCD jitter.
static const unsigned SAMPLES_PER_STEP = 2064;
static const unsigned MAX_FRAME_SAMPLES = 37152;
static const size_t SOUND_BUF_CAPACITY = 8192;
static const int BLIPPER_OUT_CHUNK = 1024;

// Periodic background SRAM flush interval (~2 seconds at 60 FPS).
static const int SRAM_DIRTY_CHECK_FRAMES = 120;
static const int SD_WRITE_FAIL_NOTIF_LIMIT = 3;

static const u64 PAUSE_ANIMATION_DURATION_MS = 180;
static const float PAUSE_ANIMATION_Y_OFFSET = 4.0f;

static gambatte::GB* g_gb = nullptr;
static blipper_t* g_blipL = nullptr;
static blipper_t* g_blipR = nullptr;

static u16* g_fbLinear = nullptr;
static std::vector<u16> g_fbStageA;
static std::vector<u16> g_fbStageB;
static u16* g_fbCurr = nullptr;
static u16* g_fbPrev = nullptr;
static bool g_hasPrevFrame = false;

static std::vector<gambatte::uint_least32_t> g_soundBuf;
static std::vector<int16_t> g_audioOutBuf;

static std::vector<u8> g_sramShadow;
static int g_sramCheckCounter = 0;

static u32 g_currentCoreInput = 0;
static u32 g_turboFrameCounter = 0;

static bool g_pendingDraw = false;
static bool g_hasValidFrame = false;
static u64 pauseAnimationStartedAt = 0;   // 0 = not started; osGetTime() is never 0

//---------------------------------------------------------
// Gambatte input callback
//---------------------------------------------------------
class GambatteInputGetter : public gambatte::InputGetter {
public:
    unsigned operator()() override {
        return g_currentCoreInput;
    }
};

static GambatteInputGetter g_inputGetter;

typedef struct {
    int sx0;
    int sy0;
    int sx1;
    int sy1;
    int sWidth;
    int sHeight;
    int cHeight;
    float tx0;
    float ty0;
    float tx1;
    float ty1;
} GameScreenViewport;

typedef Result (*GSP_CacheCallback)(const void* addr, u32 size);

//---------------------------------------------------------
// Helpers
//---------------------------------------------------------
static inline u16 rgb888To565(u32 rgb) {
    u32 r = (rgb >> 16) & 0xFF;
    u32 g = (rgb >> 8) & 0xFF;
    u32 b = rgb & 0xFF;
    return static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline u16 blendRgb565(u16 a, u16 b) {
    return static_cast<u16>(((a & 0xF7DEu) >> 1) + ((b & 0xF7DEu) >> 1) + (a & b & 0x0821u));
}

static const char* gbCartTypeName(u8 type, bool* outHasRtc) {
    if (outHasRtc) *outHasRtc = false;
    switch (type) {
        case 0x00: return "ROM ONLY";
        case 0x01: return "MBC1";
        case 0x02: return "MBC1+RAM";
        case 0x03: return "MBC1+RAM+BATTERY";
        case 0x05: return "MBC2";
        case 0x06: return "MBC2+BATTERY";
        case 0x08: return "ROM+RAM";
        case 0x09: return "ROM+RAM+BATTERY";
        case 0x0B: return "MMM01";
        case 0x0C: return "MMM01+RAM";
        case 0x0D: return "MMM01+RAM+BATTERY";
        case 0x0F:
            if (outHasRtc) *outHasRtc = true;
            return "MBC3+TIMER+BATTERY";
        case 0x10:
            if (outHasRtc) *outHasRtc = true;
            return "MBC3+TIMER+RAM+BATTERY";
        case 0x11: return "MBC3";
        case 0x12: return "MBC3+RAM";
        case 0x13: return "MBC3+RAM+BATTERY";
        case 0x19: return "MBC5";
        case 0x1A: return "MBC5+RAM";
        case 0x1B: return "MBC5+RAM+BATTERY";
        case 0x1C: return "MBC5+RUMBLE";
        case 0x1D: return "MBC5+RUMBLE+RAM";
        case 0x1E: return "MBC5+RUMBLE+RAM+BATTERY";
        case 0x20: return "MBC6";
        case 0x22: return "MBC7+SENSOR+RUMBLE+RAM";
        case 0xFC: return "POCKET CAMERA";
        case 0xFD: return "BANDAI TAMA5";
        case 0xFE:
            if (outHasRtc) *outHasRtc = true;
            return "HuC3";
        case 0xFF: return "HuC1+RAM+BATTERY";
        default:   return nullptr;
    }
}

static void parseGbHeaderInfo(const u8* romData, size_t romSize, const char* romFilePath) {
    char title[17] = {};
    if (romSize >= 0x150) {
        size_t titleLen = (romData[0x143] == 0x80 || romData[0x143] == 0xC0) ? 15 : 16;
        for (size_t i = 0; i < titleLen; i++) {
            u8 c = romData[0x134 + i];
            if (c == 0) break;
            title[i] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : ' ';
        }
        for (int i = static_cast<int>(strlen(title)) - 1; i >= 0; i--) {
            if (title[i] == ' ') title[i] = '\0';
            else break;
        }
    }

    if (title[0] != '\0') {
        snprintf(coreRomInfo.romName, sizeof(coreRomInfo.romName), "%s", title);
    } else {
        utils3dsGetBasename(romFilePath, coreRomInfo.romName, sizeof(coreRomInfo.romName), false);
    }

    bool hasRtc = false;
    const char* mapper = nullptr;
    if (romSize >= 0x150) {
        u8 cartType = romData[0x147];
        mapper = gbCartTypeName(cartType, &hasRtc);
        if (!mapper) {
            snprintf(coreRomInfo.mapperName, sizeof(coreRomInfo.mapperName), "MAPPER 0x%02X", cartType);
        } else {
            snprintf(coreRomInfo.mapperName, sizeof(coreRomInfo.mapperName), "%s", mapper);
        }
    } else {
        snprintf(coreRomInfo.mapperName, sizeof(coreRomInfo.mapperName), "UNKNOWN");
    }

    coreRomInfo.hasRtc = hasRtc || (g_gb && g_gb->rtcdata_size() > 0);
}

static void resetAudioPipeline() {
    if (g_blipL) blipper_reset(g_blipL);
    if (g_blipR) blipper_reset(g_blipR);
    snd3dsClearRingBuffer();
}

static void clearFramebuffers() {
    if (g_fbLinear) {
        memset(g_fbLinear, 0, GAME_FB_LINEAR_BYTES);
    }
    if (!g_fbStageA.empty()) {
        std::fill(g_fbStageA.begin(), g_fbStageA.end(), 0);
    }
    if (!g_fbStageB.empty()) {
        std::fill(g_fbStageB.begin(), g_fbStageB.end(), 0);
    }
    g_fbCurr = g_fbStageA.empty() ? nullptr : g_fbStageA.data();
    g_fbPrev = g_fbStageB.empty() ? nullptr : g_fbStageB.data();
    g_hasPrevFrame = false;
    g_hasValidFrame = false;
    g_pendingDraw = false;

    if (GPU3DS.textures[GAME_MAIN].tex.data) {
        gpu3dsWaitForRenderQueue();
        gpu3dsClearTexture(&GPU3DS.textures[GAME_MAIN], 0);
    }
}

static void updateCoreInputFrom3DS() {
    u32 held = input3dsGetCurrentKeysHeld();

    bool bindCirclePad = settings3DS.UseGlobalBindCirclePad
        ? settings3DS.GlobalBindCirclePad
        : settings3DS.BindCirclePad;
    const auto& mapping = settings3DS.UseGlobalButtonMappings
        ? settings3DS.GlobalButtonMapping
        : settings3DS.ButtonMapping;
    const auto& turbo = settings3DS.UseGlobalTurbo
        ? settings3DS.GlobalTurbo
        : settings3DS.Turbo;

    u32 gbInput = 0;

    bool up    = (held & KEY_DUP)    || (bindCirclePad && (held & KEY_CPAD_UP));
    bool down  = (held & KEY_DDOWN)  || (bindCirclePad && (held & KEY_CPAD_DOWN));
    bool left  = (held & KEY_DLEFT)  || (bindCirclePad && (held & KEY_CPAD_LEFT));
    bool right = (held & KEY_DRIGHT) || (bindCirclePad && (held & KEY_CPAD_RIGHT));

    // Real DMG/CGB D-pad mechanics make opposing directions unreachable.
    if (up && !down)       gbInput |= CORE_BTN_UP;
    else if (down && !up)  gbInput |= CORE_BTN_DOWN;

    if (left && !right)      gbInput |= CORE_BTN_LEFT;
    else if (right && !left) gbInput |= CORE_BTN_RIGHT;

    static const u32 keys3ds[10] = {
        KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L, KEY_R, KEY_ZL, KEY_ZR, KEY_SELECT, KEY_START
    };

    for (int i = 0; i < 10; i++) {
        if ((held & keys3ds[i]) == 0) continue;

        if (i < 8 && turbo[i] > 0) {
            u32 period = static_cast<u32>(turbo[i]) * 2u;
            if ((g_turboFrameCounter % period) >= static_cast<u32>(turbo[i]))
                continue;
        }

        for (int j = 0; j < 4; j++) {
            gbInput |= static_cast<u32>(mapping[i][j]);
        }
    }

    g_turboFrameCounter++;
    g_currentCoreInput = gbInput;
}

static void uploadLinearFrameToVram() {
    if (!g_fbLinear || !GPU3DS.textures[GAME_MAIN].tex.data) return;

    C3D_Tex* tex = &GPU3DS.textures[GAME_MAIN].tex;
    t3dsStartTimer(TIMER_UPLOAD_FRAME);
    GSPGPU_FlushDataCache(g_fbLinear, GAME_FB_LINEAR_BYTES);
    C3D_SyncDisplayTransfer(
        reinterpret_cast<u32*>(g_fbLinear), GX_BUFFER_DIM(GAME_TEX_DIM, GAME_TEX_DIM),
        reinterpret_cast<u32*>(tex->data),  GX_BUFFER_DIM(GAME_TEX_DIM, GAME_TEX_DIM),
        GX_TRANSFER_OUT_TILED(1) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565)
    );
    t3dsStopTimer(TIMER_UPLOAD_FRAME);
}

static void drainBlipperToSoundRing() {
    if (!g_blipL || !g_blipR || g_audioOutBuf.empty()) return;

    while (true) {
        unsigned availL = blipper_read_avail(g_blipL);
        unsigned availR = blipper_read_avail(g_blipR);
        unsigned avail = availL < availR ? availL : availR;
        if (avail == 0) break;

        unsigned chunk = avail > static_cast<unsigned>(BLIPPER_OUT_CHUNK)
            ? static_cast<unsigned>(BLIPPER_OUT_CHUNK)
            : avail;

        blipper_read(g_blipL, g_audioOutBuf.data(),     chunk, 2);
        blipper_read(g_blipR, g_audioOutBuf.data() + 1, chunk, 2);
        snd3dsPushSamples(g_audioOutBuf.data(), static_cast<int>(chunk));
    }
}

//---------------------------------------------------------
// Core lifecycle
//---------------------------------------------------------
bool impl3dsInitialize() {
    log3dsWrite("load up and initialize screen shader");
    gpu3dsLoadShader(SPROGRAM_SCREEN,
        const_cast<u32*>(reinterpret_cast<const u32*>(shader_screen_shbin)),
        shader_screen_shbin_size, 0);

    if (!gpu3dsInitializeShaderUniformLocations()) {
        return false;
    }

    u32 gameTexParams = GPU_TEXTURE_MAG_FILTER(GPU_NEAREST)
                      | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST)
                      | GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_BORDER)
                      | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_BORDER);

    const SGPUTextureConfig gameTexConfig = {
        gameTexParams, GAME_MAIN, GPU_RGB565, GAME_TEX_DIM, GAME_TEX_DIM
    };

    if (!gpu3dsAllocVramTexture(&GPU3DS.textures[GAME_MAIN], &gameTexConfig)) {
        log3dsWrite("Unable to allocate VRAM texture GAME_MAIN");
        return false;
    }

    int vboScreenSize = static_cast<int>(gpu3dsGetNextPowerOf2(sizeof(SQuadVertex) * MAX_VERTICES_QUAD * 2));
    SVertexListInfo screenVboInfo = {
        VBO_SCREEN, vboScreenSize, sizeof(SQuadVertex), 4,
        {
            { GPU_FLOAT, 4 },
            { GPU_FLOAT, 2 },
            { GPU_UNSIGNED_BYTE, 4 },
            { GPU_UNSIGNED_BYTE, 4 }
        }
    };

    if (!gpu3dsAllocVertexList(&screenVboInfo)) {
        log3dsWrite("Unable to allocate VBO_SCREEN");
        return false;
    }

    g_fbLinear = static_cast<u16*>(linearAlloc(GAME_FB_LINEAR_BYTES));
    if (!g_fbLinear) {
        log3dsWrite("Unable to allocate linear frame buffer");
        return false;
    }

    g_fbStageA.resize(GAME_FRAME_PIXELS, 0);
    g_fbStageB.resize(GAME_FRAME_PIXELS, 0);
    g_soundBuf.resize(SOUND_BUF_CAPACITY, 0);
    g_audioOutBuf.resize(BLIPPER_OUT_CHUNK * 2, 0);

    g_blipL = blipper_new(32, 0.85, 6.5, 64, 1536, NULL);
    g_blipR = blipper_new(32, 0.85, 6.5, 64, 1536, NULL);
    if (!g_blipL || !g_blipR) {
        log3dsWrite("Unable to initialize audio resampler");
        return false;
    }

    g_gb = new gambatte::GB();
    g_gb->setInputGetter(&g_inputGetter);

    clearFramebuffers();
    log3dsWrite("Gambatte core initialized");
    return true;
}

void impl3dsFinalize() {
    for (int i = 0; i < VBO_COUNT; i++) {
        gpu3dsDeallocVertexList(&GPU3DS.vertices[i]);
    }
    for (int i = 0; i < TEX_COUNT; i++) {
        gpu3dsDestroyTexture(&GPU3DS.textures[i]);
    }

    if (g_fbLinear) {
        linearFree(g_fbLinear);
        g_fbLinear = nullptr;
    }

    if (g_blipL) {
        blipper_free(g_blipL);
        g_blipL = nullptr;
    }
    if (g_blipR) {
        blipper_free(g_blipR);
        g_blipR = nullptr;
    }

    delete g_gb;
    g_gb = nullptr;

    g_fbStageA.clear();
    g_fbStageB.clear();
    g_soundBuf.clear();
    g_audioOutBuf.clear();
    g_sramShadow.clear();
}

static void impl3dsCacheRomAssetInfo(const char* romFilePath) {
    static const struct { const char* ext; const char* folderName; } assets[] = {
        { ".png", GAME_SCREEN_BG_DIR },
        { ".png", SECOND_SCREEN_BG_DIR },
        { ".png", OVERLAYS_DIR },
        { ".cht", CHEATS_DIR },
    };

    char path[PATH_MAX];
    for (const auto& asset : assets) {
        file3dsGetRelatedPath(romFilePath, path, sizeof(path), asset.ext, asset.folderName, true);
        file3dsGetFileInfo(path);
    }
}

bool impl3dsLoadROM(char* romFilePath) {
    if (!g_gb || !romFilePath || romFilePath[0] == '\0') return false;

    impl3dsCacheRomAssetInfo(romFilePath);

    FILE* fp = file3dsOpen(romFilePath, "rb");
    if (!fp) {
        log3dsWrite("Unable to open ROM: %s", romFilePath);
        return false;
    }

    struct stat st = {};
    if (fstat(fileno(fp), &st) != 0 || st.st_size <= 0 || st.st_size > 16 * 1024 * 1024) {
        file3dsClose(fp);
        log3dsWrite("Invalid ROM file size: %s", romFilePath);
        return false;
    }

    std::vector<u8> romBuf(static_cast<size_t>(st.st_size));
    size_t bytesRead = fread(romBuf.data(), 1, romBuf.size(), fp);
    file3dsClose(fp);
    if (bytesRead != romBuf.size()) {
        log3dsWrite("Short read loading ROM: %s", romFilePath);
        return false;
    }

    unsigned loadFlags = gambatte::GB::MULTICART_COMPAT;
    if (settings3DS.GbHardwareMode == Setting::GbHardwareMode::ForceDmg) {
        loadFlags |= gambatte::GB::FORCE_DMG;
    } else if (settings3DS.GbHardwareMode == Setting::GbHardwareMode::ForceCgb) {
        loadFlags |= gambatte::GB::FORCE_CGB;
    }

    if (g_gb->load(romBuf.data(), static_cast<unsigned>(romBuf.size()), loadFlags) != 0) {
        log3dsWrite("Gambatte failed to load ROM: %s", romFilePath);
        return false;
    }

    memset(&coreRomInfo, 0, sizeof(coreRomInfo));
    snprintf(coreRomInfo.romFilename, sizeof(coreRomInfo.romFilename), "%s", romFilePath);
    coreRomInfo.romCrc32 = static_cast<u32>(crc32(0L, romBuf.data(), static_cast<uInt>(romBuf.size())));
    coreRomInfo.romSizeBytes = static_cast<u32>(romBuf.size());
    coreRomInfo.ramSizeBytes = g_gb->savedata_size();
    coreRomInfo.isCgb = g_gb->isCgb();
    coreRomInfo.hasBattery = (g_gb->savedata_size() > 0 || g_gb->rtcdata_size() > 0);
    parseGbHeaderInfo(romBuf.data(), romBuf.size(), romFilePath);

    // Load battery RAM (+ optional trailing RTC bytes) into Gambatte.
    unsigned sramSize = g_gb->savedata_size();
    unsigned rtcSize = g_gb->rtcdata_size();
    g_sramShadow.assign(sramSize, 0);

    if (coreRomInfo.hasBattery) {
        char sramPath[PATH_MAX];
        sav3dsGetSRAMPath(sramPath, sizeof(sramPath));
        FILE* sav = file3dsOpen(sramPath, "rb");
        if (sav) {
            if (sramSize > 0 && g_gb->savedata_ptr()) {
                size_t got = fread(g_gb->savedata_ptr(), 1, sramSize, sav);
                if (got > 0) {
                    memcpy(g_sramShadow.data(), g_gb->savedata_ptr(), got);
                }
            }
            if (rtcSize > 0 && g_gb->rtcdata_ptr()) {
                std::vector<u8> rtcTmp(rtcSize);
                size_t gotRtc = fread(rtcTmp.data(), 1, rtcSize, sav);
                if (gotRtc == rtcSize) {
                    memcpy(g_gb->rtcdata_ptr(), rtcTmp.data(), rtcSize);
                }
            }
            file3dsClose(sav);
        }
    }

    g_sramCheckCounter = 0;
    resetAudioPipeline();
    clearFramebuffers();

    impl3dsApplyCoreSettings();
    impl3dsLoadCheats();

    log3dsWrite("ROM loaded: %s (%s, CRC32=%08lX, %s)",
        coreRomInfo.romName,
        coreRomInfo.isCgb ? "CGB" : "DMG",
        static_cast<unsigned long>(coreRomInfo.romCrc32),
        coreRomInfo.mapperName);

    return true;
}

void impl3dsResetConsole() {
    if (!g_gb || !g_gb->isLoaded()) return;

    g_gb->reset();
    resetAudioPipeline();
    g_hasPrevFrame = false;
    g_hasValidFrame = false;
    g_pendingDraw = false;

    impl3dsApplyCoreSettings();
    impl3dsApplyCheats();
}

void impl3dsApplyCoreSettings() {
    if (!g_gb) return;

    static const u32 dmgGreen[4]  = { 0xE0F8D0u, 0x88C070u, 0x346856u, 0x081820u };
    static const u32 gbPocket[4]  = { 0xC4CFA1u, 0x8B956Du, 0x4D533Cu, 0x1F1F1Fu };
    static const u32 grayscale[4] = { 0xFFFFFFu, 0xAAAAAAu, 0x555555u, 0x000000u };

    const u32* pal = dmgGreen;
    switch (settings3DS.GbPalette) {
        case Setting::GbPalette::GbPocket:  pal = gbPocket;  break;
        case Setting::GbPalette::Grayscale: pal = grayscale; break;
        case Setting::GbPalette::DmgGreen:
        default:                            pal = dmgGreen;  break;
    }

    for (unsigned p = 0; p < 3; p++) {
        for (unsigned c = 0; c < 4; c++) {
            g_gb->setDmgPaletteColor(p, c, rgb888To565(pal[c]));
        }
    }

    switch (settings3DS.GbcColorCorrection) {
        case Setting::GbcColorCorrection::Off:
            g_gb->setColorCorrection(false);
            break;
        case Setting::GbcColorCorrection::Fast:
            g_gb->setColorCorrection(true);
            g_gb->setColorCorrectionMode(1);
            break;
        case Setting::GbcColorCorrection::Accurate:
        default:
            g_gb->setColorCorrection(true);
            g_gb->setColorCorrectionMode(0);
            break;
    }
}

void impl3dsUpdateUiAssets() {
    const struct UiAssetConfig {
        SGPU_TEXTURE_ID id;
        int settingValue;
        const char* folderName;
    } assets[] = {
        { UI_OVERLAY,   static_cast<int>(settings3DS.GameOverlay),    OVERLAYS_DIR },
        { UI_BG_GAME,   static_cast<int>(settings3DS.GameScreenBg),   GAME_SCREEN_BG_DIR },
        { UI_BG_SECOND, static_cast<int>(settings3DS.SecondScreenBg), SECOND_SCREEN_BG_DIR }
    };

    const char* targetPath = (coreRomInfo.romFilename[0] != '\0') ? coreRomInfo.romFilename : "";
    char fileName[PATH_MAX];

    for (const auto& asset : assets) {
        Setting::AssetMode mode = static_cast<Setting::AssetMode>(asset.settingValue);
        bool externalAssetActive = false;

        if (targetPath[0] != '\0' &&
            (mode == Setting::AssetMode::Adaptive || mode == Setting::AssetMode::CustomOnly)) {
            file3dsGetRelatedPath(targetPath, fileName, sizeof(fileName), ".png", asset.folderName, true);
            if (file3dsGetFileInfo(fileName).exists) {
                externalAssetActive = img3dsLoadAsset(asset.id, fileName);
            }
        }

        if (!externalAssetActive) {
            img3dsLoadAsset(asset.id);
        }
    }
}

//---------------------------------------------------------
// Frame execution
//---------------------------------------------------------
static void emulateOneCoreFrame(bool skipDrawingFrame) {
    if (!g_gb || !g_gb->isLoaded()) return;

    t3dsStartTimer(TIMER_CORE_RUN);

    bool wantVideo = !skipDrawingFrame;
    bool blendActive = wantVideo && settings3DS.InterframeBlending && !session3DS.turboMode;

    gambatte::video_pixel_t* videoOut = nullptr;
    int pitch = GAME_TEX_DIM;

    if (wantVideo) {
        if (blendActive) {
            videoOut = g_fbCurr;
            pitch = CORE_SCREEN_WIDTH;
        } else {
            videoOut = g_fbLinear;
            pitch = GAME_TEX_DIM;
        }
    }

    unsigned totalNativeSamples = 0;
    for (int guard = 0; guard < 20; guard++) {
        unsigned samples = SAMPLES_PER_STEP;
        long frameSample = g_gb->runFor(
            videoOut, pitch,
            g_soundBuf.data(), g_soundBuf.size(),
            samples
        );

        if (samples > 0) {
            const int16_t* s16 = reinterpret_cast<const int16_t*>(g_soundBuf.data());
            blipper_push_samples(g_blipL, s16,     samples, 2);
            blipper_push_samples(g_blipR, s16 + 1, samples, 2);
            drainBlipperToSoundRing();
            totalNativeSamples += samples;
        }

        if (frameSample >= 0) break;
        if (totalNativeSamples >= MAX_FRAME_SAMPLES) break;
    }

    if (wantVideo) {
        if (blendActive) {
            if (g_hasPrevFrame) {
                for (int y = 0; y < CORE_SCREEN_HEIGHT; y++) {
                    u16* dstRow = g_fbLinear + y * GAME_TEX_DIM;
                    const u16* currRow = g_fbCurr + y * CORE_SCREEN_WIDTH;
                    const u16* prevRow = g_fbPrev + y * CORE_SCREEN_WIDTH;
                    for (int x = 0; x < CORE_SCREEN_WIDTH; x++) {
                        dstRow[x] = blendRgb565(currRow[x], prevRow[x]);
                    }
                }
            } else {
                for (int y = 0; y < CORE_SCREEN_HEIGHT; y++) {
                    memcpy(g_fbLinear + y * GAME_TEX_DIM,
                           g_fbCurr + y * CORE_SCREEN_WIDTH,
                           CORE_SCREEN_WIDTH * sizeof(u16));
                }
            }
            std::swap(g_fbCurr, g_fbPrev);
            g_hasPrevFrame = true;
        } else {
            g_hasPrevFrame = false;
        }

        g_hasValidFrame = true;
        g_pendingDraw = true;
    } else {
        g_pendingDraw = false;
    }

    t3dsStopTimer(TIMER_CORE_RUN);
}

void impl3dsRunOneFrame(bool firstFrame, bool skipDrawingFrame) {
    if (firstFrame) {
        g_pendingDraw = false;
    }

    sav3dsProcessSaveCompletion(false);

    if (session3DS.sdWriteFailPending &&
        session3DS.sdWriteFailNotifCount < SD_WRITE_FAIL_NOTIF_LIMIT) {
        session3DS.sdWriteFailPending = false;
        session3DS.sdWriteFailNotifCount++;
        notif3dsTrigger(Notif::Misc, Notif::Type::Error, 3000, "Writing to SD card failed!");
    }

    notif3dsTick();
    notif3dsSync();

    // Transfer the emulated frame BEFORE gpu3dsFrameBegin (GSP transfer cannot run mid-frame).
    if (g_pendingDraw) {
        gpu3dsWaitForRenderQueue();
        uploadLinearFrameToVram();
    }

    const ScreenshotRegion* screenshotRegion = screenshot.pending ? &screenshot.region : nullptr;
    GPU3DS.screenshotScale = screenshotRegion ? screenshotRegion->scale : 0.0f;

    gpu3dsFrameBegin(0, g_pendingDraw);
    if (firstFrame || g_pendingDraw) {
        t3dsStartTimer(TIMER_DRAW_SCENE);
        impl3dsSceneRender(firstFrame, false, screenshotRegion);
        t3dsStopTimer(TIMER_DRAW_SCENE);
    }
    gpu3dsFrameEnd();
    GPU3DS.screenshotScale = 0.0f;

    g_pendingDraw = false;

    if (screenshot.pending && g_hasValidFrame) {
        sav3dsTakeScreenshot();
    }

    input3dsScanInputForEmulation();
    updateCoreInputFrom3DS();

    emulateOneCoreFrame(skipDrawingFrame);

    if (++g_sramCheckCounter >= SRAM_DIRTY_CHECK_FRAMES) {
        g_sramCheckCounter = 0;
        if (impl3dsIsSRAMDirty()) {
            sav3dsRequestSRAMSave();
        }
    }
}

bool impl3dsHasPendingDraw() {
    return g_pendingDraw;
}

//---------------------------------------------------------
// Save states & SRAM
//---------------------------------------------------------
size_t impl3dsSerializeState(u8* dst, size_t capacity) {
    if (!g_gb || !g_gb->isLoaded() || !dst) return 0;

    size_t sz = g_gb->stateSize();
    if (sz == 0 || sz > capacity) return 0;

    g_gb->saveState(dst);
    return sz;
}

bool impl3dsUnserializeState(const u8* src, size_t size) {
    if (!g_gb || !g_gb->isLoaded() || !src || size == 0) return false;

    bool ok = g_gb->loadState(src, size);
    if (ok) {
        resetAudioPipeline();
        g_hasPrevFrame = false;
        impl3dsApplyCoreSettings();
    }
    return ok;
}

bool impl3dsHasBatterySave() {
    return g_gb && g_gb->isLoaded() && (g_gb->savedata_size() > 0 || g_gb->rtcdata_size() > 0);
}

bool impl3dsIsSRAMDirty() {
    if (!g_gb || !g_gb->isLoaded()) return false;

    unsigned sramSize = g_gb->savedata_size();
    const void* sramPtr = g_gb->savedata_ptr();
    if (sramSize == 0 || !sramPtr || g_sramShadow.size() != sramSize) {
        return false;
    }

    return memcmp(sramPtr, g_sramShadow.data(), sramSize) != 0;
}

size_t impl3dsCopySRAM(u8* dst, size_t capacity) {
    if (!g_gb || !g_gb->isLoaded() || !dst) return 0;

    unsigned sramSize = g_gb->savedata_size();
    unsigned rtcSize = g_gb->rtcdata_size();
    size_t total = static_cast<size_t>(sramSize) + static_cast<size_t>(rtcSize);
    if (total == 0 || total > capacity) return 0;

    if (sramSize > 0 && g_gb->savedata_ptr()) {
        memcpy(dst, g_gb->savedata_ptr(), sramSize);
        if (g_sramShadow.size() == sramSize) {
            memcpy(g_sramShadow.data(), g_gb->savedata_ptr(), sramSize);
        }
    }
    if (rtcSize > 0 && g_gb->rtcdata_ptr()) {
        memcpy(dst + sramSize, g_gb->rtcdata_ptr(), rtcSize);
    }

    return total;
}

//---------------------------------------------------------
// Cheats (RetroArch .cht)
//---------------------------------------------------------
static void copyCheatsFromParsed(const std::vector<ParsedCheat>& parsed) {
    memset(&coreCheats, 0, sizeof(coreCheats));
    u32 count = static_cast<u32>(std::min(parsed.size(), static_cast<size_t>(MAX_CHEATS)));
    for (u32 i = 0; i < count; i++) {
        snprintf(coreCheats.c[i].name, sizeof(coreCheats.c[i].name), "%s", parsed[i].name.c_str());
        snprintf(coreCheats.c[i].code, sizeof(coreCheats.c[i].code), "%s", parsed[i].code.c_str());
        coreCheats.c[i].enabled = parsed[i].enabled;
    }
    coreCheats.numCheats = count;
}

void impl3dsApplyCheats() {
    if (!g_gb || !g_gb->isLoaded()) return;

    g_gb->clearCheats();

    std::string allGg;
    std::string allGs;

    for (u32 i = 0; i < coreCheats.numCheats; i++) {
        if (!coreCheats.c[i].enabled) continue;

        std::string gg;
        std::string gs;
        classifyCheatParts(coreCheats.c[i].code, gg, gs);

        if (!gg.empty()) {
            if (!allGg.empty()) allGg.push_back(';');
            allGg.append(gg);
        }
        if (!gs.empty()) {
            if (!allGs.empty()) allGs.push_back(';');
            allGs.append(gs);
        }
    }

    if (!allGg.empty()) g_gb->setGameGenie(allGg);
    if (!allGs.empty()) g_gb->setGameShark(allGs);
}

void impl3dsLoadCheats() {
    memset(&coreCheats, 0, sizeof(coreCheats));
    if (coreRomInfo.romFilename[0] == '\0') {
        impl3dsApplyCheats();
        return;
    }

    char path[PATH_MAX];
    file3dsGetRelatedPath(coreRomInfo.romFilename, path, sizeof(path), ".cht", CHEATS_DIR, true);

    if (file3dsGetFileInfo(path).exists) {
        FILE* fp = file3dsOpen(path, "rb");
        if (fp) {
            struct stat st = {};
            if (fstat(fileno(fp), &st) == 0 && st.st_size > 0 && st.st_size < 1024 * 1024) {
                std::string text(static_cast<size_t>(st.st_size), '\0');
                size_t got = fread(&text[0], 1, text.size(), fp);
                text.resize(got);
                std::vector<ParsedCheat> parsed;
                parseChtText(text.c_str(), parsed, MAX_CHEATS);
                copyCheatsFromParsed(parsed);
            }
            file3dsClose(fp);
        }
    }

    impl3dsApplyCheats();
}

void impl3dsReloadCheats() {
    if (!session3DS.isRomLoaded) return;

    impl3dsLoadCheats();
    session3DS.cheatsDirty = false;
}

void impl3dsSaveCheats() {
    if (!session3DS.cheatsDirty || !session3DS.isRomLoaded) return;

    char path[PATH_MAX];
    file3dsGetRelatedPath(coreRomInfo.romFilename, path, sizeof(path), ".cht", CHEATS_DIR, true);

    if (coreCheats.numCheats == 0) {
        if (remove(path) == 0) {
            file3dsSetFileInfo(path, { false, 0 });
        }
        session3DS.cheatsDirty = false;
        return;
    }

    std::vector<ParsedCheat> entries;
    entries.reserve(coreCheats.numCheats);
    for (u32 i = 0; i < coreCheats.numCheats; i++) {
        ParsedCheat pc;
        pc.name = coreCheats.c[i].name;
        pc.code = coreCheats.c[i].code;
        pc.enabled = coreCheats.c[i].enabled;
        entries.push_back(pc);
    }

    std::string text = serializeChtText(entries);
    FILE* fp = file3dsOpen(path, "wb");
    if (fp) {
        size_t written = fwrite(text.data(), 1, text.size(), fp);
        file3dsClose(fp);
        if (written == text.size()) {
            struct stat st = {};
            u64 mtime = (stat(path, &st) == 0) ? static_cast<u64>(st.st_mtime) : 0;
            file3dsSetFileInfo(path, { true, mtime });
        } else {
            session3DS.sdWriteFailPending = true;
        }
    } else {
        session3DS.sdWriteFailPending = true;
    }

    session3DS.cheatsDirty = false;
}

void impl3dsDeleteCheat(int index) {
    if (index < 0 || static_cast<u32>(index) >= coreCheats.numCheats) return;

    for (u32 i = static_cast<u32>(index); i + 1 < coreCheats.numCheats; i++) {
        coreCheats.c[i] = coreCheats.c[i + 1];
    }
    coreCheats.numCheats--;
    memset(&coreCheats.c[coreCheats.numCheats], 0, sizeof(CoreCheat));

    session3DS.cheatsDirty = true;
    impl3dsApplyCheats();
}

//---------------------------------------------------------
// Screenshots and thumbnails
//---------------------------------------------------------
void impl3dsPrepareScreenshot(float scale, bool centered) {
    if (screenshot.pending) return;

    screenshot.region.scale = scale;
    screenshot.region.width = static_cast<u16>(CORE_SCREEN_WIDTH * scale);
    screenshot.region.height = static_cast<u16>(CORE_SCREEN_HEIGHT * scale);

    if (centered) {
        screenshot.region.x = static_cast<u16>((settings3DS.GameScreenWidth - screenshot.region.width) / 2);
        screenshot.region.y = static_cast<u16>((SCREEN_HEIGHT - screenshot.region.height) / 2);
    } else {
        screenshot.region.x = static_cast<u16>(settings3DS.GameScreenWidth - screenshot.region.width);
        screenshot.region.y = static_cast<u16>(SCREEN_HEIGHT - screenshot.region.height);
    }
    screenshot.pending = true;
}

void impl3dsCaptureScreenshot(u8* pixels, bool renderFrame, int* width, int* height) {
    (void)renderFrame;
    screenshot.pending = false;

    if (!pixels || !g_fbLinear) {
        if (width) *width = 0;
        if (height) *height = 0;
        return;
    }

    u8* dst = pixels;
    for (int y = 0; y < CORE_SCREEN_HEIGHT; y++) {
        const u16* row = g_fbLinear + y * GAME_TEX_DIM;
        for (int x = 0; x < CORE_SCREEN_WIDTH; x++) {
            u16 p = row[x];
            u32 r5 = (p >> 11) & 0x1F;
            u32 g6 = (p >> 5) & 0x3F;
            u32 b5 = p & 0x1F;
            *dst++ = static_cast<u8>((r5 << 3) | (r5 >> 2));
            *dst++ = static_cast<u8>((g6 << 2) | (g6 >> 4));
            *dst++ = static_cast<u8>((b5 << 3) | (b5 >> 2));
        }
    }

    if (width) *width = CORE_SCREEN_WIDTH;
    if (height) *height = CORE_SCREEN_HEIGHT;
    skipNextFpsUpdate = true;
}

bool impl3dsCaptureThumbnail(u16* pixels, size_t pixelsCapacity, int* width, int* height) {
    const int thumbW = SAVE_THUMBNAIL_WIDTH;       // 80
    const int thumbH = SAVE_THUMBNAIL_MAX_HEIGHT;  // 72
    const size_t needed = static_cast<size_t>(thumbW * thumbH) * sizeof(u16);

    if (!pixels || !g_fbLinear || !g_hasValidFrame || needed > pixelsCapacity) {
        return false;
    }

    // Column-major order with top row last, matching img3dsDrawSwizzledRgb565.
    for (int y = 0; y < thumbH; y++) {
        const u16* row0 = g_fbLinear + (y * 2) * GAME_TEX_DIM;
        const u16* row1 = g_fbLinear + (y * 2 + 1) * GAME_TEX_DIM;
        int dstRow = thumbH - 1 - y;

        for (int x = 0; x < thumbW; x++) {
            u16 p00 = row0[x * 2];
            u16 p01 = row0[x * 2 + 1];
            u16 p10 = row1[x * 2];
            u16 p11 = row1[x * 2 + 1];

            u32 r = (((p00 >> 11) & 0x1F) + ((p01 >> 11) & 0x1F) +
                     ((p10 >> 11) & 0x1F) + ((p11 >> 11) & 0x1F) + 2) >> 2;
            u32 g = (((p00 >> 5)  & 0x3F) + ((p01 >> 5)  & 0x3F) +
                     ((p10 >> 5)  & 0x3F) + ((p11 >> 5)  & 0x3F) + 2) >> 2;
            u32 b = ((p00 & 0x1F)         + (p01 & 0x1F)         +
                     (p10 & 0x1F)         + (p11 & 0x1F)         + 2) >> 2;

            pixels[x * thumbH + dstRow] = static_cast<u16>((r << 11) | (g << 5) | b);
        }
    }

    if (width) *width = thumbW;
    if (height) *height = thumbH;
    return true;
}

//---------------------------------------------------------
// Display & cache control
//---------------------------------------------------------
static void impl3dsApplyCacheOp(gfxScreen_t screen, bool isStereo, bool isWide, GSP_CacheCallback cacheOp) {
    (void)isWide;
    u16 w = 0, h = 0;
    u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, &w, &h);

    u32 bpp = 2;
    switch (gfxGetScreenFormat(screen)) {
        case GSP_RGBA8_OES: bpp = 4; break;
        case GSP_BGR8_OES:  bpp = 3; break;
        default:            bpp = 2; break;
    }

    u32 dataSize = static_cast<u32>(w) * static_cast<u32>(h) * bpp;
    cacheOp(fb, dataSize);

    if (screen == GFX_TOP && isStereo) {
        u8* fbRight = gfxGetFramebuffer(screen, GFX_RIGHT, &w, &h);
        cacheOp(fbRight, dataSize);
    }
}

void impl3dsFlushScreen(gfxScreen_t screen, bool isTopStereo, bool isWide) {
    impl3dsApplyCacheOp(screen, isTopStereo, isWide, GSPGPU_FlushDataCache);
}

void impl3dsInvalidateScreen(gfxScreen_t screen, bool isTopStereo, bool isWide) {
    impl3dsApplyCacheOp(screen, isTopStereo, isWide, GSPGPU_InvalidateDataCache);
}

void impl3dsClearTopFramebuffers() {
    u32 bpp = 2;
    switch (gfxGetScreenFormat(GFX_TOP)) {
        case GSP_RGBA8_OES: bpp = 4; break;
        case GSP_BGR8_OES:  bpp = 3; break;
        default:            bpp = 2; break;
    }

    for (int page = 0; page < 2; page++) {
        u16 w = 0, h = 0;
        u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
        u32 dataSize = static_cast<u32>(w) * static_cast<u32>(h) * bpp;
        if (fb && dataSize > 0) {
            memset(fb, 0, dataSize);
            GSPGPU_FlushDataCache(fb, dataSize);
        }
        gfxScreenSwapBuffers(GFX_TOP, false);
    }
}

static float impl3dsGetPauseAnimationProgress(bool paused) {
    u64 now = osGetTime();

    if (!paused) {
        pauseAnimationStartedAt = 0;
        return 1.0f;
    }

    if (!pauseAnimationStartedAt) {
        pauseAnimationStartedAt = now;
    }

    u64 elapsed = now - pauseAnimationStartedAt;
    if (elapsed >= PAUSE_ANIMATION_DURATION_MS) {
        return 1.0f;
    }

    return ui3dsSmoothstep(static_cast<float>(elapsed) / PAUSE_ANIMATION_DURATION_MS);
}

bool impl3dsPauseAnimationRunning() {
    return pauseAnimationStartedAt && (osGetTime() - pauseAnimationStartedAt < PAUSE_ANIMATION_DURATION_MS);
}

void impl3dsSceneRender(bool firstFrame, bool paused, const ScreenshotRegion* screenshotRegion) {
    (void)firstFrame;

    SVertexList* list = &GPU3DS.vertices[VBO_SCREEN];
    GameScreenViewport vp = {};
    float pauseProgress = screenshotRegion ? 1.0f : impl3dsGetPauseAnimationProgress(paused);

    bool drawBackground = true;
    bool drawOverlays = true;
    bool balancedFilter = false;
    GPU_TEXTURE_FILTER_PARAM baseFilter = GPU_NEAREST;

    if (screenshotRegion) {
        vp.sWidth = screenshotRegion->width;
        vp.sHeight = screenshotRegion->height;
        vp.cHeight = screenshotRegion->height;
        vp.sx0 = screenshotRegion->x;
        vp.sy0 = screenshotRegion->y;
        vp.sx1 = vp.sx0 + vp.sWidth;
        vp.sy1 = vp.sy0 + vp.cHeight;
        vp.tx0 = 0.0f;
        vp.tx1 = static_cast<float>(CORE_SCREEN_WIDTH);
        vp.ty0 = static_cast<float>(GAME_TEX_DIM);
        vp.ty1 = static_cast<float>(GAME_TEX_DIM - CORE_SCREEN_HEIGHT);
        drawBackground = false;
        drawOverlays = false;
    } else {
        vp.sWidth = settings3DS.StretchWidth;
        vp.sHeight = (settings3DS.StretchHeight == -1) ? CORE_SCREEN_HEIGHT : settings3DS.StretchHeight;

        int cropTopSource = 0;
        int cropBottomSource = 0;
        if (settings3DS.CropEnabled) {
            cropTopSource = settings3DS.CropTop;
            cropBottomSource = settings3DS.CropBottom;
            if (cropTopSource < 0) cropTopSource = 0;
            if (cropBottomSource < 0) cropBottomSource = 0;
            if (cropTopSource + cropBottomSource >= CORE_SCREEN_HEIGHT - 16) {
                cropBottomSource = std::max(0, CORE_SCREEN_HEIGHT - 16 - cropTopSource);
            }
        }

        int cropTopPx = cropTopSource;
        int cropBottomPx = cropBottomSource;
        if (vp.sHeight != CORE_SCREEN_HEIGHT) {
            int topEdgePx = (cropTopSource * vp.sHeight + CORE_SCREEN_HEIGHT / 2) / CORE_SCREEN_HEIGHT;
            int bottomEdgePx = ((CORE_SCREEN_HEIGHT - cropBottomSource) * vp.sHeight + CORE_SCREEN_HEIGHT / 2) / CORE_SCREEN_HEIGHT;
            cropTopPx = topEdgePx;
            cropBottomPx = vp.sHeight - bottomEdgePx;
        }

        int cHeight = vp.sHeight - cropTopPx - cropBottomPx;
        bool overscanActive = settings3DS.Overscan && (cHeight > 0) && (cHeight < SCREEN_HEIGHT);
        if (overscanActive) {
            if (vp.sWidth < settings3DS.GameScreenWidth) {
                vp.sWidth = (vp.sWidth * SCREEN_HEIGHT + cHeight / 2) / cHeight;
            }
            vp.cHeight = SCREEN_HEIGHT;
        } else {
            vp.cHeight = cHeight;
        }

        vp.sx0 = (settings3DS.GameScreenWidth - vp.sWidth) / 2;
        vp.sy0 = (SCREEN_HEIGHT - vp.cHeight) / 2;
        vp.sx1 = vp.sx0 + vp.sWidth;
        vp.sy1 = vp.sy0 + vp.cHeight;

        // Half-texel inset on scaled axes avoids linear edge bleed; 1:1 axes keep exact centers.
        float insetX = (vp.sWidth == CORE_SCREEN_WIDTH) ? 0.0f : 0.5f;
        float insetY = (vp.sHeight == CORE_SCREEN_HEIGHT && !overscanActive) ? 0.0f : 0.5f;

        // With GX_TRANSFER_FLIP_VERT(0), linear row 0 resides at texY = GAME_TEX_DIM (v = 1.0).
        vp.tx0 = insetX;
        vp.tx1 = static_cast<float>(CORE_SCREEN_WIDTH) - insetX;
        vp.ty0 = static_cast<float>(GAME_TEX_DIM - cropTopSource) - (cropTopSource == 0 ? insetY : 0.0f);
        vp.ty1 = static_cast<float>(GAME_TEX_DIM - (CORE_SCREEN_HEIGHT - cropBottomSource)) + (cropBottomSource == 0 ? insetY : 0.0f);

        bool isFullScreen = vp.sWidth >= settings3DS.GameScreenWidth && vp.cHeight >= SCREEN_HEIGHT;
        drawBackground = !isFullScreen;

        bool isStretched = (settings3DS.ScreenStretch != Setting::ScreenStretch::None) || overscanActive;
        if (isStretched && settings3DS.ScreenFilter == Setting::ScreenFilter::Smooth) {
            baseFilter = GPU_LINEAR;
        } else if (isStretched && settings3DS.ScreenFilter == Setting::ScreenFilter::Balanced) {
            balancedFilter = true;
        }
    }

    if (drawBackground) {
        gpu3dsClearScreen(settings3DS.GameScreen, false);
    }

    GPU3DS.activeSide = GFX_LEFT;
    gpu3dsSetDefaultRenderState(SPROGRAM_SCREEN, false);

    if (drawBackground) {
        img3dsDrawBackground(UI_BG_GAME, paused, 0.0f);
    }

    C3D_TexSetFilter(&GPU3DS.textures[GAME_MAIN].tex, baseFilter, baseFilter);
    GPU3DS.currentRenderState.textureEnv = TEX_ENV_REPLACE_TEXTURE0;
    GPU3DS.currentRenderState.textureBind = GAME_MAIN;

    gpu3dsAddSimpleQuadVertexes(
        static_cast<float>(vp.sx0), static_cast<float>(vp.sy0),
        static_cast<float>(vp.sx1), static_cast<float>(vp.sy1),
        vp.tx0, vp.ty0, vp.tx1, vp.ty1, 0.0f);
    gpu3dsDraw(list, NULL, list->count);

    if (balancedFilter) {
        C3D_TexSetFilter(&GPU3DS.textures[GAME_MAIN].tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexBind(0, &GPU3DS.textures[GAME_MAIN].tex);

        gpu3dsAddSimpleQuadVertexes(
            static_cast<float>(vp.sx0), static_cast<float>(vp.sy0),
            static_cast<float>(vp.sx1), static_cast<float>(vp.sy1),
            vp.tx0, vp.ty0, vp.tx1, vp.ty1, 0.0f, 0xFFFFFF88);

        GPU3DS.currentRenderState.textureEnv = TEX_ENV_REPLACE_TEXTURE0_VERTEX_ALPHA;
        GPU3DS.currentRenderState.textureBind = GAME_MAIN;
        GPU3DS.currentRenderState.alphaBlending = ALPHA_BLENDING_ENABLED;
        gpu3dsDraw(list, NULL, list->count);

        C3D_TexSetFilter(&GPU3DS.textures[GAME_MAIN].tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexBind(0, &GPU3DS.textures[GAME_MAIN].tex);
        GPU3DS.currentRenderState.alphaBlending = ALPHA_BLENDING_DISABLED;
        GPU3DS.currentRenderState.textureEnv = TEX_ENV_REPLACE_TEXTURE0;
    }

    if (drawOverlays) {
        img3dsDrawScanlines(
            static_cast<float>(vp.sx0), static_cast<float>(vp.sy0),
            static_cast<float>(vp.sx1), static_cast<float>(vp.sy1),
            vp.sWidth, vp.cHeight);

        img3dsDrawGameOverlay(UI_OVERLAY, vp.sWidth, vp.cHeight);

        if (paused) {
            img3dsDrawPause(UI_PAUSE, pauseProgress, PAUSE_ANIMATION_Y_OFFSET * (1.0f - pauseProgress));
        }

        notif3dsDraw(UI_NOTIF_MSG);
        notif3dsDraw(UI_NOTIF_FPS);
    }
}

std::string impl3dsGetRomInfo() {
    if (!session3DS.isRomLoaded) return "";

    char buf[256];
    snprintf(buf, sizeof(buf),
        "Title: %s\nMode: %s\nMapper: %s\nROM: %lu KB | RAM: %lu KB%s\nCRC32: %08lX",
        coreRomInfo.romName,
        coreRomInfo.isCgb ? "Game Boy Color" : "Game Boy",
        coreRomInfo.mapperName,
        static_cast<unsigned long>(coreRomInfo.romSizeBytes / 1024),
        static_cast<unsigned long>(coreRomInfo.ramSizeBytes / 1024),
        coreRomInfo.hasRtc ? " + RTC" : "",
        static_cast<unsigned long>(coreRomInfo.romCrc32));
    return std::string(buf);
}
