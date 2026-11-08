// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gamepad.h"
#include "mapping.h"

class Sender;

// ---------------------------------------------------------------------------
// Protocol
//
// The Wii Balance Board connects over Bluetooth as a Wii Remote (VID 057e,
// PID 0306) whose only peripheral is a built-in extension. It speaks the Wii
// Remote HID protocol: output report 0x16 writes a register, 0x17 reads one
// (answered with input reports 0x21), 0x12 sets the data reporting mode, 0x11
// sets the LED. See drivers/hid/hid-wiimote-*.c in Linux and
// wiibrew.org/wiki/Wii_Balance_Board.
//
// After the extension is initialized (0x55 to a400f0, 0x00 to a400fb) its
// registers at a40000 hold:
//
//   00..07  sensors: top right, bottom right, top left, bottom left
//           (16-bit big-endian, raw)
//   08      temperature
//   0a      battery
//   20..3f  calibration: 24..2b raw values at 0 kg, 2c..33 at 17 kg,
//           34..3b at 34 kg (same sensor order), 3c..3f CRC32
//   60      reference temperature (temperature at calibration)
//   fa..ff  extension ID, 00 00 a4 20 04 02 for the balance board
//
// In reporting mode 0x34 each input report carries the two core button bytes
// and the first 19 extension bytes, so the sensors, temperature and battery.
// The board's button is the Wii Remote's A button.

constexpr uint16_t WIIMOTE_VID = 0x057e;
constexpr uint16_t WIIMOTE_PID = 0x0306;
constexpr uint16_t WIIMOTE_PID_PLUS = 0x0330;

constexpr int BOARD_SENSOR_COUNT = 4;
enum class BoardSensor { TopRight, BottomRight, TopLeft, BottomLeft };
extern const char* const BOARD_SENSOR_NAMES[BOARD_SENSOR_COUNT];

namespace wii {
constexpr uint8_t OUT_LED = 0x11;
constexpr uint8_t OUT_REPORT_MODE = 0x12;
constexpr uint8_t OUT_STATUS_REQUEST = 0x15;
constexpr uint8_t OUT_WRITE_MEMORY = 0x16;
constexpr uint8_t OUT_READ_MEMORY = 0x17;

constexpr uint8_t IN_STATUS = 0x20;
constexpr uint8_t IN_READ_DATA = 0x21;
constexpr uint8_t IN_ACK = 0x22;
constexpr uint8_t IN_BUTTONS_EXT8 = 0x32;
constexpr uint8_t IN_BUTTONS_EXT19 = 0x34;

constexpr uint32_t EXT_INIT1 = 0xa400f0;
constexpr uint32_t EXT_INIT2 = 0xa400fb;
constexpr uint32_t EXT_ID = 0xa400fa;
constexpr uint32_t EXT_CALIBRATION = 0xa40020;
constexpr uint32_t EXT_REF_TEMPERATURE = 0xa40060;
}  // namespace wii

constexpr uint8_t BALANCE_BOARD_ID[6] = { 0x00, 0x00, 0xa4, 0x20, 0x04, 0x02 };

struct BoardCalibration {
    // raw[sensor][point], points at 0, 17 and 34 kg
    uint16_t raw[BOARD_SENSOR_COUNT][3] = {};
    uint8_t ref_temperature = 0;
    bool checksum_ok = false;
};

// Parses the 32 bytes at a40020 and the 2 at a40060.
BoardCalibration parse_calibration(const uint8_t* cal32, const uint8_t* temp2);

// Raw sensor value to kg, interpolating between the calibration points (and
// extrapolating past them). Not clamped, so a little negative when empty.
float sensor_kg(const BoardCalibration& cal, int sensor, uint16_t raw);

// The correction for temperature Wii Fit is said to apply to the total
// weight (wiibrew): a factor around 1.
float temperature_factor(uint8_t temperature, uint8_t ref_temperature);

// 0..4 bars, as on wiibrew.
int battery_bars(uint8_t battery);

struct BoardFrame {
    bool button = false;
    uint16_t raw[BOARD_SENSOR_COUNT] = {};
    bool have_extra = false;  // temperature and battery (mode 0x34 only)
    uint8_t temperature = 0;
    uint8_t battery = 0;
};

// Decodes a 0x32 or 0x34 input report (report ID included).
bool parse_board_frame(const uint8_t* report, size_t len, BoardFrame& frame);

struct BoardWeights {
    float kg[BOARD_SENSOR_COUNT] = {};  // tare subtracted
    float total = 0.0f;
    float total_corrected = 0.0f;  // with the temperature factor
    // Center of pressure, -1..1 (the balance axes): +1 = all the weight on
    // the right / top sensors. Zero when there's (almost) nothing on the board.
    float balance[BOARD_AXIS_COUNT] = {};
};

// tare: kg to subtract from each sensor, or null.
BoardWeights compute_weights(const BoardCalibration& cal, const BoardFrame& frame, const float* tare = nullptr);

// ---------------------------------------------------------------------------
// HID access

class BoardDevice {
public:
    ~BoardDevice() { close(); }

    // When set, read() returns -1 as soon as this event is signaled.
    void set_abort_event(HANDLE event) { abort_event_ = event; }

    // Opens the first Wii Remote-like HID device. Fills name or error.
    bool open(std::string& name, std::string& error);
    void close();
    bool is_open() const { return handle_ != INVALID_HANDLE_VALUE; }

    // Sends an output report (report ID first), padded to the device's length.
    bool write(const uint8_t* data, size_t len);
    // 1 = got a report, 0 = timeout, -1 = error (device gone).
    int read(std::vector<uint8_t>& report, DWORD timeout_ms);

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE read_event_ = nullptr;
    HANDLE write_event_ = nullptr;
    HANDLE abort_event_ = nullptr;
    OVERLAPPED read_ov_ = {};
    bool read_pending_ = false;
    std::vector<uint8_t> read_buf_;
    size_t out_len_ = 0;
};

// Wii Remote commands on top of BoardDevice. Reports that arrive while
// waiting for an answer are dropped.
bool wii_write_register(BoardDevice& dev, uint32_t address, const uint8_t* data, uint8_t len, std::string& error);
bool wii_read_memory(BoardDevice& dev, uint32_t address, uint16_t len, uint8_t* out, std::string& error);
bool wii_set_report_mode(BoardDevice& dev, uint8_t mode);
bool wii_set_led(BoardDevice& dev, bool on);

struct BoardInfo {
    uint8_t ext_id[6] = {};
    bool is_balance_board = false;
    uint8_t cal_bytes[32] = {};
    uint8_t temp_bytes[2] = {};
    BoardCalibration cal;
};

// Initializes the extension, reads the ID and calibration, turns the LED on
// and starts continuous reporting in mode 0x34.
bool init_board(BoardDevice& dev, BoardInfo& info, std::string& error);

// ---------------------------------------------------------------------------
// Mapping

struct MappingSettings {
    AxisMapping axes[BOARD_AXIS_COUNT];
    int board_button = NO_BUTTON;
    float min_weight = 15.0f;
    float neutral[BOARD_AXIS_COUNT] = {};
    float tare[BOARD_SENSOR_COUNT] = {};
};

struct MappedBoard {
    BoardWeights weights;
    bool in_use = false;  // enough weight on it
    float relative[BOARD_AXIS_COUNT] = {};  // balance minus neutral
    float levels[BOARD_AXIS_COUNT] = {};
};

// Turns frames into gamepad states. Keeps button states between frames.
class BoardMapper {
public:
    void map(const BoardCalibration& cal, const BoardFrame& frame, const MappingSettings& settings,
        GamepadState& state, MappedBoard& out);
    void reset();

private:
    bool buttons_[BOARD_AXIS_COUNT][2] = {};
};

// ---------------------------------------------------------------------------
// Reading the board

// What the UI shows.
struct BoardSnapshot {
    bool connected = false;  // found and initialized
    int connection = 0;      // goes up with every new connection
    std::string device_name;
    std::string error;
    BoardCalibration cal;
    bool have_frame = false;  // a data report arrived recently
    BoardFrame frame;
    float raw_kg[BOARD_SENSOR_COUNT] = {};  // without the tare
    MappedBoard mapped;
    float rate = 0.0f;  // reports per second
    GamepadState state;
};

// Finds the board, initializes it and reads it on its own thread
// (reconnecting if it goes away), maps it and passes the result to the sender.
class BoardReader {
public:
    ~BoardReader();
    void start(Sender* sender);
    void stop();

    void set_settings(const MappingSettings& settings);
    std::shared_ptr<const BoardSnapshot> snapshot();

private:
    void thread_main();
    bool connect();
    void disconnect(const char* why);
    void handle_report(const std::vector<uint8_t>& report);
    void go_stale();
    void publish();
    void set_state(const GamepadState& state);
    bool quitting() const;

    std::thread thread_;
    HANDLE quit_event_ = nullptr;
    Sender* sender_ = nullptr;

    std::mutex mutex_;
    MappingSettings settings_;
    std::shared_ptr<const BoardSnapshot> snapshot_;

    // Reader thread only.
    BoardDevice dev_;
    BoardSnapshot current_;
    BoardMapper mapper_;
    uint64_t last_frame_ms_ = 0;
    uint64_t last_kick_ms_ = 0;
    uint64_t rate_start_ms_ = 0;
    int rate_count_ = 0;
    int connection_ = 0;
};
