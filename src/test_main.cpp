// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

// bht-test: self-tests (no arguments); "dump [seconds]", which prints what
// the connected Balance Board sends; "read [seconds]", which runs the app's
// reader and prints what it would send.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "board.h"
#include "config.h"
#include "stadia.h"

static int failures = 0;

#define CHECK(cond)                                               \
    do {                                                          \
        if (!(cond)) {                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                           \
        }                                                         \
    } while (0)

static bool close_to(float a, float b, float eps = 0.01f) {
    return std::fabs(a - b) < eps;
}

static void selftest() {
    // The sample calibration from wiibrew.
    const uint8_t cal[32] = {
        0x01, 0x69, 0x00, 0x00,
        0x07, 0xbc, 0x11, 0x8b, 0x06, 0xba, 0x46, 0x52,
        0x0e, 0x6e, 0x18, 0x79, 0x0d, 0x5d, 0x4d, 0x4c,
        0x15, 0x2e, 0x1f, 0x71, 0x14, 0x07, 0x54, 0x51,
        0xa9, 0x06, 0xb4, 0xf0,
    };
    const uint8_t temp[2] = { 0x19, 0x01 };
    BoardCalibration c = parse_calibration(cal, temp);
    CHECK(c.checksum_ok);
    CHECK(c.ref_temperature == 0x19);
    CHECK(c.raw[0][0] == 0x07bc && c.raw[3][0] == 0x4652);
    CHECK(c.raw[1][1] == 0x1879 && c.raw[2][2] == 0x1407);
    CHECK(close_to(sensor_kg(c, 0, 0x07bc), 0.0f));
    CHECK(close_to(sensor_kg(c, 0, 0x0e6e), 17.0f));
    CHECK(close_to(sensor_kg(c, 0, 0x152e), 34.0f));
    CHECK(close_to(sensor_kg(c, 3, (0x4652 + 0x4d4c) / 2), 8.5f, 0.05f));
    CHECK(close_to(sensor_kg(c, 3, 0x5451 + (0x5451 - 0x4d4c)), 51.0f));
    uint8_t bad[32];
    memcpy(bad, cal, sizeof(bad));
    bad[10] ^= 1;
    CHECK(!parse_calibration(bad, temp).checksum_ok);

    // A report in mode 0x34: A pressed, 17 kg on each sensor.
    uint8_t r[22] = { 0x34, 0x00, 0x08, 0x0e, 0x6e, 0x18, 0x79, 0x0d, 0x5d, 0x4d, 0x4c, 0x19, 0x00, 0x80 };
    BoardFrame f;
    CHECK(parse_board_frame(r, sizeof(r), f));
    CHECK(f.button && f.have_extra && f.temperature == 0x19 && f.battery == 0x80);
    BoardWeights w = compute_weights(c, f);
    CHECK(close_to(w.total, 68.0f));
    CHECK(close_to(w.balance[0], 0.0f) && close_to(w.balance[1], 0.0f));
    CHECK(close_to(w.total_corrected, 68.0f * 0.999f));

    // Mapping, with the default settings. 34 kg on the right sensors, nothing
    // on the left ones, button pressed.
    Config cfg;
    MappingSettings ms;
    for (int a = 0; a < BOARD_AXIS_COUNT; a++) {
        ms.axes[a] = cfg.axes[a];
    }
    ms.board_button = cfg.board_button;
    ms.min_weight = cfg.min_weight;
    uint8_t right[22] = { 0x34, 0x00, 0x08, 0x15, 0x2e, 0x1f, 0x71, 0x06, 0xba, 0x46, 0x52, 0x19, 0x00, 0x80 };
    CHECK(parse_board_frame(right, sizeof(right), f));
    BoardMapper mapper;
    GamepadState state;
    MappedBoard mb;
    mapper.map(c, f, ms, state, mb);
    CHECK(mb.in_use);
    CHECK(close_to(mb.weights.total, 68.0f));
    CHECK(close_to(mb.weights.balance[(int) BoardAxis::LeftRight], 1.0f));
    CHECK(close_to(mb.weights.balance[(int) BoardAxis::ForwardBack], 0.0f));
    CHECK(close_to(state.sticks[(int) Stick::Left][0], 1.0f));
    CHECK(close_to(state.sticks[(int) Stick::Left][1], 0.0f));
    CHECK(state.buttons == button_mask(Button::A));

    // The neutral moves the center.
    ms.neutral[(int) BoardAxis::LeftRight] = 0.75f;
    mapper.map(c, f, ms, state, mb);
    CHECK(close_to(mb.relative[(int) BoardAxis::LeftRight], 0.25f));
    CHECK(close_to(state.sticks[(int) Stick::Left][0], (0.25f - 0.05f) / (0.5f - 0.05f)));
    ms.neutral[(int) BoardAxis::LeftRight] = 0.0f;

    // Forward: the top sensors. Left stick Y is -1 = up.
    uint8_t front[22] = { 0x34, 0x00, 0x00, 0x15, 0x2e, 0x11, 0x8b, 0x14, 0x07, 0x46, 0x52, 0x19, 0x00, 0x80 };
    CHECK(parse_board_frame(front, sizeof(front), f));
    mapper.map(c, f, ms, state, mb);
    CHECK(close_to(mb.weights.balance[(int) BoardAxis::ForwardBack], 1.0f));
    CHECK(close_to(state.sticks[(int) Stick::Left][1], -1.0f));
    CHECK(state.buttons == 0);

    // The tare comes off the weight; under the minimum weight there's no
    // output.
    for (float& t : ms.tare) {
        t = 10.0f;
    }
    mapper.map(c, f, ms, state, mb);
    CHECK(close_to(mb.weights.total, 28.0f));
    ms.min_weight = 30.0f;
    mapper.map(c, f, ms, state, mb);
    CHECK(!mb.in_use);
    CHECK(state == GamepadState());

    // Stadia packet, from the 3dRudder HID Transmitter.
    GamepadState gs;
    gs.buttons = button_mask(Button::A);
    gs.sticks[0][0] = 1.0f;
    uint8_t packet[STADIA_PACKET_SIZE];
    CHECK(build_stadia_packet(gs, packet) == STADIA_PACKET_SIZE);
    CHECK(packet[6] == 0x40 && packet[7] == 255 && packet[8] == 128);

    printf(failures ? "%d failure(s)\n" : "All tests passed.\n", failures);
}

static void print_hex(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        printf("%02x ", p[i]);
    }
    printf("\n");
}

static int dump(double seconds) {
    BoardDevice dev;
    std::string name, error;
    if (!dev.open(name, error)) {
        printf("%s\n", error.c_str());
        return 1;
    }
    printf("Device: %s\n", name.c_str());
    BoardInfo info;
    const bool ok = init_board(dev, info, error);
    printf("Extension ID: ");
    print_hex(info.ext_id, 6);
    if (!ok) {
        printf("Init failed: %s\n", error.c_str());
        return 1;
    }
    printf("Calibration (a40020): ");
    print_hex(info.cal_bytes, 32);
    printf("Ref temperature (a40060): ");
    print_hex(info.temp_bytes, 2);
    printf("Checksum %s\n", info.cal.checksum_ok ? "OK" : "MISMATCH");
    for (int s = 0; s < BOARD_SENSOR_COUNT; s++) {
        printf("  %-12s 0 kg %5u  17 kg %5u  34 kg %5u\n", BOARD_SENSOR_NAMES[s], info.cal.raw[s][0],
            info.cal.raw[s][1], info.cal.raw[s][2]);
    }
    printf("\n");

    const ULONGLONG start = GetTickCount64();
    const ULONGLONG end = seconds > 0 ? start + (ULONGLONG) (seconds * 1000) : ~0ull;
    int frames = 0, printed_at = 0;
    ULONGLONG rate_start = start;
    float rate = 0;
    std::vector<uint8_t> r;
    while (GetTickCount64() < end) {
        const int got = dev.read(r, 500);
        if (got < 0) {
            printf("Device disconnected.\n");
            return 1;
        }
        if (got == 0) {
            printf("(no reports for 500 ms)\n");
            continue;
        }
        BoardFrame f;
        if (parse_board_frame(r.data(), r.size(), f)) {
            frames++;
            const ULONGLONG now = GetTickCount64();
            if (now - rate_start >= 1000) {
                rate = (frames - printed_at) * 1000.0f / (float) (now - rate_start);
                rate_start = now;
                printed_at = frames;
            }
            if (frames % 10 == 1) {
                BoardWeights w = compute_weights(info.cal, f);
                printf("%6.2f  raw %5u %5u %5u %5u  kg %6.2f %6.2f %6.2f %6.2f  total %6.2f (%6.2f)  "
                       "lr %+.2f fb %+.2f  temp %02x bat %02x (%d)  %s  %.0f/s\n",
                    (now - start) / 1000.0, f.raw[0], f.raw[1], f.raw[2], f.raw[3], w.kg[0], w.kg[1], w.kg[2],
                    w.kg[3], w.total, w.total_corrected, w.balance[0], w.balance[1], f.temperature, f.battery,
                    battery_bars(f.battery), f.button ? "BUTTON" : "      ", rate);
            }
        } else {
            printf("report: ");
            print_hex(r.data(), r.size());
            if (r[0] == wii::IN_STATUS) {
                // A status report turns continuous reporting off.
                wii_set_report_mode(dev, wii::IN_BUTTONS_EXT19);
            }
        }
    }
    wii_set_led(dev, false);
    return 0;
}

// Runs the app's reader (with the saved settings) and prints what it would
// show and send, ten times a second.
static int read_mapped(double seconds) {
    Config cfg;
    load_config(app_data_dir() + L"\\settings.json", cfg);
    MappingSettings ms;
    for (int a = 0; a < BOARD_AXIS_COUNT; a++) {
        ms.axes[a] = cfg.axes[a];
        ms.neutral[a] = cfg.neutral[a];
    }
    ms.board_button = cfg.board_button;
    ms.min_weight = cfg.min_weight;
    BoardReader reader;
    reader.set_settings(ms);
    reader.start(nullptr);
    const ULONGLONG end = seconds > 0 ? GetTickCount64() + (ULONGLONG) (seconds * 1000) : ~0ull;
    while (GetTickCount64() < end) {
        Sleep(100);
        auto snap = reader.snapshot();
        if (!snap) {
            printf("(no snapshot yet)\n");
            continue;
        }
        if (!snap->connected) {
            printf("not connected: %s\n", snap->error.c_str());
            continue;
        }
        if (!snap->have_frame) {
            printf("connected, no data\n");
            continue;
        }
        const MappedBoard& m = snap->mapped;
        const GamepadState& s = snap->state;
        printf("%3.0f/s  %6.2f kg %s  balance %+.2f %+.2f  rel %+.2f %+.2f  LS %+.2f %+.2f  RS %+.2f %+.2f  "
               "L2 %.2f R2 %.2f  buttons %05x\n",
            snap->rate, m.weights.total, m.in_use ? "in use" : "      ", m.weights.balance[0], m.weights.balance[1],
            m.relative[0], m.relative[1], s.sticks[0][0], s.sticks[0][1], s.sticks[1][0], s.sticks[1][1],
            s.triggers[0], s.triggers[1], s.buttons);
    }
    reader.stop();
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
        return dump(argc >= 3 ? atof(argv[2]) : 0);
    }
    if (argc >= 2 && strcmp(argv[1], "read") == 0) {
        return read_mapped(argc >= 3 ? atof(argv[2]) : 0);
    }
    selftest();
    return failures ? 1 : 0;
}
