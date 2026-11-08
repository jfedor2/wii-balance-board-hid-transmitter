// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

#include "mapping.h"

struct WindowRect {
    bool valid = false;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool maximized = false;

    bool operator==(const WindowRect& o) const {
        return valid == o.valid && x == o.x && y == o.y && w == o.w && h == o.h && maximized == o.maximized;
    }
};

constexpr float MIN_WEIGHT_MIN = 2.0f;
constexpr float MIN_WEIGHT_MAX = 60.0f;

struct Config {
    std::string receiver_ip;
    AxisMapping axes[BOARD_AXIS_COUNT];
    int board_button = NO_BUTTON;  // what the board's own button presses
    float min_weight = 15.0f;      // kg on the board for it to count as in use
    float neutral[BOARD_AXIS_COUNT] = {};  // balance with no output
    WindowRect window;

    Config();
};

// The mapping defaults, also used by "Reset to defaults".
void default_mapping(Config& config);

// %APPDATA%\wii-balance-board-hid-transmitter\wii-balance-board-hid-transmitter,
// created if needed.
std::wstring app_data_dir();

bool load_config(const std::wstring& path, Config& config);
bool save_config(const std::wstring& path, const Config& config);
