// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "board.h"

#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "sender.h"

const char* const BOARD_SENSOR_NAMES[BOARD_SENSOR_COUNT] = {
    "top right",
    "bottom right",
    "top left",
    "bottom left",
};

static uint16_t be16(const uint8_t* p) {
    return (uint16_t) ((p[0] << 8) | p[1]);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
        }
    }
    return crc;
}

BoardCalibration parse_calibration(const uint8_t* c, const uint8_t* t) {
    BoardCalibration cal;
    for (int point = 0; point < 3; point++) {
        for (int s = 0; s < BOARD_SENSOR_COUNT; s++) {
            cal.raw[s][point] = be16(c + 4 + point * 8 + s * 2);
        }
    }
    cal.ref_temperature = t[0];
    // CRC32 over 24..3b, 20..21 and 60..61 (wiibrew).
    uint32_t crc = 0xffffffffu;
    crc = crc32_update(crc, c + 4, 24);
    crc = crc32_update(crc, c, 2);
    crc = crc32_update(crc, t, 2);
    crc ^= 0xffffffffu;
    const uint32_t stored = ((uint32_t) c[28] << 24) | ((uint32_t) c[29] << 16) | ((uint32_t) c[30] << 8) | c[31];
    cal.checksum_ok = crc == stored;
    return cal;
}

float sensor_kg(const BoardCalibration& cal, int s, uint16_t raw) {
    const uint16_t* p = cal.raw[s];
    // Below 17 kg use the 0-17 segment, above it the 17-34 one (as Linux does).
    const int seg = raw < p[1] ? 0 : 1;
    const float lo = p[seg], hi = p[seg + 1];
    if (hi == lo) {
        return 17.0f * seg;
    }
    return 17.0f * seg + 17.0f * ((float) raw - lo) / (hi - lo);
}

float temperature_factor(uint8_t temperature, uint8_t ref_temperature) {
    return 0.999f * (1.0f - 0.0007f * ((float) temperature - (float) ref_temperature));
}

int battery_bars(uint8_t b) {
    if (b >= 0x82) return 4;
    if (b >= 0x7d) return 3;
    if (b >= 0x78) return 2;
    if (b >= 0x6a) return 1;
    return 0;
}

bool parse_board_frame(const uint8_t* r, size_t len, BoardFrame& f) {
    if (len < 11 || (r[0] != wii::IN_BUTTONS_EXT8 && r[0] != wii::IN_BUTTONS_EXT19)) {
        return false;
    }
    f = BoardFrame();
    f.button = (r[2] & 0x08) != 0;
    for (int s = 0; s < BOARD_SENSOR_COUNT; s++) {
        f.raw[s] = be16(r + 3 + s * 2);
    }
    if (r[0] == wii::IN_BUTTONS_EXT19 && len >= 14) {
        f.have_extra = true;
        f.temperature = r[3 + 8];
        f.battery = r[3 + 10];
    }
    return true;
}

BoardWeights compute_weights(const BoardCalibration& cal, const BoardFrame& f, const float* tare) {
    BoardWeights w;
    for (int s = 0; s < BOARD_SENSOR_COUNT; s++) {
        w.kg[s] = sensor_kg(cal, s, f.raw[s]) - (tare ? tare[s] : 0.0f);
        w.total += w.kg[s];
    }
    w.total_corrected = f.have_extra ? w.total * temperature_factor(f.temperature, cal.ref_temperature) : w.total;
    if (w.total > 1.0f) {
        using S = BoardSensor;
        // A sensor can read a little below zero, so clamp them for the balance.
        float k[BOARD_SENSOR_COUNT];
        float sum = 0.0f;
        for (int s = 0; s < BOARD_SENSOR_COUNT; s++) {
            k[s] = std::max(w.kg[s], 0.0f);
            sum += k[s];
        }
        if (sum > 0.0f) {
            w.balance[(int) BoardAxis::LeftRight] =
                ((k[(int) S::TopRight] + k[(int) S::BottomRight]) - (k[(int) S::TopLeft] + k[(int) S::BottomLeft])) /
                sum;
            w.balance[(int) BoardAxis::ForwardBack] =
                ((k[(int) S::TopRight] + k[(int) S::TopLeft]) - (k[(int) S::BottomRight] + k[(int) S::BottomLeft])) /
                sum;
        }
    }
    return w;
}

// ---------------------------------------------------------------------------

static std::string narrow(const wchar_t* w) {
    char buf[256];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string();
}

bool BoardDevice::open(std::string& name, std::string& error) {
    close();
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO info = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) {
        error = "Can't list HID devices.";
        return false;
    }
    error = "Balance Board not found. Is it paired and connected?";
    std::vector<uint8_t> detail_buf;
    for (DWORD i = 0; handle_ == INVALID_HANDLE_VALUE; i++) {
        SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &hid_guid, i, &ifd)) {
            break;
        }
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &ifd, nullptr, 0, &size, nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }
        detail_buf.assign(size, 0);
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*) detail_buf.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &ifd, detail, size, nullptr, nullptr)) {
            continue;
        }
        HANDLE probe = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            continue;
        }
        HIDD_ATTRIBUTES attr = { sizeof(attr) };
        HIDP_CAPS caps = {};
        bool match = HidD_GetAttributes(probe, &attr) && attr.VendorID == WIIMOTE_VID &&
                     (attr.ProductID == WIIMOTE_PID || attr.ProductID == WIIMOTE_PID_PLUS);
        if (match) {
            PHIDP_PREPARSED_DATA pp = nullptr;
            match = HidD_GetPreparsedData(probe, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS;
            if (pp) {
                HidD_FreePreparsedData(pp);
            }
        }
        wchar_t wname[128] = {};
        if (match && !HidD_GetProductString(probe, wname, sizeof(wname))) {
            wname[0] = 0;
        }
        CloseHandle(probe);
        if (!match) {
            continue;
        }
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            error = "Found a Wii device, but can't open it.";
            continue;
        }
        handle_ = h;
        out_len_ = caps.OutputReportByteLength;
        read_buf_.assign(caps.InputReportByteLength ? caps.InputReportByteLength : 22, 0);
        name = wname[0] ? narrow(wname) : "Wii device";
        char ids[64];
        snprintf(ids, sizeof(ids), " (%04x:%04x, in %u / out %u bytes)", attr.VendorID, attr.ProductID,
            caps.InputReportByteLength, caps.OutputReportByteLength);
        name += ids;
        error.clear();
    }
    SetupDiDestroyDeviceInfoList(info);
    if (handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    read_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return true;
}

void BoardDevice::close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        if (read_pending_) {
            CancelIoEx(handle_, &read_ov_);
            DWORD n;
            GetOverlappedResult(handle_, &read_ov_, &n, TRUE);
            read_pending_ = false;
        }
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    if (read_event_) {
        CloseHandle(read_event_);
        read_event_ = nullptr;
    }
    if (write_event_) {
        CloseHandle(write_event_);
        write_event_ = nullptr;
    }
}

bool BoardDevice::write(const uint8_t* data, size_t len) {
    if (handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::vector<uint8_t> buf(out_len_ > len ? out_len_ : len, 0);
    memcpy(buf.data(), data, len);
    OVERLAPPED ov = {};
    ov.hEvent = write_event_;
    ResetEvent(write_event_);
    DWORD n = 0;
    if (!WriteFile(handle_, buf.data(), (DWORD) buf.size(), nullptr, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            // Some Bluetooth stacks only take output reports this way.
            return HidD_SetOutputReport(handle_, buf.data(), (ULONG) buf.size()) != FALSE;
        }
        if (WaitForSingleObject(write_event_, 1000) != WAIT_OBJECT_0) {
            CancelIoEx(handle_, &ov);
            GetOverlappedResult(handle_, &ov, &n, TRUE);
            return false;
        }
    }
    return GetOverlappedResult(handle_, &ov, &n, FALSE) != FALSE;
}

int BoardDevice::read(std::vector<uint8_t>& report, DWORD timeout_ms) {
    if (handle_ == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (!read_pending_) {
        read_ov_ = {};
        read_ov_.hEvent = read_event_;
        ResetEvent(read_event_);
        if (!ReadFile(handle_, read_buf_.data(), (DWORD) read_buf_.size(), nullptr, &read_ov_) &&
            GetLastError() != ERROR_IO_PENDING) {
            return -1;
        }
        read_pending_ = true;
    }
    const HANDLE handles[2] = { read_event_, abort_event_ };
    const DWORD r = WaitForMultipleObjects(abort_event_ ? 2 : 1, handles, FALSE, timeout_ms);
    if (r == WAIT_OBJECT_0 + 1) {
        return -1;
    }
    if (r != WAIT_OBJECT_0) {
        return 0;  // stays pending for the next call
    }
    read_pending_ = false;
    DWORD n = 0;
    if (!GetOverlappedResult(handle_, &read_ov_, &n, FALSE)) {
        return -1;
    }
    report.assign(read_buf_.begin(), read_buf_.begin() + n);
    return 1;
}

// ---------------------------------------------------------------------------

static constexpr DWORD ANSWER_TIMEOUT_MS = 1000;
static constexpr int RETRIES = 3;

bool wii_write_register(BoardDevice& dev, uint32_t address, const uint8_t* data, uint8_t len, std::string& error) {
    uint8_t cmd[22] = {};
    cmd[0] = wii::OUT_WRITE_MEMORY;
    cmd[1] = 0x04;  // registers, not EEPROM
    cmd[2] = (uint8_t) (address >> 16);
    cmd[3] = (uint8_t) (address >> 8);
    cmd[4] = (uint8_t) address;
    cmd[5] = len;
    memcpy(cmd + 6, data, len);
    for (int attempt = 0; attempt < RETRIES; attempt++) {
        if (!dev.write(cmd, sizeof(cmd))) {
            error = "Write failed.";
            continue;
        }
        const ULONGLONG deadline = GetTickCount64() + ANSWER_TIMEOUT_MS;
        std::vector<uint8_t> r;
        while (GetTickCount64() < deadline) {
            const int got = dev.read(r, (DWORD) (deadline - GetTickCount64()));
            if (got < 0) {
                error = "Read failed.";
                return false;
            }
            if (got > 0 && r.size() >= 5 && r[0] == wii::IN_ACK && r[3] == wii::OUT_WRITE_MEMORY) {
                if (r[4] != 0) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "Write to %06x failed with error %u.", address, r[4]);
                    error = buf;
                    return false;
                }
                return true;
            }
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "No answer to a write to %06x.", address);
        error = buf;
    }
    return false;
}

bool wii_read_memory(BoardDevice& dev, uint32_t address, uint16_t len, uint8_t* out, std::string& error) {
    uint8_t cmd[7];
    cmd[0] = wii::OUT_READ_MEMORY;
    cmd[1] = 0x04;
    cmd[2] = (uint8_t) (address >> 16);
    cmd[3] = (uint8_t) (address >> 8);
    cmd[4] = (uint8_t) address;
    cmd[5] = (uint8_t) (len >> 8);
    cmd[6] = (uint8_t) len;
    for (int attempt = 0; attempt < RETRIES; attempt++) {
        if (!dev.write(cmd, sizeof(cmd))) {
            error = "Write failed.";
            continue;
        }
        // The answer comes in 0x21 reports of up to 16 bytes each.
        uint16_t got_len = 0;
        ULONGLONG deadline = GetTickCount64() + ANSWER_TIMEOUT_MS;
        std::vector<uint8_t> r;
        bool failed = false;
        while (got_len < len && GetTickCount64() < deadline) {
            const int got = dev.read(r, (DWORD) (deadline - GetTickCount64()));
            if (got < 0) {
                error = "Read failed.";
                return false;
            }
            if (got == 0 || r.size() < 22 || r[0] != wii::IN_READ_DATA) {
                continue;
            }
            const int err = r[3] & 0x0f;
            const int size = (r[3] >> 4) + 1;
            const uint16_t offset = be16(&r[4]);
            if (err != 0) {
                char buf[64];
                snprintf(buf, sizeof(buf), "Read of %06x failed with error %d.", address, err);
                error = buf;
                failed = true;
                break;
            }
            const uint16_t pos = (uint16_t) (offset - (uint16_t) address);
            if (pos + size > len) {
                continue;
            }
            memcpy(out + pos, &r[6], size);
            got_len = (uint16_t) (got_len + size);
            deadline = GetTickCount64() + ANSWER_TIMEOUT_MS;
        }
        if (got_len >= len) {
            return true;
        }
        if (failed) {
            return false;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "No answer to a read of %06x.", address);
        error = buf;
    }
    return false;
}

bool wii_set_report_mode(BoardDevice& dev, uint8_t mode) {
    const uint8_t cmd[3] = { wii::OUT_REPORT_MODE, 0x04 /* continuous */, mode };
    return dev.write(cmd, sizeof(cmd));
}

bool wii_set_led(BoardDevice& dev, bool on) {
    const uint8_t cmd[2] = { wii::OUT_LED, (uint8_t) (on ? 0x10 : 0x00) };
    return dev.write(cmd, sizeof(cmd));
}

bool init_board(BoardDevice& dev, BoardInfo& info, std::string& error) {
    wii_set_led(dev, true);
    const uint8_t init1 = 0x55, init2 = 0x00;
    if (!wii_write_register(dev, wii::EXT_INIT1, &init1, 1, error) ||
        !wii_write_register(dev, wii::EXT_INIT2, &init2, 1, error)) {
        return false;
    }
    if (!wii_read_memory(dev, wii::EXT_ID, 6, info.ext_id, error)) {
        return false;
    }
    info.is_balance_board = memcmp(info.ext_id, BALANCE_BOARD_ID, 6) == 0;
    if (!info.is_balance_board) {
        error = "This isn't a Balance Board.";
        return false;
    }
    if (!wii_read_memory(dev, wii::EXT_CALIBRATION, 32, info.cal_bytes, error) ||
        !wii_read_memory(dev, wii::EXT_REF_TEMPERATURE, 2, info.temp_bytes, error)) {
        return false;
    }
    info.cal = parse_calibration(info.cal_bytes, info.temp_bytes);
    if (!wii_set_report_mode(dev, wii::IN_BUTTONS_EXT19)) {
        error = "Can't set the reporting mode.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

void BoardMapper::reset() {
    memset(buttons_, 0, sizeof(buttons_));
}

void BoardMapper::map(const BoardCalibration& cal, const BoardFrame& frame, const MappingSettings& settings,
    GamepadState& state, MappedBoard& out) {
    state = GamepadState();
    out = MappedBoard();
    out.weights = compute_weights(cal, frame, settings.tare);
    out.in_use = out.weights.total >= settings.min_weight;
    if (frame.button && settings.board_button != NO_BUTTON) {
        state.buttons |= button_mask((Button) settings.board_button);
    }
    if (!out.in_use) {
        memset(buttons_, 0, sizeof(buttons_));
        return;
    }
    for (int a = 0; a < BOARD_AXIS_COUNT; a++) {
        const AxisMapping& m = settings.axes[a];
        const float v = out.weights.balance[a] - settings.neutral[a];
        const float level = axis_level(m, v);
        apply_axis(m, axis_output(m, v), state);
        buttons_[a][0] = button_pressed(buttons_[a][0], level, m.button_level);
        buttons_[a][1] = button_pressed(buttons_[a][1], -level, m.button_level);
        if (buttons_[a][0] && m.button_pos != NO_BUTTON) {
            state.buttons |= button_mask((Button) m.button_pos);
        }
        if (buttons_[a][1] && m.button_neg != NO_BUTTON) {
            state.buttons |= button_mask((Button) m.button_neg);
        }
        out.relative[a] = v;
        out.levels[a] = level;
    }
    clamp_state(state);
}

// ---------------------------------------------------------------------------

static constexpr DWORD RECONNECT_MS = 1000;
// Without data reports for this long, the output goes neutral; after KICK_MS
// we ask for them again, and after RESET_MS we start over.
static constexpr uint64_t STALE_MS = 500;
static constexpr uint64_t KICK_MS = 1000;
static constexpr uint64_t RESET_MS = 4000;

BoardReader::~BoardReader() {
    stop();
}

void BoardReader::start(Sender* sender) {
    sender_ = sender;
    quit_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    dev_.set_abort_event(quit_event_);
    thread_ = std::thread(&BoardReader::thread_main, this);
}

void BoardReader::stop() {
    if (thread_.joinable()) {
        SetEvent(quit_event_);
        thread_.join();
    }
    if (quit_event_) {
        dev_.set_abort_event(nullptr);
        CloseHandle(quit_event_);
        quit_event_ = nullptr;
    }
}

bool BoardReader::quitting() const {
    return WaitForSingleObject(quit_event_, 0) == WAIT_OBJECT_0;
}

void BoardReader::set_settings(const MappingSettings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
}

std::shared_ptr<const BoardSnapshot> BoardReader::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void BoardReader::publish() {
    auto snap = std::make_shared<const BoardSnapshot>(current_);
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = std::move(snap);
}

void BoardReader::set_state(const GamepadState& state) {
    current_.state = state;
    if (sender_) {
        sender_->set_state(state);
    }
}

bool BoardReader::connect() {
    std::string name, error;
    if (!dev_.open(name, error)) {
        current_.error = error;
        return false;
    }
    current_.error = "Found " + name + ", initializing...";
    publish();
    BoardInfo info;
    if (!init_board(dev_, info, error)) {
        dev_.close();
        current_.error = "Can't initialize the board: " + error;
        return false;
    }
    current_.device_name = "Balance Board";
    current_.cal = info.cal;
    current_.connected = true;
    current_.connection = ++connection_;
    current_.error.clear();
    mapper_.reset();
    const uint64_t now = GetTickCount64();
    last_frame_ms_ = last_kick_ms_ = rate_start_ms_ = now;
    rate_count_ = 0;
    return true;
}

void BoardReader::disconnect(const char* why) {
    dev_.close();
    current_ = BoardSnapshot();
    current_.error = why;
    mapper_.reset();
    set_state(GamepadState());
}

void BoardReader::go_stale() {
    current_.have_frame = false;
    current_.rate = 0.0f;
    current_.mapped = MappedBoard();
    mapper_.reset();
    set_state(GamepadState());
}

void BoardReader::handle_report(const std::vector<uint8_t>& r) {
    if (r.empty()) {
        return;
    }
    BoardFrame frame;
    if (!parse_board_frame(r.data(), r.size(), frame)) {
        if (r[0] == wii::IN_STATUS) {
            // A status report (sent when the extension state changes, for
            // example) turns continuous reporting off; turn it back on.
            wii_set_report_mode(dev_, wii::IN_BUTTONS_EXT19);
        }
        return;
    }
    MappingSettings settings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
    }
    GamepadState state;
    mapper_.map(current_.cal, frame, settings, state, current_.mapped);
    current_.frame = frame;
    current_.have_frame = true;
    for (int s = 0; s < BOARD_SENSOR_COUNT; s++) {
        current_.raw_kg[s] = sensor_kg(current_.cal, s, frame.raw[s]);
    }
    const uint64_t now = GetTickCount64();
    last_frame_ms_ = now;
    rate_count_++;
    if (now - rate_start_ms_ >= 1000) {
        current_.rate = rate_count_ * 1000.0f / (float) (now - rate_start_ms_);
        rate_start_ms_ = now;
        rate_count_ = 0;
    }
    set_state(state);
    publish();
}

void BoardReader::thread_main() {
    std::vector<uint8_t> report;
    while (!quitting()) {
        if (!current_.connected) {
            if (!connect()) {
                if (quitting()) {
                    break;
                }
                publish();
                WaitForSingleObject(quit_event_, RECONNECT_MS);
                continue;
            }
            publish();
        }
        const int got = dev_.read(report, 100);
        if (got < 0) {
            if (!quitting()) {
                disconnect("Balance Board disconnected.");
                publish();
            }
            continue;
        }
        if (got > 0) {
            handle_report(report);
            continue;
        }
        // Quiet. Go neutral, ask for reports again, and eventually start over.
        const uint64_t now = GetTickCount64();
        if (current_.have_frame && now - last_frame_ms_ > STALE_MS) {
            go_stale();
            publish();
        }
        if (now - last_frame_ms_ > RESET_MS) {
            disconnect("The board stopped sending data, reconnecting...");
            publish();
        } else if (now - last_frame_ms_ > KICK_MS && now - last_kick_ms_ > KICK_MS) {
            last_kick_ms_ = now;
            wii_set_report_mode(dev_, wii::IN_BUTTONS_EXT19);
        }
    }
    if (dev_.is_open()) {
        wii_set_led(dev_, false);
        dev_.close();
    }
}
