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

#define PIN_CLK GPIO_NUM_18
#define PIN_DIN GPIO_NUM_23

static const size_t NUM_LEDS = 144;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("PLS916H driver bring-up");

    SPI.begin(PIN_CLK, -1 /* MISO unused */, PIN_DIN);
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

void show() {
    write_pls916h(frame);
}

// Host-controlled mode. Protocol (one line at a time over serial, newline
// terminated):
//   "L <index>"        -> clear all, light only <index> at full brightness
//   "F <i1> <i2> ..."   -> clear all, light every listed index at full
//                          brightness (space-separated, any count)
//   "C"                -> clear all
//   "A"                -> light all LEDs (sanity check)
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
            clearAll();
            String rest = line.substring(2);
            rest.trim();
            int start = 0;
            while (start < (int)rest.length()) {
                int sp = rest.indexOf(' ', start);
                String tok = (sp == -1) ? rest.substring(start) : rest.substring(start, sp);
                if (tok.length() > 0) {
                    setPixel((size_t)tok.toInt(), 0xFF);
                }
                if (sp == -1) break;
                start = sp + 1;
            }
            show();
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
    }
}
