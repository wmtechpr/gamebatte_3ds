#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "gambatte.h"
#include "blipper.h"
#include "3dsaudio_ring.h"
#include "3dscheat_parse.h"

void cartridge_set_rumble(unsigned active) {
    (void)active;
}

class DummyInputGetter : public gambatte::InputGetter {
public:
    unsigned buttons = 0;
    unsigned operator()() override { return buttons; }
};

static std::vector<uint8_t> buildSyntheticGbRom() {
    std::vector<uint8_t> rom(32 * 1024, 0x00);

    // Entry point at 0x0100: nop; jp 0x0150
    rom[0x0100] = 0x00; // NOP
    rom[0x0101] = 0xC3; // JP a16
    rom[0x0102] = 0x50;
    rom[0x0103] = 0x01;

    // Official Nintendo logo bytes (0x0104..0x0133)
    static const uint8_t logo[48] = {
        0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
        0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
        0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
        0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E
    };
    memcpy(&rom[0x0104], logo, sizeof(logo));

    // Title at 0x0134
    const char* title = "GAMBATTE3DS";
    memcpy(&rom[0x0134], title, strlen(title));

    rom[0x0143] = 0x00; // DMG
    rom[0x0147] = 0x03; // MBC1+RAM+BATTERY
    rom[0x0148] = 0x00; // 32 KB ROM
    rom[0x0149] = 0x02; // 8 KB SRAM

    // Header checksum at 0x014D
    uint8_t cksum = 0;
    for (int addr = 0x0134; addr <= 0x014C; addr++) {
        cksum = static_cast<uint8_t>(cksum - rom[addr] - 1);
    }
    rom[0x014D] = cksum;

    // Program at 0x0150:
    // Turn on LCD (LCDC=0x91 -> FF40), enable sound master (0x80 -> FF26),
    // configure Channel 1 sweep/duty/envelope/freq so APU runs, and spin in jr -2.
    size_t pc = 0x0150;
    auto emitWriteIo = [&](uint8_t reg, uint8_t val) {
        rom[pc++] = 0x3E; // LD A, val
        rom[pc++] = val;
        rom[pc++] = 0xE0; // LDH (reg), A
        rom[pc++] = reg;
    };

    emitWriteIo(0x26, 0x80); // NR52: sound on
    emitWriteIo(0x25, 0xFF); // NR51: all channels L/R
    emitWriteIo(0x24, 0x77); // NR50: max volume L/R
    emitWriteIo(0x11, 0x80); // NR11: 50% duty
    emitWriteIo(0x12, 0xF0); // NR12: envelope max
    emitWriteIo(0x13, 0x00); // NR13: freq lo
    emitWriteIo(0x14, 0x87); // NR14: trigger channel 1
    emitWriteIo(0x47, 0xE4); // BGP palette
    emitWriteIo(0x40, 0x91); // LCDC: LCD on, BG on
    rom[pc++] = 0x18;        // JR -2 (infinite loop)
    rom[pc++] = 0xFE;

    return rom;
}

static void testGambatteCoreAndAudio() {
    std::vector<uint8_t> rom = buildSyntheticGbRom();

    gambatte::GB gb;
    DummyInputGetter input;
    gb.setInputGetter(&input);

    int loadRc = gb.load(rom.data(), static_cast<unsigned>(rom.size()), gambatte::GB::MULTICART_COMPAT);
    assert(loadRc == 0);
    assert(gb.isLoaded());
    assert(!gb.isCgb());
    assert(gb.savedata_size() == 8192);
    assert(gb.savedata_ptr() != nullptr);

    // Apply DMG palette & color correction APIs
    for (unsigned p = 0; p < 3; p++) {
        gb.setDmgPaletteColor(p, 0, 0xFFFF);
        gb.setDmgPaletteColor(p, 1, 0xAD55);
        gb.setDmgPaletteColor(p, 2, 0x52AA);
        gb.setDmgPaletteColor(p, 3, 0x0000);
    }
    gb.setColorCorrection(true);
    gb.setColorCorrectionMode(0);

    blipper_t* blipL = blipper_new(32, 0.85, 6.5, 64, 1536, NULL);
    blipper_t* blipR = blipper_new(32, 0.85, 6.5, 64, 1536, NULL);
    assert(blipL != nullptr && blipR != nullptr);

    std::vector<gambatte::video_pixel_t> video(256 * 144, 0x1234);
    std::vector<gambatte::uint_least32_t> soundBuf(8192, 0);
    std::vector<int16_t> outBuf(4096, 0);

    unsigned totalAudioFrames = 0;
    for (int frame = 0; frame < 15; frame++) {
        unsigned totalNative = 0;
        for (int step = 0; step < 20; step++) {
            unsigned samples = 2064;
            long blit = gb.runFor(video.data(), 256, soundBuf.data(), soundBuf.size(), samples);
            if (samples > 0) {
                const int16_t* s16 = reinterpret_cast<const int16_t*>(soundBuf.data());
                blipper_push_samples(blipL, s16, samples, 2);
                blipper_push_samples(blipR, s16 + 1, samples, 2);

                unsigned avail = std::min(blipper_read_avail(blipL), blipper_read_avail(blipR));
                assert(avail * 2 <= outBuf.size());
                if (avail > 0) {
                    blipper_read(blipL, outBuf.data(), avail, 2);
                    blipper_read(blipR, outBuf.data() + 1, avail, 2);
                    totalAudioFrames += avail;
                }
                totalNative += samples;
            }
            if (blit >= 0 || totalNative >= 37152) break;
        }
    }

    // 15 frames * ~548.625 samples/frame = ~8229 decimated samples
    assert(totalAudioFrames > 7500 && totalAudioFrames < 9000);

    // Framebuffer should have been updated from initial sentinel 0x1234 to DMG color 0 (0xFFFF)
    assert(video[0] == 0xFFFF);

    // Save/load state round-trip
    size_t stSize = gb.stateSize();
    assert(stSize > 0);
    std::vector<uint8_t> state(stSize, 0);
    gb.saveState(state.data());
    assert(!gb.loadState(state.data(), stSize - 1));
    assert(gb.loadState(state.data(), stSize));

    // Continue running 1 frame after loadState
    unsigned samples = 2064;
    long postLoadBlit = -1;
    for (int step = 0; step < 20 && postLoadBlit < 0; step++) {
        samples = 2064;
        postLoadBlit = gb.runFor(video.data(), 256, soundBuf.data(), soundBuf.size(), samples);
    }
    assert(postLoadBlit >= 0);

    // Game Genie & GameShark cheat application + clear
    gb.setGameGenie("010-123-456;AA1-BB2");
    gb.setGameShark("01FF00C0;010101C0");
    gb.clearCheats();

    blipper_free(blipL);
    blipper_free(blipR);
    printf("[PASS] testGambatteCoreAndAudio (frames=15, audioSamples=%u, stateSize=%zu)\n",
           totalAudioFrames, stSize);
}

static void testStereoRingBuffer() {
    StereoRingBuffer ring;
    int16_t tmp[StereoRingBuffer::CAPACITY_FRAMES * 2];

    // Underrun returns 0 popped and zero-fills output
    memset(tmp, 0x5A, sizeof(int16_t) * 8);
    assert(ring.pop(tmp, 4) == 0);
    for (int i = 0; i < 8; i++) assert(tmp[i] == 0);

    // Push & pop normal frames
    int16_t inA[6] = { 10, -10, 20, -20, 30, -30 };
    assert(ring.push(inA, 3) == 0);
    assert(ring.available() == 3);
    assert(ring.pop(tmp, 2) == 2);
    assert(tmp[0] == 10 && tmp[1] == -10 && tmp[2] == 20 && tmp[3] == -20);
    assert(ring.available() == 1);

    // Partial pop with underrun tail
    assert(ring.pop(tmp, 3) == 1);
    assert(tmp[0] == 30 && tmp[1] == -30);
    for (int i = 2; i < 6; i++) assert(tmp[i] == 0);
    assert(ring.available() == 0);

    // Wrap-around test
    std::vector<int16_t> chunk(5000 * 2);
    for (size_t i = 0; i < 5000; i++) {
        chunk[i * 2] = static_cast<int16_t>(i);
        chunk[i * 2 + 1] = static_cast<int16_t>(-static_cast<int>(i));
    }
    assert(ring.push(chunk.data(), 5000) == 0);
    assert(ring.pop(tmp, 4000) == 4000);
    assert(ring.push(chunk.data(), 5000) == 0); // wraps across CAPACITY_FRAMES (8192)
    assert(ring.available() == 6000);
    assert(ring.pop(tmp, 1000) == 1000);
    assert(tmp[0] == 4000 && tmp[1] == -4000);
    assert(ring.pop(tmp, 5000) == 5000);
    assert(tmp[0] == 0 && tmp[1] == 0 && tmp[4999 * 2] == 4999 && tmp[4999 * 2 + 1] == -4999);
    assert(ring.available() == 0);

    // Overflow drops oldest frames
    assert(ring.push(chunk.data(), 5000) == 0);
    size_t dropped = ring.push(chunk.data(), 5000); // 10000 - 8192 = 1808 dropped
    assert(dropped == 1808);
    assert(ring.available() == StereoRingBuffer::CAPACITY_FRAMES);
    assert(ring.pop(tmp, 1) == 1);
    assert(tmp[0] == 1808 && tmp[1] == -1808);

    // Reset clears count
    ring.reset();
    assert(ring.available() == 0);

    printf("[PASS] testStereoRingBuffer\n");
}

static void testCheatParserAndClassifier() {
    const char* rawCht =
        "# Sample GB cheats\n"
        "cheats = 3\n"
        "cheat0_desc = \"Infinite \\\"Lives\\\"\"\n"
        "cheat0_code = \"001-234-567+01aa22cc\"\n"
        "cheat0_enable = true\n"
        "cheat1_desc = \"Max Money\"\n"
        "cheat1_code = \"019935D1\"\n"
        "cheat1_enable = false\n"
        "cheat2_desc = \"Invincible\"\n"
        "cheat2_code = \"AAA-BBB\"\n"
        "cheat2_enable = 1\n";

    std::vector<ParsedCheat> parsed;
    parseChtText(rawCht, parsed, 256);
    assert(parsed.size() == 3);
    assert(parsed[0].name == "Infinite \"Lives\"");
    assert(parsed[0].code == "001-234-567+01aa22cc");
    assert(parsed[0].enabled == true);
    assert(parsed[1].name == "Max Money");
    assert(parsed[1].code == "019935D1");
    assert(parsed[1].enabled == false);
    assert(parsed[2].enabled == true);

    std::string gg, gs;
    classifyCheatParts(parsed[0].code.c_str(), gg, gs);
    assert(gg == "001-234-567");
    assert(gs == "01AA22CC");

    std::string serialized = serializeChtText(parsed);
    std::vector<ParsedCheat> reparsed;
    parseChtText(serialized.c_str(), reparsed, 256);
    assert(reparsed.size() == parsed.size());
    for (size_t i = 0; i < parsed.size(); i++) {
        assert(reparsed[i].name == parsed[i].name);
        assert(reparsed[i].code == parsed[i].code);
        assert(reparsed[i].enabled == parsed[i].enabled);
    }

    printf("[PASS] testCheatParserAndClassifier\n");
}

int main() {
    testGambatteCoreAndAudio();
    testStereoRingBuffer();
    testCheatParserAndClassifier();
    printf("ALL HOST SELF-TESTS PASSED\n");
    return 0;
}
