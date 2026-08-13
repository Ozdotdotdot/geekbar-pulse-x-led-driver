// Driver for the PLS916H LED matrix controller found in the Geekbar Pulse X
// flex/LED film. Based on reverse-engineering from:
//   https://gist.github.com/The5thZone/fcb294b999651d99634e3481fc5ee4ed
//   https://github.com/schlae/vapere
//
// Wiring (classic ESP32-WROOM-32 dev board):
//   Flex tab G -> ESP32 GND
//   Flex tab C -> ESP32 IO18 (SCK)
//   Flex tab D -> ESP32 IO23 (MOSI)
//   Flex tab V -> ESP32 3.3V (fine for bring-up; move to direct battery
//                 for sustained full-brightness/all-LED use)
//
// NOTE: on some boards the V test point isn't actually wired to the chip's
// VCC pin -- confirm continuity to the chip (or jumper to the nearby
// capacitor, e.g. C2) if nothing lights up.

#include <Arduino.h>
#include <SPI.h>
#include <math.h>

#include "led_groups.h"

#define PIN_CLK GPIO_NUM_18
#define PIN_DIN GPIO_NUM_23

static const size_t NUM_LEDS = 144;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("PLS916H driver bring-up");

    SPI.begin(PIN_CLK, -1 /* MISO unused */, PIN_DIN);
    randomSeed(esp_random());
}

// 8-bit checksum over the 144 data bytes.
uint8_t chk8_pls916h(uint8_t (&data)[NUM_LEDS]) {
    uint8_t b = 0;
    for (size_t i = 0; i < NUM_LEDS; i++) {
        b += data[i];
    }
    return b;
}

// Write a 144-byte brightness frame to the PLS916H.
void write_pls916h(uint8_t (&data)[NUM_LEDS]) {
    static struct {
        // Header captured verbatim from a logic analyzer trace of the
        // stock main board talking to the chip -- meaning of individual
        // bytes is not understood, treat as an opaque preamble.
        uint8_t header[13] = {0x5A, 0xFF, 0x01, 0x5A, 0x24, 0x21,
                               0x3D, 0x01, 0x83, 0x5A, 0xFF, 0x02, 0x5B};

        uint8_t data[NUM_LEDS] = {0};
        uint8_t checksum = 0;

        // Also captured verbatim, meaning not understood.
        uint8_t tail[4] = {0x5A, 0xFF, 0x04, 0x5D};
    } __attribute__((packed)) pls916h_packet{};

    memcpy(pls916h_packet.data, data, sizeof(pls916h_packet.data));
    pls916h_packet.checksum = chk8_pls916h(data);

    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    SPI.transferBytes((uint8_t *)&pls916h_packet, NULL, sizeof(pls916h_packet));
    SPI.endTransaction();

    // The chip expects a long low/idle pulse (~3us) on the clock line to
    // latch the frame. Transferring one dummy byte at a much lower clock
    // rate reliably produces that, using the same SPI peripheral/API.
    SPI.beginTransaction(SPISettings(200000, MSBFIRST, SPI_MODE0));
    SPI.transfer(0);
    SPI.endTransaction();
}

uint8_t frame[NUM_LEDS];

void setPixel(size_t index, uint8_t brightness) {
    if (index < NUM_LEDS) {
        frame[index] = brightness;
    }
}

void clearAll() {
    memset(frame, 0, sizeof(frame));
}

// Background twinkle, e.g. stray_stars, that keeps flickering underneath
// whatever the current path animation is doing -- armed with "K", it's
// applied every show() call regardless of who's calling it, since path
// animations don't know about it and shouldn't have to.
static const size_t MAX_BG_TWINKLE = 144;
bool bgTwinkleActive = false;
const uint8_t *bgTwinkleNodes = nullptr;
size_t bgTwinkleCount = 0;
uint8_t bgTwinkleMinB = 0, bgTwinkleMaxB = 0;
float bgTwinkleFreq[MAX_BG_TWINKLE];
float bgTwinklePhase[MAX_BG_TWINKLE];
unsigned long bgTwinkleStartMs = 0;

void armBackgroundTwinkle(const uint8_t *nodes, size_t count, uint8_t minB, uint8_t maxB) {
    if (count > MAX_BG_TWINKLE) count = MAX_BG_TWINKLE;
    bgTwinkleNodes = nodes;
    bgTwinkleCount = count;
    bgTwinkleMinB = minB;
    bgTwinkleMaxB = maxB;
    for (size_t i = 0; i < count; i++) {
        bgTwinkleFreq[i] = 0.04f + (random(0, 1000) / 1000.0f) * 0.10f;
        bgTwinklePhase[i] = (random(0, 6283)) / 1000.0f;
    }
    bgTwinkleStartMs = millis();
    bgTwinkleActive = true;
}

void disarmBackgroundTwinkle() {
    bgTwinkleActive = false;
}

// Smooth ease-in-out curve (t in 0..1 -> eased fraction in 0..1); defined
// further down but needed here too, hence the forward declaration.
float easeInOutSine(float t);

// How long a freshly-armed background twinkle takes to ease up from off to
// its normal range, so re-arming it (e.g. on resuming playback) reads as a
// breath fading in rather than every star popping straight to some random
// mid-brightness phase.
static const float BG_TWINKLE_FADE_IN_SEC = 1.2f;

void show() {
    if (bgTwinkleActive) {
        float tSec = (millis() - bgTwinkleStartMs) / 1000.0f;
        float envelope = 1.0f;
        if (tSec < BG_TWINKLE_FADE_IN_SEC) {
            envelope = easeInOutSine(tSec / BG_TWINKLE_FADE_IN_SEC);
        }
        for (size_t i = 0; i < bgTwinkleCount; i++) {
            float s = sinf(tSec * bgTwinkleFreq[i] * 30.0f + bgTwinklePhase[i]);
            float norm = (s + 1.0f) * 0.5f;
            uint8_t b = (uint8_t)((bgTwinkleMinB + norm * (bgTwinkleMaxB - bgTwinkleMinB)) * envelope);
            setPixel(bgTwinkleNodes[i], b);
        }
    }
    write_pls916h(frame);
}

void lightGroup(const uint8_t *indices, size_t count, uint8_t brightness) {
    clearAll();
    for (size_t i = 0; i < count; i++) {
        setPixel(indices[i], brightness);
    }
    show();
}

// Standard 7-segment truth table, one bit per position in a digit slot
// array ([top, upperLeft, upperRight, middle, lowerLeft, lowerRight,
// bottom], bit 0 = top .. bit 6 = bottom).
static const uint8_t SEVEN_SEG_BITS[10] = {
    119,  // 0: top,upperLeft,upperRight,lowerLeft,lowerRight,bottom
    36,   // 1: upperRight,lowerRight
    93,   // 2: top,upperRight,middle,lowerLeft,bottom
    109,  // 3: top,upperRight,middle,lowerRight,bottom
    46,   // 4: upperLeft,upperRight,middle,lowerRight
    107,  // 5: top,upperLeft,middle,lowerRight,bottom
    123,  // 6: top,upperLeft,middle,lowerLeft,lowerRight,bottom
    37,   // 7: top,upperRight,lowerRight
    127,  // 8: all
    111,  // 9: top,upperLeft,upperRight,middle,lowerRight,bottom
};

// Lights the segments for a single digit (0-9) in a 7-index slot array,
// without clearing -- caller clears first so multiple slots (e.g. all
// four digits of a clock face) can be composed into one frame.
void renderDigit(const uint8_t *segMap, uint8_t digitValue, uint8_t brightness) {
    if (digitValue > 9) return;
    uint8_t bits = SEVEN_SEG_BITS[digitValue];
    for (int i = 0; i < 7; i++) {
        if (bits & (1 << i)) setPixel(segMap[i], brightness);
    }
}

// Animates a traveling light along a path of LED indices: for each
// consecutive pair, the first node ramps from full brightness to off while
// the second ramps from off to full, over `stepsPerTransition` steps. Runs
// entirely on-device (no per-step serial round trips) so the timing is
// smooth and consistent.
void crossfadePath(const uint8_t *path, size_t pathLen, int stepsPerTransition, int stepDelayMs) {
    if (pathLen < 2 || stepsPerTransition < 1) return;
    for (size_t i = 0; i + 1 < pathLen; i++) {
        uint8_t a = path[i];
        uint8_t b = path[i + 1];
        for (int s = 0; s <= stepsPerTransition; s++) {
            uint8_t bBright = (uint8_t)((255 * s) / stepsPerTransition);
            uint8_t aBright = (uint8_t)(255 - bBright);
            clearAll();
            setPixel(a, aBright);
            setPixel(b, bBright);
            show();
            delay(stepDelayMs);
        }
    }
}

// Runs crossfadePath forward then backward along the same path, `repeats`
// times, entirely on-device -- a ping-pong so a short path is easier to
// visually track than a single one-shot pass.
void pingPongPath(const uint8_t *path, size_t pathLen, int stepsPerTransition, int stepDelayMs, int repeats) {
    if (pathLen < 2) return;
    static uint8_t reversed[64];
    size_t n = pathLen < 64 ? pathLen : 64;
    for (size_t i = 0; i < n; i++) {
        reversed[i] = path[n - 1 - i];
    }
    for (int r = 0; r < repeats; r++) {
        crossfadePath(path, pathLen, stepsPerTransition, stepDelayMs);
        crossfadePath(reversed, n, stepsPerTransition, stepDelayMs);
    }
}

// Sweeps a wavefront outward through a branching tree: `nodes[i]` is at
// tree-depth `depths[i]` (edge count from the root). At each depth
// transition every node at the new depth fades in together while every
// node at the previous depth fades out together -- so simultaneous
// branches light up together instead of one branch at a time, one-shot
// (no return sweep).
uint8_t treeMaxDepth(const uint8_t *depths, size_t count) {
    uint8_t maxDepth = 0;
    for (size_t i = 0; i < count; i++) {
        if (depths[i] > maxDepth) maxDepth = depths[i];
    }
    return maxDepth;
}

// Smooth ease-in-out curve (t in 0..1 -> eased fraction in 0..1), so
// brightness ramps read as a breathing curve rather than a linear fade.
float easeInOutSine(float t) {
    return -(cosf((float)M_PI * t) - 1.0f) / 2.0f;
}

// Progress-fill a ring (or any flat, non-branching path): each LED eases
// in and stays lit as we move through the list in order, so it reads as a
// pie-chart-style progress indicator filling clockwise.
void ringFill(const uint8_t *ring, size_t count, int subSteps, int stepDelayMs,
              uint8_t peakBrightness) {
    clearAll();
    for (size_t i = 0; i < count; i++) {
        for (int s = 0; s <= subSteps; s++) {
            uint8_t b = (uint8_t)(peakBrightness * easeInOutSine((float)s / subSteps));
            setPixel(ring[i], b);
            show();
            delay(stepDelayMs);
        }
    }
}

// Radar/second-hand style sweep around a ring: a bright leading LED with
// an exponentially decaying tail trailing behind it, going around `laps`
// times. `decayPercent` is how much brightness the tail keeps per LED
// step back (lower = shorter, snappier tail).
void ringSweep(const uint8_t *ring, size_t count, int stepDelayMs, uint8_t peakBrightness,
               uint8_t decayPercent, int laps) {
    int totalSteps = (int)count * laps;
    float decay = decayPercent / 100.0f;
    for (int step = 0; step < totalSteps; step++) {
        clearAll();
        float b = peakBrightness;
        for (int t = 0; t < (int)count; t++) {
            uint8_t bi = (uint8_t)b;
            if (bi == 0) break;
            int idx = ((step - t) % (int)count + (int)count) % (int)count;
            setPixel(ring[idx], bi);
            b *= decay;
        }
        show();
        delay(stepDelayMs);
    }
    clearAll();
    show();
}

// Instant, non-blocking ring progress set: lights the first `filledCount`
// ring LEDs solid and turns the rest off, in a single frame -- no
// animation, no delay() loop. Only touches the ring's own indices, so
// whatever else is already in frame[] (a clock face, a background
// twinkle) is left alone. Meant to be called repeatedly by a host-side
// timer/clock driving its own timing, since the on-device animation
// commands (I/S/etc.) block the whole board for their duration and can't
// be interrupted -- fine for a few seconds, not for an hour-long timer.
void setRingProgress(const uint8_t *ring, size_t count, size_t filledCount, uint8_t brightness) {
    if (filledCount > count) filledCount = count;
    for (size_t i = 0; i < count; i++) {
        setPixel(ring[i], i < filledCount ? brightness : 0);
    }
    show();
}

// Generic non-destructive pixel set: writes each listed (index, brightness)
// pair into frame[] and shows it, without clearing anything else already
// resident there -- the host-side daemon uses this to compose several
// independent things (a held constellation, clock digits, the X icon) into
// one frame without any of them stomping on the others. Same pattern as
// setRingProgress(), generalized to arbitrary indices instead of just the
// ring.
void setPixels(const uint16_t *indices, const uint8_t *brightnesses, size_t count) {
    for (size_t i = 0; i < count; i++) {
        setPixel(indices[i], brightnesses[i]);
    }
    show();
}

// Eases every LED in a group from fromBrightness to toBrightness over
// totalMs, on-device -- lets a host smoothly dim/undim a group already held
// lit (e.g. a constellation going dim while music is paused, then back up
// on resume) without a full clear-and-replay animation.
void easeGroup(const uint8_t *nodes, size_t count, uint8_t fromB, uint8_t toB, int totalMs) {
    const int kSteps = 30;
    int stepMs = totalMs / kSteps;
    if (stepMs < 1) stepMs = 1;
    for (int s = 0; s <= kSteps; s++) {
        float t = easeInOutSine((float)s / kSteps);
        uint8_t b = (uint8_t)(fromB + t * ((float)toB - (float)fromB));
        for (size_t i = 0; i < count; i++) setPixel(nodes[i], b);
        show();
        delay(stepMs);
    }
}

// Like easeGroup, but each LED fades from whatever brightness it's actually
// currently at (read out of frame[], not a caller-supplied uniform value)
// down/up to toBrightness -- for fading out a group that was mid-animation
// (e.g. each star at a different point in its own twinkle cycle) without
// every LED snapping to a shared starting brightness first.
void easeGroupFromCurrent(const uint8_t *nodes, size_t count, uint8_t toB, int totalMs) {
    static uint8_t fromB[144];
    for (size_t i = 0; i < count && i < 144; i++) {
        fromB[i] = frame[nodes[i]];
    }
    const int kSteps = 30;
    int stepMs = totalMs / kSteps;
    if (stepMs < 1) stepMs = 1;
    for (int s = 0; s <= kSteps; s++) {
        float t = easeInOutSine((float)s / kSteps);
        for (size_t i = 0; i < count && i < 144; i++) {
            uint8_t b = (uint8_t)(fromB[i] + t * ((float)toB - (float)fromB[i]));
            setPixel(nodes[i], b);
        }
        show();
        delay(stepMs);
    }
}

// Hard step blink (no easing, full on/off cut) across a whole group,
// `times` times -- meant to grab attention, e.g. a timer finishing.
void hardBlink(const uint8_t *nodes, size_t count, int times, int onMs, int offMs) {
    for (int i = 0; i < times; i++) {
        for (size_t j = 0; j < count; j++) setPixel(nodes[j], 0xFF);
        show();
        delay(onMs);
        clearAll();
        show();
        delay(offMs);
    }
}

// One directional sweep through depth levels `fromDepth` -> `toDepth`
// (either ascending or descending), scaled to `peakBrightness` instead of
// full 255 so repeated bounces can decay. Does not fade the final level
// out -- caller decides whether this leg's endpoint hands off to another
// sweep (e.g. the reverse leg) or needs its own fade-to-black.
void waveSweep(const uint8_t *nodes, const uint8_t *depths, size_t count,
               int fromDepth, int toDepth, int stepsPerLevel, int stepDelayMs,
               uint8_t peakBrightness) {
    int dir = (toDepth >= fromDepth) ? 1 : -1;
    for (int d = fromDepth; d != toDepth + dir; d += dir) {
        int prev = d - dir;
        for (int s = 0; s <= stepsPerLevel; s++) {
            uint8_t bIn = (uint8_t)(peakBrightness * easeInOutSine((float)s / stepsPerLevel));
            uint8_t bOut = (uint8_t)(peakBrightness - bIn);
            clearAll();
            for (size_t i = 0; i < count; i++) {
                if (depths[i] == d) setPixel(nodes[i], bIn);
                else if (depths[i] == prev) setPixel(nodes[i], bOut);
            }
            show();
            delay(stepDelayMs);
        }
    }
}

// Like waveSweep, but depths the wavefront has already passed through stay
// lit at peakBrightness instead of dimming back out -- so by the time the
// sweep reaches the far end, the whole tree is already lit, with no
// separate "snap to full brightness" step needed afterward.
void waveSweepHold(const uint8_t *nodes, const uint8_t *depths, size_t count,
                    int fromDepth, int toDepth, int stepsPerLevel, int stepDelayMs,
                    uint8_t peakBrightness) {
    int dir = (toDepth >= fromDepth) ? 1 : -1;
    for (int d = fromDepth; d != toDepth + dir; d += dir) {
        for (int s = 0; s <= stepsPerLevel; s++) {
            uint8_t bIn = (uint8_t)(peakBrightness * easeInOutSine((float)s / stepsPerLevel));
            for (size_t i = 0; i < count; i++) {
                int dd = depths[i];
                bool alreadyPassed = (dir == 1) ? (dd < d) : (dd > d);
                if (dd == d) setPixel(nodes[i], bIn);
                else if (alreadyPassed) setPixel(nodes[i], peakBrightness);
            }
            show();
            delay(stepDelayMs);
        }
    }
}

// Mirror of waveSweepHold: assumes every node is already lit at
// peakBrightness (e.g. right after a waveSweepHold/flash), and sweeps
// through extinguishing depths one at a time as it passes -- the current
// depth fades peakBrightness->0, already-passed depths are forced off,
// and not-yet-reached depths are left untouched (still lit from before).
void waveSweepUnhold(const uint8_t *nodes, const uint8_t *depths, size_t count,
                      int fromDepth, int toDepth, int stepsPerLevel, int stepDelayMs,
                      uint8_t peakBrightness) {
    int dir = (toDepth >= fromDepth) ? 1 : -1;
    for (int d = fromDepth; d != toDepth + dir; d += dir) {
        for (int s = 0; s <= stepsPerLevel; s++) {
            uint8_t bOut = (uint8_t)(peakBrightness * (1.0f - easeInOutSine((float)s / stepsPerLevel)));
            for (size_t i = 0; i < count; i++) {
                int dd = depths[i];
                bool alreadyPassed = (dir == 1) ? (dd < d) : (dd > d);
                if (dd == d) setPixel(nodes[i], bOut);
                else if (alreadyPassed) setPixel(nodes[i], 0);
            }
            show();
            delay(stepDelayMs);
        }
    }
}

// Smoothly ramps every node in the group from peakBrightness down to 0 over
// `downMs`, then back up to peakBrightness over `upMs`, both eased -- a
// full breathing cycle rather than an instant on/off cut.
void breathePulse(const uint8_t *nodes, size_t count, int downMs, int upMs,
                   uint8_t peakBrightness) {
    static const int kSteps = 30;
    int downStepMs = downMs / kSteps;
    if (downStepMs < 1) downStepMs = 1;
    int upStepMs = upMs / kSteps;
    if (upStepMs < 1) upStepMs = 1;

    for (int s = 0; s <= kSteps; s++) {
        uint8_t b = (uint8_t)(peakBrightness * (1.0f - easeInOutSine((float)s / kSteps)));
        for (size_t i = 0; i < count; i++) setPixel(nodes[i], b);
        show();
        delay(downStepMs);
    }
    for (int s = 0; s <= kSteps; s++) {
        uint8_t b = (uint8_t)(peakBrightness * easeInOutSine((float)s / kSteps));
        for (size_t i = 0; i < count; i++) setPixel(nodes[i], b);
        show();
        delay(upStepMs);
    }
}

// Full trace-and-pulse sequence: reveal root->tip holding as it goes, flash
// the fully-lit tree twice (each flash a full breathing dip, not an instant
// cut), sweep root->tip again extinguishing depths one at a time, then
// sweep back tip->root holding as it relights -- ending fully lit, like a
// breath tracing the path out and back.
void traceBreathe(const uint8_t *nodes, const uint8_t *depths, size_t count,
                   int stepsPerLevel, int stepDelayMs, int flashDownMs, int flashUpMs,
                   uint8_t peakBrightness) {
    if (count == 0) return;
    uint8_t maxDepth = treeMaxDepth(depths, count);

    clearAll();
    show();
    waveSweepHold(nodes, depths, count, 0, maxDepth, stepsPerLevel, stepDelayMs, peakBrightness);

    for (int i = 0; i < 2; i++) {
        breathePulse(nodes, count, flashDownMs, flashUpMs, peakBrightness);
    }

    waveSweepUnhold(nodes, depths, count, 0, maxDepth, stepsPerLevel, stepDelayMs, peakBrightness);
    waveSweepHold(nodes, depths, count, maxDepth, 0, stepsPerLevel, stepDelayMs, peakBrightness);
}

void fadeLevelOut(const uint8_t *nodes, const uint8_t *depths, size_t count,
                   int depth, int stepsPerLevel, int stepDelayMs, uint8_t peakBrightness) {
    for (int s = 0; s <= stepsPerLevel; s++) {
        uint8_t bOut = (uint8_t)(peakBrightness * (1.0f - easeInOutSine((float)s / stepsPerLevel)));
        clearAll();
        for (size_t i = 0; i < count; i++) {
            if (depths[i] == depth) setPixel(nodes[i], bOut);
        }
        show();
        delay(stepDelayMs);
    }
}

// Sweeps outward from the root exactly like wavePath, but never dims
// anything back down -- each depth fades in and then just stays lit, so the
// whole tree progressively reveals itself and ends fully glowing rather
// than fading away.
void revealAndHold(const uint8_t *nodes, const uint8_t *depths, size_t count,
                    int stepsPerLevel, int stepDelayMs, uint8_t peakBrightness = 0xFF) {
    if (count == 0) return;
    clearAll();
    uint8_t maxDepth = treeMaxDepth(depths, count);
    for (int d = 0; d <= maxDepth; d++) {
        for (int s = 0; s <= stepsPerLevel; s++) {
            uint8_t bIn = (uint8_t)(peakBrightness * easeInOutSine((float)s / stepsPerLevel));
            for (size_t i = 0; i < count; i++) {
                if (depths[i] == d) setPixel(nodes[i], bIn);
            }
            show();
            delay(stepDelayMs);
        }
    }
}

// Independent random twinkle: each LED gets its own random frequency and
// phase, so brightness drifts up and down smoothly and asynchronously --
// no traveling direction, no shared timing, just a night-sky flicker.
static const size_t MAX_TWINKLE = 144;
void twinkle(const uint8_t *nodes, size_t count, int durationMs, int frameDelayMs,
             uint8_t minBrightness, uint8_t maxBrightness) {
    if (count == 0) return;
    if (count > MAX_TWINKLE) count = MAX_TWINKLE;

    static float freq[MAX_TWINKLE];
    static float phase[MAX_TWINKLE];
    for (size_t i = 0; i < count; i++) {
        freq[i] = 0.04f + (random(0, 1000) / 1000.0f) * 0.10f;  // ~0.04-0.14 rad/frame
        phase[i] = (random(0, 6283)) / 1000.0f;                  // 0-2pi
    }

    int totalFrames = durationMs / frameDelayMs;
    for (int f = 0; f < totalFrames; f++) {
        clearAll();
        for (size_t i = 0; i < count; i++) {
            float s = sinf(f * freq[i] + phase[i]);       // -1..1
            float norm = (s + 1.0f) * 0.5f;                // 0..1
            uint8_t b = (uint8_t)(minBrightness + norm * (maxBrightness - minBrightness));
            setPixel(nodes[i], b);
        }
        show();
        delay(frameDelayMs);
    }
    clearAll();
    show();
}

// Same as twinkle, but a second group is held static at staticBrightness
// for the whole duration instead of being cleared each frame -- lets a
// constellation stay lit solidly while the stray stars twinkle around it.
void twinkleWithStatic(const uint8_t *nodes, size_t count, int durationMs, int frameDelayMs,
                        uint8_t minBrightness, uint8_t maxBrightness,
                        const uint8_t *staticNodes, size_t staticCount, uint8_t staticBrightness) {
    if (count == 0) return;
    if (count > MAX_TWINKLE) count = MAX_TWINKLE;

    static float freq[MAX_TWINKLE];
    static float phase[MAX_TWINKLE];
    for (size_t i = 0; i < count; i++) {
        freq[i] = 0.04f + (random(0, 1000) / 1000.0f) * 0.10f;
        phase[i] = (random(0, 6283)) / 1000.0f;
    }

    int totalFrames = durationMs / frameDelayMs;
    for (int f = 0; f < totalFrames; f++) {
        clearAll();
        for (size_t i = 0; i < staticCount; i++) setPixel(staticNodes[i], staticBrightness);
        for (size_t i = 0; i < count; i++) {
            float s = sinf(f * freq[i] + phase[i]);
            float norm = (s + 1.0f) * 0.5f;
            uint8_t b = (uint8_t)(minBrightness + norm * (maxBrightness - minBrightness));
            setPixel(nodes[i], b);
        }
        show();
        delay(frameDelayMs);
    }
    clearAll();
    show();
}

// Wavefront sweep from root to the deepest tip, one-shot (see wavePingPongTree
// for the bouncing version).
void wavePath(const uint8_t *nodes, const uint8_t *depths, size_t count,
              int stepsPerLevel, int stepDelayMs, uint8_t peakBrightness = 0xFF) {
    if (count == 0) return;
    uint8_t maxDepth = treeMaxDepth(depths, count);
    waveSweep(nodes, depths, count, 0, maxDepth, stepsPerLevel, stepDelayMs, peakBrightness);
    fadeLevelOut(nodes, depths, count, maxDepth, stepsPerLevel, stepDelayMs, peakBrightness);
}

// Bounces a wavefront out to the deepest tip and back to the root,
// `bounces` times, losing `decayPerBounce` brightness each round trip so
// the motion settles down on its own rather than needing a hardcoded step
// count. Short branches don't ping back independently -- they rejoin the
// shared sweep when it passes back through their (shallower) depth level.
// The final return-to-root leg holds each depth lit as it passes (see
// waveSweepHold) instead of dimming behind itself, so the tree fills in
// progressively during that last pass and ends fully lit right as the
// wavefront reaches the root -- no separate "snap to full brightness" step.
void wavePingPongTree(const uint8_t *nodes, const uint8_t *depths, size_t count,
                       int stepsPerLevel, int stepDelayMs, int bounces, uint8_t decayPerBounce,
                       uint8_t peakBrightness = 0xFF) {
    if (count == 0) return;
    uint8_t maxDepth = treeMaxDepth(depths, count);
    int peak = peakBrightness;

    for (int b = 0; b < bounces && peak > 0; b++) {
        bool isLastBounce = (b == bounces - 1) || (peak - (int)decayPerBounce <= 0);
        waveSweep(nodes, depths, count, 0, maxDepth, stepsPerLevel, stepDelayMs, (uint8_t)peak);
        if (isLastBounce) {
            waveSweepHold(nodes, depths, count, maxDepth, 0, stepsPerLevel, stepDelayMs,
                          peakBrightness);
        } else {
            waveSweep(nodes, depths, count, maxDepth, 0, stepsPerLevel, stepDelayMs, (uint8_t)peak);
        }
        peak -= decayPerBounce;
    }
}

// Independent traveling-pulse simulation over a graph's edge list. Each
// pulse moves along one edge at a time; on arrival at a node it either
// bounces back the way it came (leaf, degree 1), continues straight through
// (degree 2, not a real junction), or forks into a new pulse per remaining
// edge (degree >=3 junction) -- unless another pulse also arrives at that
// same node on the same tick, in which case it's a collision: both pulses
// just reflect back the way they came instead of forking, so the pulse
// count can't run away. Brightness decays a bit on every bounce/fork so the
// whole thing fades out and stops on its own.
static const int MAX_PULSES = 24;
struct Pulse {
    uint8_t from, to;
    float t;
    uint8_t brightness;
    bool alive;
};

size_t edgeNeighborsExcluding(const uint8_t edges[][2], size_t edgeCount,
                               uint8_t node, uint8_t exclude, uint8_t *out, size_t outCap) {
    size_t n = 0;
    for (size_t i = 0; i < edgeCount && n < outCap; i++) {
        uint8_t a = edges[i][0], b = edges[i][1];
        if (a == node && b != exclude) out[n++] = b;
        else if (b == node && a != exclude) out[n++] = a;
    }
    return n;
}

void pulseBounce(const uint8_t edges[][2], size_t edgeCount, uint8_t startNode,
                  int stepsPerEdge, int stepDelayMs, uint8_t decayPercent, uint8_t minBrightness,
                  int maxTicks, uint8_t startBrightness = 0xFF) {
    uint8_t first[8];
    size_t firstCount = edgeNeighborsExcluding(edges, edgeCount, startNode, startNode, first, 8);
    if (firstCount == 0) return;

    static Pulse pulses[MAX_PULSES];
    memset(pulses, 0, sizeof(pulses));
    pulses[0] = {startNode, first[0], 0.0f, startBrightness, true};
    int aliveCount = 1;

    float dt = 1.0f / stepsPerEdge;

    for (int tick = 0; tick < maxTicks && aliveCount > 0; tick++) {
        // Advance all pulses.
        for (int i = 0; i < MAX_PULSES; i++) {
            if (pulses[i].alive) pulses[i].t += dt;
        }

        // Find which nodes have an arrival this tick, and how many.
        uint8_t arrivalNode[MAX_PULSES];
        int arrivalPulse[MAX_PULSES];
        int arrivalCount = 0;
        for (int i = 0; i < MAX_PULSES; i++) {
            if (pulses[i].alive && pulses[i].t >= 1.0f) {
                arrivalNode[arrivalCount] = pulses[i].to;
                arrivalPulse[arrivalCount] = i;
                arrivalCount++;
            }
        }

        for (int a = 0; a < arrivalCount; a++) {
            int i = arrivalPulse[a];
            if (!pulses[i].alive) continue;  // already consumed by a collision below

            uint8_t node = pulses[i].to;
            uint8_t cameFrom = pulses[i].from;
            uint8_t newBrightness = (uint8_t)(((uint16_t)pulses[i].brightness * decayPercent) / 100);

            // Collision: does another still-alive arrival land on this same
            // node this tick?
            bool collision = false;
            for (int b = a + 1; b < arrivalCount; b++) {
                int j = arrivalPulse[b];
                if (pulses[j].alive && arrivalNode[b] == node) {
                    collision = true;
                    uint8_t jNewBrightness = (uint8_t)(((uint16_t)pulses[j].brightness * decayPercent) / 100);
                    if (jNewBrightness >= minBrightness) {
                        // Reflect j back the way it came.
                        uint8_t jCameFrom = pulses[j].from;
                        pulses[j].from = pulses[j].to;
                        pulses[j].to = jCameFrom;
                        pulses[j].t = 0.0f;
                        pulses[j].brightness = jNewBrightness;
                    } else {
                        pulses[j].alive = false;
                        aliveCount--;
                    }
                }
            }

            if (collision || newBrightness < minBrightness) {
                if (newBrightness < minBrightness) {
                    pulses[i].alive = false;
                    aliveCount--;
                } else {
                    // Reflect this pulse back the way it came.
                    pulses[i].from = node;
                    pulses[i].to = cameFrom;
                    pulses[i].t = 0.0f;
                    pulses[i].brightness = newBrightness;
                }
                continue;
            }

            uint8_t next[8];
            size_t nextCount = edgeNeighborsExcluding(edges, edgeCount, node, cameFrom, next, 8);

            if (nextCount == 0) {
                // Leaf: bounce straight back.
                pulses[i].from = node;
                pulses[i].to = cameFrom;
                pulses[i].t = 0.0f;
                pulses[i].brightness = newBrightness;
            } else if (nextCount == 1) {
                // Pass-through node: keep going, no fork, no decay beyond
                // the arrival decay already applied.
                pulses[i].from = node;
                pulses[i].to = next[0];
                pulses[i].t = 0.0f;
                pulses[i].brightness = newBrightness;
            } else {
                // Junction: fork into a new pulse per remaining edge.
                pulses[i].from = node;
                pulses[i].to = next[0];
                pulses[i].t = 0.0f;
                pulses[i].brightness = newBrightness;
                for (size_t k = 1; k < nextCount; k++) {
                    for (int slot = 0; slot < MAX_PULSES; slot++) {
                        if (!pulses[slot].alive) {
                            pulses[slot] = {node, next[k], 0.0f, newBrightness, true};
                            aliveCount++;
                            break;
                        }
                    }
                }
            }
        }

        // Render: max brightness contribution per node across all pulses.
        clearAll();
        for (int i = 0; i < MAX_PULSES; i++) {
            if (!pulses[i].alive) continue;
            float t = pulses[i].t > 1.0f ? 1.0f : pulses[i].t;
            uint8_t bTo = (uint8_t)(pulses[i].brightness * t);
            uint8_t bFrom = (uint8_t)(pulses[i].brightness * (1.0f - t));
            if (bFrom > frame[pulses[i].from]) setPixel(pulses[i].from, bFrom);
            if (bTo > frame[pulses[i].to]) setPixel(pulses[i].to, bTo);
        }
        show();
        delay(stepDelayMs);
    }
    clearAll();
    show();
}

// Splits a space-separated string of ints starting at `str`, calling
// `fn(value)` for each one.
template <typename Fn>
void forEachInt(const String &str, Fn fn) {
    int start = 0;
    while (start < (int)str.length()) {
        int sp = str.indexOf(' ', start);
        String tok = (sp == -1) ? str.substring(start) : str.substring(start, sp);
        if (tok.length() > 0) {
            fn(tok.toInt());
        }
        if (sp == -1) break;
        start = sp + 1;
    }
}

// Host-controlled mode. Protocol (one line at a time over serial, newline
// terminated):
//   "L <index>"                 -> clear all, light only <index> at full
//                                   brightness
//   "F <b> <i1> <i2> ..."       -> clear all, light every listed index at
//                                   brightness <b> (0-255)
//   "G <b> <group>"             -> clear all, light a named group (e.g.
//                                   "constellation_left") at brightness <b>
//   "P <steps> <ms> <path...>"  -> on-device crossfade along a path of
//                                   indices, one-shot
//   "X <n> <steps> <ms> <path...>" -> ping-pong crossfadePath along a path,
//                                   forward+backward, repeated <n> times
//   "W <steps> <ms> <peakB> <group>" -> on-device wavefront sweep through a
//                                   named branching-tree group, one-shot,
//                                   peaking at brightness <peakB>
//   "R <steps> <ms> <peakB> <group>" -> reveal and hold: same sweep, but each
//                                   depth stays lit instead of fading out,
//                                   ending with the whole tree glowing at
//                                   <peakB>
//   "T <ms> <frameMs> <minB> <maxB> <group>" -> independent random twinkle
//                                   per LED for <ms> total duration
//   "Y <bounces> <decay> <steps> <ms> <peakB> <group>" -> wavefront bounces
//                                   root<->tip `bounces` times, losing
//                                   `decay` brightness per round trip; the
//                                   final return-to-root leg holds each
//                                   depth lit as it passes instead of
//                                   fading, ending with the whole tree lit
//                                   at <peakB>
//   "Z <steps> <ms> <peakB> <group>" -> independent pulse-bounce simulation:
//                                   each branch bounces on its own,
//                                   junctions fork, colliding pulses
//                                   reflect instead of forking, decays and
//                                   stops on its own, starting at <peakB>
//   "B <steps> <ms> <flashDownMs> <flashUpMs> <peakB> <group>" -> trace and
//                                   pulse: reveal root->tip holding as it
//                                   goes, breathe the fully-lit tree down
//                                   to 0 and back up twice (each dip taking
//                                   <flashDownMs> down + <flashUpMs> up,
//                                   eased -- not an instant cut), sweep
//                                   root->tip extinguishing depths one at a
//                                   time, then sweep back tip->root holding
//                                   as it relights -- ends fully lit
//   "N <ms> <frameMs> <minB> <maxB> <staticB> <group>" -> stray_stars
//                                   twinkle while <group> ("constellation_left",
//                                   "constellation_right", or
//                                   "both_constellations") stays lit
//                                   solidly at <staticB>
//   "I <subSteps> <ms> <brightness>" -> ring progress-fill, clockwise from
//                                   LED 30, single pass, each LED eases in
//                                   and stays lit
//   "S <ms> <brightness> <decayPercent> <laps>" -> ring radar/second-hand
//                                   sweep, a bright LED with a decaying
//                                   tail going around <laps> times
//   "J <filledCount> <brightness>" -> instant, non-blocking ring progress
//                                   set (0-14 filled), for a host-driven
//                                   timer ticking its own pace
//   "H <times> <onMs> <offMs> <group>" -> hard step blink (no easing,
//                                   full on/off cut), <group> "all" or
//                                   "ring" -- e.g. a timer finishing
//   "D <brightness> <topA> <topB> <bottomA> <bottomB>" -> render a digit
//                                   0-9 (or "X" for blank) in each of the
//                                   four 7-segment slots at once, like a
//                                   clock face reading topA topB : bottomA
//                                   bottomB
//   "K <minB> <maxB> <group>"   -> arm a background twinkle ("stray_stars"
//                                   or "all") that keeps flickering under
//                                   whatever W/R/Y/Z/B animation runs next
//   "K off"                     -> disarm the background twinkle
//   "E <fromB> <toB> <ms> <group>" -> ease every LED in a group from fromB
//                                   to toB over ms, on-device, without
//                                   clearing anything else already lit --
//                                   for smoothly dimming/undimming a held
//                                   group (e.g. a constellation on pause/
//                                   resume) instead of an instant step
//   "V <toB> <ms> <group>"      -> like "E", but each LED fades from its
//                                   own current brightness (not a shared
//                                   starting value) to toB -- for fading
//                                   out a group that was mid-animation
//                                   (e.g. twinkling stars) without every
//                                   LED snapping to one brightness first
//   "U <i1> <b1> <i2> <b2> ..." -> non-destructive pixel set: writes each
//                                   (index, brightness) pair without
//                                   clearing anything else already lit --
//                                   for host-side compositing of several
//                                   independent layers at once
//   "C"                         -> clear all
//   "A"                         -> light all LEDs
// After handling a command the firmware prints "OK" so a host script can
// wait for acknowledgement before it snaps a photo / moves on.
void loop() {
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();

        if (line.startsWith("L ")) {
            long idx = line.substring(2).toInt();
            clearAll();
            setPixel((size_t)idx, 0xFF);
            show();
            Serial.println("OK");
        } else if (line.startsWith("F ")) {
            String rest = line.substring(2);
            rest.trim();
            int sp = rest.indexOf(' ');
            uint8_t brightness = (sp == -1) ? 0xFF : (uint8_t)rest.substring(0, sp).toInt();
            String indicesStr = (sp == -1) ? "" : rest.substring(sp + 1);
            clearAll();
            forEachInt(indicesStr, [&](long v) { setPixel((size_t)v, brightness); });
            show();
            Serial.println("OK");
        } else if (line.startsWith("G ")) {
            String rest = line.substring(2);
            rest.trim();
            int sp = rest.indexOf(' ');
            uint8_t brightness = (sp == -1) ? 0xFF : (uint8_t)rest.substring(0, sp).toInt();
            String groupName = (sp == -1) ? rest : rest.substring(sp + 1);
            if (groupName == "constellation_left") {
                lightGroup(CONSTELLATION_LEFT, CONSTELLATION_LEFT_LEN, brightness);
            } else if (groupName == "constellation_right") {
                lightGroup(CONSTELLATION_RIGHT, CONSTELLATION_RIGHT_LEN, brightness);
            } else if (groupName == "both_constellations") {
                lightGroup(BOTH_CONSTELLATIONS, BOTH_CONSTELLATIONS_LEN, brightness);
            } else if (groupName == "x_icon") {
                lightGroup(X_ICON, X_ICON_LEN, brightness);
            }
            Serial.println("OK");
        } else if (line.startsWith("P ")) {
            // "P <stepsPerTransition> <stepDelayMs> <i1> <i2> ..."
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int steps = rest.substring(0, sp1).toInt();
            String afterSteps = rest.substring(sp1 + 1);
            int sp2 = afterSteps.indexOf(' ');
            int delayMs = afterSteps.substring(0, sp2).toInt();
            String pathStr = afterSteps.substring(sp2 + 1);

            static uint8_t path[64];
            size_t pathLen = 0;
            forEachInt(pathStr, [&](long v) {
                if (pathLen < 64) path[pathLen++] = (uint8_t)v;
            });
            crossfadePath(path, pathLen, steps, delayMs);
            Serial.println("OK");
        } else if (line.startsWith("X ")) {
            // "X <repeats> <stepsPerTransition> <stepDelayMs> <i1> <i2> ..."
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int repeats = rest.substring(0, sp1).toInt();
            String afterRepeats = rest.substring(sp1 + 1);
            int sp2 = afterRepeats.indexOf(' ');
            int steps = afterRepeats.substring(0, sp2).toInt();
            String afterSteps = afterRepeats.substring(sp2 + 1);
            int sp3 = afterSteps.indexOf(' ');
            int delayMs = afterSteps.substring(0, sp3).toInt();
            String pathStr = afterSteps.substring(sp3 + 1);

            static uint8_t path[64];
            size_t pathLen = 0;
            forEachInt(pathStr, [&](long v) {
                if (pathLen < 64) path[pathLen++] = (uint8_t)v;
            });
            pingPongPath(path, pathLen, steps, delayMs, repeats);
            Serial.println("OK");
        } else if (line.startsWith("W ")) {
            // "W <stepsPerLevel> <stepDelayMs> <peakBrightness> <group>"
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int steps = rest.substring(0, sp1).toInt();
            String afterSteps = rest.substring(sp1 + 1);
            int sp2 = afterSteps.indexOf(' ');
            int delayMs = afterSteps.substring(0, sp2).toInt();
            String r3 = afterSteps.substring(sp2 + 1);
            int sp3 = r3.indexOf(' ');
            int peakBrightness = r3.substring(0, sp3).toInt();
            String groupName = r3.substring(sp3 + 1);
            if (groupName == "constellation_left") {
                wavePath(CONSTELLATION_LEFT_TREE, CONSTELLATION_LEFT_DEPTH,
                         CONSTELLATION_LEFT_TREE_LEN, steps, delayMs, (uint8_t)peakBrightness);
            } else if (groupName == "constellation_right") {
                wavePath(CONSTELLATION_RIGHT_TREE, CONSTELLATION_RIGHT_DEPTH,
                         CONSTELLATION_RIGHT_TREE_LEN, steps, delayMs, (uint8_t)peakBrightness);
            } else if (groupName == "both_constellations") {
                wavePath(BOTH_CONSTELLATIONS_TREE, BOTH_CONSTELLATIONS_DEPTH,
                         BOTH_CONSTELLATIONS_TREE_LEN, steps, delayMs, (uint8_t)peakBrightness);
            }
            Serial.println("OK");
        } else if (line.startsWith("T ")) {
            // "T <durationMs> <frameDelayMs> <minB> <maxB> <group>"
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int durationMs = rest.substring(0, sp1).toInt();
            String r2 = rest.substring(sp1 + 1);
            int sp2 = r2.indexOf(' ');
            int frameDelayMs = r2.substring(0, sp2).toInt();
            String r3 = r2.substring(sp2 + 1);
            int sp3 = r3.indexOf(' ');
            int minB = r3.substring(0, sp3).toInt();
            String r4 = r3.substring(sp3 + 1);
            int sp4 = r4.indexOf(' ');
            int maxB = r4.substring(0, sp4).toInt();
            String groupName = r4.substring(sp4 + 1);
            if (groupName == "constellation_left") {
                twinkle(CONSTELLATION_LEFT, CONSTELLATION_LEFT_LEN, durationMs, frameDelayMs,
                        (uint8_t)minB, (uint8_t)maxB);
            } else if (groupName == "constellation_right") {
                twinkle(CONSTELLATION_RIGHT, CONSTELLATION_RIGHT_LEN, durationMs, frameDelayMs,
                        (uint8_t)minB, (uint8_t)maxB);
            } else if (groupName == "stray_stars") {
                twinkle(STRAY_STARS, STRAY_STARS_LEN, durationMs, frameDelayMs,
                        (uint8_t)minB, (uint8_t)maxB);
            } else if (groupName == "all") {
                twinkle(ALL_LEDS, ALL_LEDS_LEN, durationMs, frameDelayMs,
                        (uint8_t)minB, (uint8_t)maxB);
            }
            Serial.println("OK");
        } else if (line.startsWith("R ")) {
            // "R <steps> <ms> <peakBrightness> <group>" -- reveal and hold (no fade-out)
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int steps = rest.substring(0, sp1).toInt();
            String afterSteps = rest.substring(sp1 + 1);
            int sp2 = afterSteps.indexOf(' ');
            int delayMs = afterSteps.substring(0, sp2).toInt();
            String r3 = afterSteps.substring(sp2 + 1);
            int sp3 = r3.indexOf(' ');
            int peakBrightness = r3.substring(0, sp3).toInt();
            String groupName = r3.substring(sp3 + 1);
            if (groupName == "constellation_left") {
                revealAndHold(CONSTELLATION_LEFT_TREE, CONSTELLATION_LEFT_DEPTH,
                               CONSTELLATION_LEFT_TREE_LEN, steps, delayMs, (uint8_t)peakBrightness);
            } else if (groupName == "constellation_right") {
                revealAndHold(CONSTELLATION_RIGHT_TREE, CONSTELLATION_RIGHT_DEPTH,
                               CONSTELLATION_RIGHT_TREE_LEN, steps, delayMs, (uint8_t)peakBrightness);
            } else if (groupName == "both_constellations") {
                revealAndHold(BOTH_CONSTELLATIONS_TREE, BOTH_CONSTELLATIONS_DEPTH,
                               BOTH_CONSTELLATIONS_TREE_LEN, steps, delayMs, (uint8_t)peakBrightness);
            }
            Serial.println("OK");
        } else if (line.startsWith("Y ")) {
            // "Y <bounces> <decayPerBounce> <stepsPerLevel> <ms> <peakBrightness> <group>"
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int bounces = rest.substring(0, sp1).toInt();
            String r2 = rest.substring(sp1 + 1);
            int sp2 = r2.indexOf(' ');
            int decay = r2.substring(0, sp2).toInt();
            String r3 = r2.substring(sp2 + 1);
            int sp3 = r3.indexOf(' ');
            int steps = r3.substring(0, sp3).toInt();
            String r4 = r3.substring(sp3 + 1);
            int sp4 = r4.indexOf(' ');
            int delayMs = r4.substring(0, sp4).toInt();
            String r5 = r4.substring(sp4 + 1);
            int sp5 = r5.indexOf(' ');
            int peakBrightness = r5.substring(0, sp5).toInt();
            String groupName = r5.substring(sp5 + 1);
            if (groupName == "constellation_left") {
                wavePingPongTree(CONSTELLATION_LEFT_TREE, CONSTELLATION_LEFT_DEPTH,
                                  CONSTELLATION_LEFT_TREE_LEN, steps, delayMs,
                                  bounces, (uint8_t)decay, (uint8_t)peakBrightness);
            } else if (groupName == "constellation_right") {
                wavePingPongTree(CONSTELLATION_RIGHT_TREE, CONSTELLATION_RIGHT_DEPTH,
                                  CONSTELLATION_RIGHT_TREE_LEN, steps, delayMs,
                                  bounces, (uint8_t)decay, (uint8_t)peakBrightness);
            } else if (groupName == "both_constellations") {
                wavePingPongTree(BOTH_CONSTELLATIONS_TREE, BOTH_CONSTELLATIONS_DEPTH,
                                  BOTH_CONSTELLATIONS_TREE_LEN, steps, delayMs,
                                  bounces, (uint8_t)decay, (uint8_t)peakBrightness);
            }
            Serial.println("OK");
        } else if (line.startsWith("Z ")) {
            // "Z <stepsPerEdge> <ms> <peakBrightness> <group>"
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int steps = rest.substring(0, sp1).toInt();
            String r2 = rest.substring(sp1 + 1);
            int sp2 = r2.indexOf(' ');
            int delayMs = r2.substring(0, sp2).toInt();
            String r3 = r2.substring(sp2 + 1);
            int sp3 = r3.indexOf(' ');
            int peakBrightness = r3.substring(0, sp3).toInt();
            String groupName = r3.substring(sp3 + 1);
            if (groupName == "constellation_left") {
                pulseBounce(CONSTELLATION_LEFT_EDGES, CONSTELLATION_LEFT_EDGES_LEN,
                            /*startNode=*/72, steps, delayMs,
                            /*decayPercent=*/82, /*minBrightness=*/18, /*maxTicks=*/3000,
                            (uint8_t)peakBrightness);
            } else if (groupName == "constellation_right") {
                pulseBounce(CONSTELLATION_RIGHT_EDGES, CONSTELLATION_RIGHT_EDGES_LEN,
                            /*startNode=*/125, steps, delayMs,
                            /*decayPercent=*/82, /*minBrightness=*/18, /*maxTicks=*/3000,
                            (uint8_t)peakBrightness);
            }
            Serial.println("OK");
        } else if (line.startsWith("B ")) {
            // "B <steps> <ms> <flashOnMs> <flashOffMs> <peakBrightness> <group>"
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int steps = rest.substring(0, sp1).toInt();
            String r2 = rest.substring(sp1 + 1);
            int sp2 = r2.indexOf(' ');
            int delayMs = r2.substring(0, sp2).toInt();
            String r3 = r2.substring(sp2 + 1);
            int sp3 = r3.indexOf(' ');
            int flashOnMs = r3.substring(0, sp3).toInt();
            String r4 = r3.substring(sp3 + 1);
            int sp4 = r4.indexOf(' ');
            int flashOffMs = r4.substring(0, sp4).toInt();
            String r5 = r4.substring(sp4 + 1);
            int sp5 = r5.indexOf(' ');
            int peakBrightness = r5.substring(0, sp5).toInt();
            String groupName = r5.substring(sp5 + 1);
            if (groupName == "constellation_left") {
                traceBreathe(CONSTELLATION_LEFT_TREE, CONSTELLATION_LEFT_DEPTH,
                             CONSTELLATION_LEFT_TREE_LEN, steps, delayMs, flashOnMs, flashOffMs,
                             (uint8_t)peakBrightness);
            } else if (groupName == "constellation_right") {
                traceBreathe(CONSTELLATION_RIGHT_TREE, CONSTELLATION_RIGHT_DEPTH,
                             CONSTELLATION_RIGHT_TREE_LEN, steps, delayMs, flashOnMs, flashOffMs,
                             (uint8_t)peakBrightness);
            } else if (groupName == "both_constellations") {
                traceBreathe(BOTH_CONSTELLATIONS_TREE, BOTH_CONSTELLATIONS_DEPTH,
                             BOTH_CONSTELLATIONS_TREE_LEN, steps, delayMs, flashOnMs, flashOffMs,
                             (uint8_t)peakBrightness);
            }
            Serial.println("OK");
        } else if (line.startsWith("I ")) {
            // "I <subSteps> <ms> <brightness>" -- ring progress-fill,
            // clockwise, single pass
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int subSteps = rest.substring(0, sp1).toInt();
            String r2 = rest.substring(sp1 + 1);
            int sp2 = r2.indexOf(' ');
            int delayMs = r2.substring(0, sp2).toInt();
            int brightness = r2.substring(sp2 + 1).toInt();
            ringFill(RING, RING_LEN, subSteps, delayMs, (uint8_t)brightness);
            Serial.println("OK");
        } else if (line.startsWith("S ")) {
            // "S <ms> <brightness> <decayPercent> <laps>" -- ring
            // radar/second-hand sweep with a decaying tail
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int delayMs = rest.substring(0, sp1).toInt();
            String r2 = rest.substring(sp1 + 1);
            int sp2 = r2.indexOf(' ');
            int brightness = r2.substring(0, sp2).toInt();
            String r3 = r2.substring(sp2 + 1);
            int sp3 = r3.indexOf(' ');
            int decayPercent = r3.substring(0, sp3).toInt();
            int laps = r3.substring(sp3 + 1).toInt();
            ringSweep(RING, RING_LEN, delayMs, (uint8_t)brightness, (uint8_t)decayPercent, laps);
            Serial.println("OK");
        } else if (line.startsWith("J ")) {
            // "J <filledCount> <brightness>" -- instant, non-blocking ring
            // progress set (0-14 filled), for host-driven timers
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int filledCount = rest.substring(0, sp1).toInt();
            int brightness = rest.substring(sp1 + 1).toInt();
            setRingProgress(RING, RING_LEN, (size_t)filledCount, (uint8_t)brightness);
            Serial.println("OK");
        } else if (line.startsWith("H ")) {
            // "H <times> <onMs> <offMs> <group>" -- hard step blink (no
            // easing) to grab attention, e.g. a timer finishing
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int times = rest.substring(0, sp1).toInt();
            String r2 = rest.substring(sp1 + 1);
            int sp2 = r2.indexOf(' ');
            int onMs = r2.substring(0, sp2).toInt();
            String r3 = r2.substring(sp2 + 1);
            int sp3 = r3.indexOf(' ');
            int offMs = r3.substring(0, sp3).toInt();
            String groupName = r3.substring(sp3 + 1);
            if (groupName == "all") {
                hardBlink(ALL_LEDS, ALL_LEDS_LEN, times, onMs, offMs);
            } else if (groupName == "ring") {
                hardBlink(RING, RING_LEN, times, onMs, offMs);
            }
            Serial.println("OK");
        } else if (line.startsWith("D ")) {
            // "D <brightness> <topA> <topB> <bottomA> <bottomB>" -- each
            // digit slot is 0-9 or "X" to leave blank; renders all four at
            // once like a clock face (topA topB : bottomA bottomB)
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            uint8_t brightness = (uint8_t)rest.substring(0, sp1).toInt();
            String r2 = rest.substring(sp1 + 1);
            int sp2 = r2.indexOf(' ');
            String vTopA = r2.substring(0, sp2);
            String r3 = r2.substring(sp2 + 1);
            int sp3 = r3.indexOf(' ');
            String vTopB = r3.substring(0, sp3);
            String r4 = r3.substring(sp3 + 1);
            int sp4 = r4.indexOf(' ');
            String vBottomA = r4.substring(0, sp4);
            String vBottomB = r4.substring(sp4 + 1);

            clearAll();
            if (vTopA != "X") renderDigit(DIGIT_TOP_A, (uint8_t)vTopA.toInt(), brightness);
            if (vTopB != "X") renderDigit(DIGIT_TOP_B, (uint8_t)vTopB.toInt(), brightness);
            if (vBottomA != "X") renderDigit(DIGIT_BOTTOM_A, (uint8_t)vBottomA.toInt(), brightness);
            if (vBottomB != "X") renderDigit(DIGIT_BOTTOM_B, (uint8_t)vBottomB.toInt(), brightness);
            show();
            Serial.println("OK");
        } else if (line == "K off") {
            disarmBackgroundTwinkle();
            Serial.println("OK");
        } else if (line.startsWith("K ")) {
            // "K <minB> <maxB> <group>" -- arm a background twinkle (e.g.
            // stray_stars) that keeps flickering underneath whatever path
            // animation (W/R/Y/Z/B) runs next, until "K off" disarms it
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int minB = rest.substring(0, sp1).toInt();
            String r2 = rest.substring(sp1 + 1);
            int sp2 = r2.indexOf(' ');
            int maxB = r2.substring(0, sp2).toInt();
            String groupName = r2.substring(sp2 + 1);
            if (groupName == "stray_stars") {
                armBackgroundTwinkle(STRAY_STARS, STRAY_STARS_LEN, (uint8_t)minB, (uint8_t)maxB);
            } else if (groupName == "all") {
                armBackgroundTwinkle(ALL_LEDS, ALL_LEDS_LEN, (uint8_t)minB, (uint8_t)maxB);
            }
            Serial.println("OK");
        } else if (line.startsWith("N ")) {
            // "N <durationMs> <frameDelayMs> <minB> <maxB> <staticBrightness> <group>"
            // -- stray_stars twinkle while <group> (a constellation, or
            // both) stays lit solidly at <staticBrightness>
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int durationMs = rest.substring(0, sp1).toInt();
            String r2 = rest.substring(sp1 + 1);
            int sp2 = r2.indexOf(' ');
            int frameDelayMs = r2.substring(0, sp2).toInt();
            String r3 = r2.substring(sp2 + 1);
            int sp3 = r3.indexOf(' ');
            int minB = r3.substring(0, sp3).toInt();
            String r4 = r3.substring(sp3 + 1);
            int sp4 = r4.indexOf(' ');
            int maxB = r4.substring(0, sp4).toInt();
            String r5 = r4.substring(sp4 + 1);
            int sp5 = r5.indexOf(' ');
            int staticBrightness = r5.substring(0, sp5).toInt();
            String groupName = r5.substring(sp5 + 1);
            if (groupName == "constellation_left") {
                twinkleWithStatic(STRAY_STARS, STRAY_STARS_LEN, durationMs, frameDelayMs,
                                  (uint8_t)minB, (uint8_t)maxB, CONSTELLATION_LEFT,
                                  CONSTELLATION_LEFT_LEN, (uint8_t)staticBrightness);
            } else if (groupName == "constellation_right") {
                twinkleWithStatic(STRAY_STARS, STRAY_STARS_LEN, durationMs, frameDelayMs,
                                  (uint8_t)minB, (uint8_t)maxB, CONSTELLATION_RIGHT,
                                  CONSTELLATION_RIGHT_LEN, (uint8_t)staticBrightness);
            } else if (groupName == "both_constellations") {
                twinkleWithStatic(STRAY_STARS, STRAY_STARS_LEN, durationMs, frameDelayMs,
                                  (uint8_t)minB, (uint8_t)maxB, BOTH_CONSTELLATIONS,
                                  BOTH_CONSTELLATIONS_LEN, (uint8_t)staticBrightness);
            }
            Serial.println("OK");
        } else if (line.startsWith("E ")) {
            // "E <fromB> <toB> <ms> <group>" -- ease every LED in a group
            // from fromB to toB over ms, on-device, without clearing
            // anything else
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int fromB = rest.substring(0, sp1).toInt();
            String r2 = rest.substring(sp1 + 1);
            int sp2 = r2.indexOf(' ');
            int toB = r2.substring(0, sp2).toInt();
            String r3 = r2.substring(sp2 + 1);
            int sp3 = r3.indexOf(' ');
            int ms = r3.substring(0, sp3).toInt();
            String groupName = r3.substring(sp3 + 1);
            if (groupName == "constellation_left") {
                easeGroup(CONSTELLATION_LEFT, CONSTELLATION_LEFT_LEN, (uint8_t)fromB, (uint8_t)toB, ms);
            } else if (groupName == "constellation_right") {
                easeGroup(CONSTELLATION_RIGHT, CONSTELLATION_RIGHT_LEN, (uint8_t)fromB, (uint8_t)toB, ms);
            } else if (groupName == "both_constellations") {
                easeGroup(BOTH_CONSTELLATIONS, BOTH_CONSTELLATIONS_LEN, (uint8_t)fromB, (uint8_t)toB, ms);
            } else if (groupName == "x_icon") {
                easeGroup(X_ICON, X_ICON_LEN, (uint8_t)fromB, (uint8_t)toB, ms);
            } else if (groupName == "stray_stars") {
                easeGroup(STRAY_STARS, STRAY_STARS_LEN, (uint8_t)fromB, (uint8_t)toB, ms);
            }
            Serial.println("OK");
        } else if (line.startsWith("V ")) {
            // "V <toB> <ms> <group>" -- ease every LED in a group from its
            // own current brightness down/up to toB over ms, on-device
            String rest = line.substring(2);
            rest.trim();
            int sp1 = rest.indexOf(' ');
            int toB = rest.substring(0, sp1).toInt();
            String r2 = rest.substring(sp1 + 1);
            int sp2 = r2.indexOf(' ');
            int ms = r2.substring(0, sp2).toInt();
            String groupName = r2.substring(sp2 + 1);
            if (groupName == "constellation_left") {
                easeGroupFromCurrent(CONSTELLATION_LEFT, CONSTELLATION_LEFT_LEN, (uint8_t)toB, ms);
            } else if (groupName == "constellation_right") {
                easeGroupFromCurrent(CONSTELLATION_RIGHT, CONSTELLATION_RIGHT_LEN, (uint8_t)toB, ms);
            } else if (groupName == "both_constellations") {
                easeGroupFromCurrent(BOTH_CONSTELLATIONS, BOTH_CONSTELLATIONS_LEN, (uint8_t)toB, ms);
            } else if (groupName == "x_icon") {
                easeGroupFromCurrent(X_ICON, X_ICON_LEN, (uint8_t)toB, ms);
            } else if (groupName == "stray_stars") {
                easeGroupFromCurrent(STRAY_STARS, STRAY_STARS_LEN, (uint8_t)toB, ms);
            }
            Serial.println("OK");
        } else if (line.startsWith("U ")) {
            // "U <i1> <b1> <i2> <b2> ..." -- non-destructive pixel set: writes
            // each (index, brightness) pair without clearing anything else
            // already in frame[], for host-side compositing of several
            // independent layers (held constellation, clock digits, icons)
            String rest = line.substring(2);
            rest.trim();
            static uint16_t idxBuf[144];
            static uint8_t brightBuf[144];
            size_t n = 0;
            bool haveIdx = false;
            uint16_t pendingIdx = 0;
            forEachInt(rest, [&](long v) {
                if (!haveIdx) {
                    pendingIdx = (uint16_t)v;
                    haveIdx = true;
                } else {
                    if (n < 144) {
                        idxBuf[n] = pendingIdx;
                        brightBuf[n] = (uint8_t)v;
                        n++;
                    }
                    haveIdx = false;
                }
            });
            setPixels(idxBuf, brightBuf, n);
            Serial.println("OK");
        } else if (line == "C") {
            clearAll();
            show();
            Serial.println("OK");
        } else if (line == "A") {
            memset(frame, 0xFF, sizeof(frame));
            show();
            Serial.println("OK");
        }
    } else if (bgTwinkleActive) {
        // No command pending -- keep the armed background twinkle
        // animating on its own instead of freezing once the last
        // foreground animation returns. Doesn't touch anything else
        // already in frame[], so whatever the last command left lit
        // (e.g. a constellation) stays put.
        show();
        delay(40);
    }
}
