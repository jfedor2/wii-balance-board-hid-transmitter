// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include "gamepad.h"

// The balance axes: where the weight is on the board (the center of
// pressure), -1..1. +1 = all the weight on the right / front sensors.
enum class BoardAxis : int {
    LeftRight,
    ForwardBack,
    Count
};

constexpr int BOARD_AXIS_COUNT = (int) BoardAxis::Count;
extern const char* const BOARD_AXIS_NAMES[BOARD_AXIS_COUNT];         // "Left / right"
extern const char* const BOARD_AXIS_KEYS[BOARD_AXIS_COUNT];          // "left_right" (config)
extern const char* const BOARD_AXIS_DESCRIPTIONS[BOARD_AXIS_COUNT];  // "lean right = +"

// Slider limits, in balance units (-1..1).
struct AxisLimits {
    float range_min, range_max;  // full output at
    float deadzone_max;
    float display;  // the bar shows +/- this much
};
extern const AxisLimits AXIS_LIMITS[BOARD_AXIS_COUNT];

// What an axis drives. For sticks, the + side (right, forward) moves the stick
// right / up. Triggers: the + side drives R2, the - side L2. L2 / R2 alone are
// driven by the + side only.
enum class AxisTarget : int {
    None,
    LeftStickX,
    LeftStickY,
    RightStickX,
    RightStickY,
    Triggers,
    L2,
    R2,
    Count
};

constexpr int AXIS_TARGET_COUNT = (int) AxisTarget::Count;
extern const char* const AXIS_TARGET_NAMES[AXIS_TARGET_COUNT];

constexpr int NO_BUTTON = -1;

struct AxisMapping {
    AxisTarget target = AxisTarget::None;
    bool invert = false;
    float range = 0.5f;      // from neutral for full output
    float deadzone = 0.05f;  // from neutral with no output
    float curve = 1.0f;      // response exponent, 1 = linear
    // Buttons pressed past a level (0..1 of the way from deadzone to full)
    // on either side, independent of the analog target.
    int button_pos = NO_BUTTON;
    int button_neg = NO_BUTTON;
    float button_level = 0.5f;
};

// Linear level (-1..1) for a value relative to neutral: 0 inside the
// deadzone, 1 at the range. Invert applied, curve not.
float axis_level(const AxisMapping& m, float value);

// The analog output (-1..1): the level through the response curve.
float axis_output(const AxisMapping& m, float value);

// Whether a level presses a button, with a little hysteresis so it doesn't
// chatter at the threshold.
bool button_pressed(bool was_pressed, float level, float threshold);

// Adds the output of one axis to the gamepad state. Values add up when several
// axes drive the same output; clamp_state() caps them.
void apply_axis(const AxisMapping& m, float output, GamepadState& state);
void clamp_state(GamepadState& state);
