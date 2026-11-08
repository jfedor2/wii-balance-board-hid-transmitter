// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "mapping.h"

#include <algorithm>
#include <cmath>

const char* const BOARD_AXIS_NAMES[BOARD_AXIS_COUNT] = {
    "Left / right",
    "Forward / back",
};

const char* const BOARD_AXIS_KEYS[BOARD_AXIS_COUNT] = {
    "left_right",
    "forward_back",
};

const char* const BOARD_AXIS_DESCRIPTIONS[BOARD_AXIS_COUNT] = {
    "lean right = +",
    "lean forward = +",
};

const AxisLimits AXIS_LIMITS[BOARD_AXIS_COUNT] = {
    { 0.05f, 1.0f, 0.5f, 1.0f },
    { 0.05f, 1.0f, 0.5f, 1.0f },
};

const char* const AXIS_TARGET_NAMES[AXIS_TARGET_COUNT] = {
    "None",
    "Left stick X",
    "Left stick Y",
    "Right stick X",
    "Right stick Y",
    "L2 / R2",
    "L2",
    "R2",
};

float axis_level(const AxisMapping& m, float value) {
    if (m.invert) {
        value = -value;
    }
    const float mag = std::abs(value);
    if (mag <= m.deadzone) {
        return 0.0f;
    }
    const float span = std::max(m.range - m.deadzone, 1e-3f);
    const float out = std::min((mag - m.deadzone) / span, 1.0f);
    return value < 0 ? -out : out;
}

float axis_output(const AxisMapping& m, float value) {
    const float level = axis_level(m, value);
    if (m.curve == 1.0f || level == 0.0f) {
        return level;
    }
    const float out = std::pow(std::abs(level), m.curve);
    return level < 0 ? -out : out;
}

bool button_pressed(bool was_pressed, float level, float threshold) {
    static constexpr float HYSTERESIS = 0.05f;
    if (was_pressed) {
        return level > std::max(threshold - HYSTERESIS, 0.0f);
    }
    return level >= threshold && level > 0.0f;
}

void apply_axis(const AxisMapping& m, float v, GamepadState& state) {
    float(&ls)[2] = state.sticks[(int) Stick::Left];
    float(&rs)[2] = state.sticks[(int) Stick::Right];
    float& l2 = state.triggers[(int) Trigger::L2];
    float& r2 = state.triggers[(int) Trigger::R2];
    switch (m.target) {
        case AxisTarget::None:
        case AxisTarget::Count:
            break;
        case AxisTarget::LeftStickX:
            ls[0] += v;
            break;
        case AxisTarget::LeftStickY:
            ls[1] -= v;  // -1 = up
            break;
        case AxisTarget::RightStickX:
            rs[0] += v;
            break;
        case AxisTarget::RightStickY:
            rs[1] -= v;
            break;
        case AxisTarget::Triggers:
            (v > 0 ? r2 : l2) += std::abs(v);
            break;
        case AxisTarget::L2:
            l2 += std::max(v, 0.0f);
            break;
        case AxisTarget::R2:
            r2 += std::max(v, 0.0f);
            break;
    }
}

void clamp_state(GamepadState& state) {
    for (auto& s : state.sticks) {
        for (float& a : s) {
            a = std::clamp(a, -1.0f, 1.0f);
        }
    }
    for (float& t : state.triggers) {
        t = std::clamp(t, 0.0f, 1.0f);
    }
}
