// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <winsock2.h>
#include <windows.h>

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "board.h"
#include "config.h"
#include "mapping.h"
#include "sender.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_device_context = nullptr;
static IDXGISwapChain* g_swap_chain = nullptr;
static ID3D11RenderTargetView* g_render_target_view = nullptr;
static bool g_swap_chain_occluded = false;
static UINT g_resize_width = 0;
static UINT g_resize_height = 0;
static float g_pending_dpi_scale = 0.0f;

static const ImVec4 COLOR_ERROR(1.0f, 0.42f, 0.42f, 1.0f);
static const ImVec4 COLOR_OK(0.45f, 0.9f, 0.55f, 1.0f);
static const ImVec4 COLOR_WARN(1.0f, 0.8f, 0.35f, 1.0f);

static const ImU32 COLOR_MARKER = IM_COL32(255, 200, 80, 255);
static const ImU32 COLOR_FILL = IM_COL32(60, 140, 220, 200);
static const ImU32 COLOR_BUTTON_MARK = IM_COL32(240, 120, 60, 255);

static constexpr float AUTO_ZERO_MAX_KG = 10.0f;

// Application state, owned by the UI thread.
struct App {
    Config config;
    std::wstring config_path;
    bool running = false;

    bool config_dirty = false;
    ULONGLONG config_dirty_since = 0;
    HWND main_hwnd = nullptr;

    Sender sender;
    BoardReader reader;
    std::shared_ptr<const BoardSnapshot> snapshot;

    // kg subtracted from each sensor, set with "Zero". Not saved: the empty
    // board's readings change with temperature.
    float tare[BOARD_SENSOR_COUNT] = {};
    bool tared = false;
    bool tare_auto = false;  // set by auto_zero()
    int auto_zero_connection = 0;

    void mark_dirty() {
        if (!config_dirty) {
            config_dirty_since = GetTickCount64();
        }
        config_dirty = true;
    }

    // Call after any change to the mapping.
    void mapping_changed(bool save = true) {
        MappingSettings s;
        for (int a = 0; a < BOARD_AXIS_COUNT; a++) {
            s.axes[a] = config.axes[a];
            s.neutral[a] = config.neutral[a];
        }
        s.board_button = config.board_button;
        s.min_weight = config.min_weight;
        for (int i = 0; i < BOARD_SENSOR_COUNT; i++) {
            s.tare[i] = tare[i];
        }
        reader.set_settings(s);
        if (save) {
            mark_dirty();
        }
    }

    void set_tare(const float* kg, bool automatic) {
        for (int s = 0; s < BOARD_SENSOR_COUNT; s++) {
            tare[s] = kg ? kg[s] : 0.0f;
        }
        tared = kg != nullptr;
        tare_auto = automatic;
        mapping_changed(false);
    }

    // Zeroes the board when it connects, if it's (nearly) empty: the factory
    // calibration can be off by a few kg.
    void auto_zero() {
        if (!have_frame() || snapshot->connection == auto_zero_connection) {
            return;
        }
        auto_zero_connection = snapshot->connection;
        float total = 0.0f;
        for (float kg : snapshot->raw_kg) {
            total += kg;
        }
        if (std::fabs(total) < AUTO_ZERO_MAX_KG) {
            set_tare(snapshot->raw_kg, true);
        }
    }

    bool have_frame() const {
        return snapshot && snapshot->have_frame;
    }

    bool in_use() const {
        return have_frame() && snapshot->mapped.in_use;
    }
};

static App g_app;

// ---------------------------------------------------------------------------
// D3D11 boilerplate (from the Dear ImGui example_win32_directx11)

static void create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer != nullptr) {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target_view);
        back_buffer->Release();
    }
}

static void cleanup_render_target() {
    if (g_render_target_view != nullptr) {
        g_render_target_view->Release();
        g_render_target_view = nullptr;
    }
}

static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_device_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_device_context);
    }
    if (FAILED(hr)) {
        return false;
    }
    create_render_target();
    return true;
}

static void cleanup_device() {
    cleanup_render_target();
    if (g_swap_chain) {
        g_swap_chain->Release();
        g_swap_chain = nullptr;
    }
    if (g_device_context) {
        g_device_context->Release();
        g_device_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

// ---------------------------------------------------------------------------

static void apply_style(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.Colors[ImGuiCol_PopupBg].w = 1.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

static bool g_track_placement = false;  // set once the saved placement has been applied

static void capture_window_placement() {
    if (!g_track_placement) {
        return;
    }
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (!GetWindowPlacement(g_app.main_hwnd, &wp)) {
        return;
    }
    WindowRect w;
    w.valid = true;
    w.x = wp.rcNormalPosition.left;
    w.y = wp.rcNormalPosition.top;
    w.w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    w.h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    w.maximized = wp.showCmd == SW_SHOWMAXIMIZED ||
                  (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    if (!(w == g_app.config.window)) {
        g_app.config.window = w;
        g_app.mark_dirty();
    }
}

static void save_now() {
    save_config(g_app.config_path, g_app.config);
    g_app.config_dirty = false;
}

// ---------------------------------------------------------------------------
// Window icon: a circle showing the state. The left half is the board:
// yellow = connected but not in use (nobody on it), green = in use. The right
// half is red when stopped, green when running. No board at all makes the
// whole circle red.

constexpr uint32_t ICON_RED = 0xd94040, ICON_YELLOW = 0xe0b830, ICON_GREEN = 0x3cb85a;

static HICON make_status_icon(uint32_t left_rgb, uint32_t right_rgb, int size) {
    BITMAPV5HEADER bh = {};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = size;
    bh.bV5Height = -size;  // top-down
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00ff0000;
    bh.bV5GreenMask = 0x0000ff00;
    bh.bV5BlueMask = 0x000000ff;
    bh.bV5AlphaMask = 0xff000000;
    uint32_t* pixels = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*) &bh, DIB_RGB_COLORS, (void**) &pixels, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) {
        return nullptr;
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);

    const float c = size / 2.0f, r = size / 2.0f - std::max(size / 32.0f, 0.5f);
    constexpr int SS = 4;  // supersampling, for smooth edges
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int inside = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    const float dx = x + (sx + 0.5f) / SS - c, dy = y + (sy + 0.5f) / SS - c;
                    inside += dx * dx + dy * dy <= r * r;
                }
            }
            const uint32_t rgb = x < size / 2 ? left_rgb : right_rgb;
            const uint32_t alpha = inside * 255 / (SS * SS);
            pixels[y * size + x] = (alpha << 24) | rgb;
        }
    }

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// Sets the window's icons if the state (or the DPI) changed.
static void update_icon(HWND hwnd) {
    static uint32_t current_left = 0, current_right = 0;
    static UINT current_dpi = 0;
    static HICON big_icon = nullptr, small_icon = nullptr;

    const BoardSnapshot* snap = g_app.snapshot.get();
    uint32_t left = ICON_RED, right = ICON_RED;
    if (snap && snap->connected) {
        left = g_app.in_use() ? ICON_GREEN : ICON_YELLOW;
        right = g_app.running ? ICON_GREEN : ICON_RED;
    }
    const UINT dpi = GetDpiForWindow(hwnd);
    if (left == current_left && right == current_right && dpi == current_dpi) {
        return;
    }
    current_left = left;
    current_right = right;
    current_dpi = dpi;
    HICON new_big = make_status_icon(left, right, GetSystemMetricsForDpi(SM_CXICON, dpi));
    HICON new_small = make_status_icon(left, right, GetSystemMetricsForDpi(SM_CXSMICON, dpi));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM) new_big);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM) new_small);
    if (big_icon) {
        DestroyIcon(big_icon);
    }
    if (small_icon) {
        DestroyIcon(small_icon);
    }
    big_icon = new_big;
    small_icon = new_small;
}

static void push_button_colors(ImVec4 base) {
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(base.x * 1.2f, base.y * 1.2f, base.z * 1.2f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(base.x * 1.4f, base.y * 1.4f, base.z * 1.4f, 1));
}

static bool button_combo(const char* id, int& button) {
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
    if (ImGui::BeginCombo(id, button == NO_BUTTON ? "None" : BUTTON_NAMES[button])) {
        if (ImGui::Selectable("None", button == NO_BUTTON)) {
            button = NO_BUTTON;
            changed = true;
        }
        for (int b = 0; b < BUTTON_COUNT; b++) {
            if (ImGui::Selectable(BUTTON_NAMES[b], button == b)) {
                button = b;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Rounded to 1 / scale, without the "-0.0" that printf gives small negatives.
static float no_negative_zero(float v, float scale) {
    return std::round(v * scale) / scale + 0.0f;
}

static void format_value(float v, char* buf, size_t size, bool sign = true) {
    snprintf(buf, size, sign ? "%+.2f" : "%.2f", v);
}

// ---------------------------------------------------------------------------
// Device view

// Where each sensor is in the view: -1 / +1 for left / right and bottom / top.
static const float SENSOR_POS[BOARD_SENSOR_COUNT][2] = {
    { 1, 1 },    // top right
    { 1, -1 },   // bottom right
    { -1, 1 },   // top left
    { -1, -1 },  // bottom left
};

// Top-down view of the board: the weight on each sensor, the balance (center
// of pressure) as a dot, the neutral, the deadzones and the full output.
static void draw_board_view(float width) {
    const App& app = g_app;
    const BoardSnapshot* snap = app.snapshot.get();
    const bool have = app.have_frame();
    const bool in_use = app.in_use();
    const float em = ImGui::GetFontSize();
    const float height = width * 0.56f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + height);
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c((p0.x + p1.x) / 2, (p0.y + p1.y) / 2);

    dl->AddRectFilled(p0, p1, IM_COL32(32, 34, 40, 255), height * 0.18f);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 60), height * 0.18f, 0, 1.5f);

    // The sensors sit at +/-1 on the balance scale.
    const float hx = width / 2 - em * 2.6f, hy = height / 2 - em * 2.0f;
    auto at = [&](float x, float y) { return ImVec2(c.x + x * hx, c.y - y * hy); };
    dl->AddLine(at(-1, 0), at(1, 0), IM_COL32(255, 255, 255, 25));
    dl->AddLine(at(0, -1), at(0, 1), IM_COL32(255, 255, 255, 25));

    // Sensors: a circle that grows with the weight on it.
    for (int s = 0; s < BOARD_SENSOR_COUNT; s++) {
        const ImVec2 sp = at(SENSOR_POS[s][0], SENSOR_POS[s][1]);
        const float kg = have ? snap->mapped.weights.kg[s] : 0.0f;
        const float r_max = em * 1.5f;
        dl->AddCircle(sp, r_max, IM_COL32(255, 255, 255, 40), 32, 1.0f);
        if (kg > 0.2f) {
            const float r = r_max * std::sqrt(std::clamp(kg / 50.0f, 0.0f, 1.0f));
            dl->AddCircleFilled(sp, std::max(r, 2.0f), in_use ? COLOR_FILL : IM_COL32(90, 100, 120, 200), 32);
        }
        char text[16];
        snprintf(text, sizeof(text), have ? "%.1f" : "--", no_negative_zero(kg, 10));
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const float ty = SENSOR_POS[s][1] > 0 ? sp.y - r_max - ts.y - em * 0.1f : sp.y + r_max + em * 0.1f;
        dl->AddText(ImVec2(sp.x - ts.x / 2, std::clamp(ty, p0.y + 2, p1.y - ts.y - 2)), IM_COL32(200, 200, 210, 255), text);
    }

    // Neutral, deadzone (shaded) and full output (outline), for the axes in use.
    const Config& cfg = app.config;
    const AxisMapping& mx = cfg.axes[(int) BoardAxis::LeftRight];
    const AxisMapping& my = cfg.axes[(int) BoardAxis::ForwardBack];
    auto used = [](const AxisMapping& m) {
        return m.target != AxisTarget::None || m.button_pos != NO_BUTTON || m.button_neg != NO_BUTTON;
    };
    const float nx = cfg.neutral[(int) BoardAxis::LeftRight], ny = cfg.neutral[(int) BoardAxis::ForwardBack];
    const bool ux = used(mx), uy = used(my);
    if (ux || uy) {
        const float dx = ux ? mx.deadzone : 1.0f, dy = uy ? my.deadzone : 1.0f;
        const float rx = ux ? mx.range : 1.0f, ry = uy ? my.range : 1.0f;
        auto clamped = [&](float x, float y) { return at(std::clamp(x, -1.0f, 1.0f), std::clamp(y, -1.0f, 1.0f)); };
        dl->AddRectFilled(clamped(nx - dx, ny + dy), clamped(nx + dx, ny - dy), IM_COL32(255, 255, 255, 22));
        dl->AddRect(clamped(nx - rx, ny + ry), clamped(nx + rx, ny - ry), IM_COL32(255, 255, 255, 90), 0, 0, 1.0f);
    }
    const ImVec2 n = at(nx, ny);
    dl->AddLine(ImVec2(n.x - em * 0.5f, n.y), ImVec2(n.x + em * 0.5f, n.y), IM_COL32(255, 255, 255, 140), 1.5f);
    dl->AddLine(ImVec2(n.x, n.y - em * 0.5f), ImVec2(n.x, n.y + em * 0.5f), IM_COL32(255, 255, 255, 140), 1.5f);

    if (have && snap->mapped.weights.total > 1.0f) {
        const float* b = snap->mapped.weights.balance;
        const ImVec2 dot = at(std::clamp(b[0], -1.0f, 1.0f), std::clamp(b[1], -1.0f, 1.0f));
        if (in_use) {
            dl->AddLine(n, dot, IM_COL32(255, 200, 80, 120), 2.0f);
        }
        dl->AddCircleFilled(dot, em * 0.4f, in_use ? COLOR_MARKER : IM_COL32(150, 150, 160, 200));
    }
    if (!in_use) {
        const char* text = !snap || !snap->connected ? "Not connected"
                           : !have                   ? "No data"
                                                     : "Step on the board";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 tp(c.x - ts.x / 2, c.y + em * 1.2f);
        dl->AddRectFilled(ImVec2(tp.x - em * 0.4f, tp.y - em * 0.2f), ImVec2(tp.x + ts.x + em * 0.4f, tp.y + ts.y + em * 0.2f),
            IM_COL32(0, 0, 0, 170), 4.0f);
        dl->AddText(tp, ImGui::GetColorU32(COLOR_WARN), text);
    }
}

static void draw_device_panel() {
    App& app = g_app;
    Config& cfg = app.config;
    const BoardSnapshot* snap = app.snapshot.get();
    const float em = ImGui::GetFontSize();
    const bool have = app.have_frame();
    const bool in_use = app.in_use();

    draw_board_view(std::clamp(ImGui::GetContentRegionAvail().x, em * 18, em * 34));

    // Weight and the rest of what the board reports.
    ImGui::Spacing();
    if (have) {
        const BoardWeights& w = snap->mapped.weights;
        ImGui::Text("Weight %.1f kg", no_negative_zero(w.total, 10));
        ImGui::SetItemTooltip("The total of the four sensors, minus the zero.\n"
                              "With the temperature correction Wii Fit is said to use: %.1f kg.",
            w.total_corrected);
        ImGui::SameLine(0, em * 1.5f);
        ImGui::TextDisabled("Balance %+.2f, %+.2f", w.balance[0], w.balance[1]);
        ImGui::SetItemTooltip("Left / right and forward / back, -1..1.\n"
                              "+1 = all the weight on the right / top sensors.");
    } else {
        ImGui::TextDisabled("Weight --");
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!in_use);
    push_button_colors(ImVec4(0.18f, 0.36f, 0.58f, 1));
    if (ImGui::Button("Recenter", ImVec2(em * 7, ImGui::GetFrameHeight() * 1.5f))) {
        for (int a = 0; a < BOARD_AXIS_COUNT; a++) {
            cfg.neutral[a] = snap->mapped.weights.balance[a];
        }
        app.mapping_changed();
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Take the current balance as neutral (no output).\n"
                          "Stand the way you normally would and click.");
    ImGui::SameLine();
    ImGui::BeginGroup();
    const bool recentered = cfg.neutral[0] != 0.0f || cfg.neutral[1] != 0.0f;
    if (recentered) {
        ImGui::Text("Neutral: %+.2f, %+.2f", cfg.neutral[0], cfg.neutral[1]);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##neutral")) {
            cfg.neutral[0] = cfg.neutral[1] = 0.0f;
            app.mapping_changed();
        }
        ImGui::SetItemTooltip("Go back to the middle of the board as neutral.");
    } else {
        ImGui::TextUnformatted("Neutral: the middle of the board.");
    }
    ImGui::TextDisabled(in_use ? "Stand where there should be no output and click."
                               : "Can be set while you're on the board.");
    ImGui::EndGroup();

    ImGui::BeginDisabled(!have);
    push_button_colors(ImVec4(0.30f, 0.30f, 0.36f, 1));
    if (ImGui::Button("Zero", ImVec2(em * 7, 0))) {
        app.set_tare(snap->raw_kg, false);
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Take what the empty board reads as zero. Step off the board first.\n"
                          "Done automatically when the board connects, if it's empty then.");
    ImGui::SameLine();
    if (app.tared) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(app.tare_auto ? "Zeroed when it connected." : "Zeroed.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##zero")) {
            app.set_tare(nullptr, false);
        }
        ImGui::SetItemTooltip("Go back to the board's own calibration.");
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Zero: the board's calibration.");
    }

    const float label_w = em * 7.5f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("In use above");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, em * 14));
    if (ImGui::SliderFloat("##minweight", &cfg.min_weight, MIN_WEIGHT_MIN, MIN_WEIGHT_MAX, "%.0f kg")) {
        app.mapping_changed();
    }
    ImGui::SetItemTooltip("With less weight than this on the board, the balance gives no output.");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Board button");
    ImGui::SameLine(label_w);
    if (button_combo("##boardbutton", cfg.board_button)) {
        app.mapping_changed();
    }
    ImGui::SetItemTooltip("What the button on the front of the board presses.");
    ImGui::SameLine();
    if (have && snap->frame.button) {
        ImGui::TextColored(COLOR_OK, "pressed");
    } else {
        ImGui::TextDisabled(have ? "released" : "");
    }

    ImGui::SeparatorText("Sensors");
    if (ImGui::BeginTable("sensors", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Sensor");
        ImGui::TableSetupColumn("kg");
        ImGui::TableSetupColumn("Raw");
        ImGui::TableSetupColumn("Calibration (0 / 17 / 34 kg)");
        ImGui::TableSetupColumn("Zero");
        ImGui::TableHeadersRow();
        for (int s = 0; s < BOARD_SENSOR_COUNT; s++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(BOARD_SENSOR_NAMES[s]);
            ImGui::TableNextColumn();
            if (have) {
                ImGui::Text("%6.2f", snap->mapped.weights.kg[s]);
            } else {
                ImGui::TextDisabled("--");
            }
            ImGui::TableNextColumn();
            if (have) {
                ImGui::TextDisabled("%5u", snap->frame.raw[s]);
            } else {
                ImGui::TextDisabled("--");
            }
            ImGui::TableNextColumn();
            if (snap && snap->connected) {
                const uint16_t* c = snap->cal.raw[s];
                ImGui::TextDisabled("%5u  %5u  %5u", c[0], c[1], c[2]);
            } else {
                ImGui::TextDisabled("--");
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%+.2f", app.tare[s]);
        }
        ImGui::EndTable();
    }
    if (snap && snap->connected) {
        ImGui::TextDisabled("Calibration checksum %s, reference temperature %u", snap->cal.checksum_ok ? "OK" : "wrong",
            snap->cal.ref_temperature);
    }
    if (have && snap->frame.have_extra) {
        static const char* const BARS[] = { "empty", "1/4", "2/4", "3/4", "full" };
        const int bars = battery_bars(snap->frame.battery);
        ImGui::TextDisabled("Temperature %u", snap->frame.temperature);
        ImGui::SameLine(0, em * 1.5f);
        if (bars <= 1) {
            ImGui::TextColored(bars == 0 ? COLOR_ERROR : COLOR_WARN, "Battery %s (%u)", BARS[bars], snap->frame.battery);
        } else {
            ImGui::TextDisabled("Battery %s (%u)", BARS[bars], snap->frame.battery);
        }
    }
}

// ---------------------------------------------------------------------------
// Output

static void draw_stick_indicator(Stick stick, const float (&value)[2]) {
    const float em = ImGui::GetFontSize();
    const float radius = em * 1.3f;
    const ImVec2 size(radius * 2, radius * 2);
    ImGui::BeginGroup();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + radius, p.y + radius);
    const Color3& col = STICK_COLORS[(int) stick];
    const bool centered = stick_byte(value[0]) == 128 && stick_byte(value[1]) == 128;
    dl->AddCircleFilled(c, radius, IM_COL32(40, 42, 48, 255));
    dl->AddCircle(c, radius, ImGui::GetColorU32(ImVec4(col.r, col.g, col.b, 1)), 0, 1.5f);
    dl->AddLine(ImVec2(c.x - radius, c.y), ImVec2(c.x + radius, c.y), IM_COL32(255, 255, 255, 30));
    dl->AddLine(ImVec2(c.x, c.y - radius), ImVec2(c.x, c.y + radius), IM_COL32(255, 255, 255, 30));
    const ImVec2 dot(c.x + value[0] * (radius - 3), c.y + value[1] * (radius - 3));
    dl->AddCircleFilled(dot, em * 0.22f,
        centered ? IM_COL32(120, 120, 130, 255)
                 : ImGui::GetColorU32(ImVec4(col.r + (1 - col.r) * 0.4f, col.g + (1 - col.g) * 0.4f, col.b + (1 - col.b) * 0.4f, 1)));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(STICK_NAMES[(int) stick]);
    ImGui::TextDisabled("%+.2f, %+.2f", value[0], 0.0f - value[1]);  // not -0.00
    ImGui::EndGroup();
    ImGui::EndGroup();
}

static void draw_output() {
    const GamepadState state = g_app.snapshot ? g_app.snapshot->state : GamepadState();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    const float slot_w = em * 2.6f + style.ItemSpacing.x +
                         std::max(ImGui::CalcTextSize(STICK_NAMES[(int) Stick::Right]).x,
                             ImGui::CalcTextSize("-0.00, -0.00").x * 1.15f);
    const float x0 = ImGui::GetCursorPosX();
    draw_stick_indicator(Stick::Left, state.sticks[(int) Stick::Left]);
    ImGui::SameLine(x0 + slot_w + em);
    draw_stick_indicator(Stick::Right, state.sticks[(int) Stick::Right]);
    for (int t = 0; t < TRIGGER_COUNT; t++) {
        const Color3& c = BUTTON_COLORS[(int) trigger_button((Trigger) t)];
        char overlay[16];
        snprintf(overlay, sizeof(overlay), "%.2f", state.triggers[t]);
        if (t > 0) {
            ImGui::SameLine(0, em);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TRIGGER_NAMES[t]);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(c.r, c.g, c.b, 1));
        ImGui::ProgressBar(state.triggers[t], ImVec2(em * 6, em * 1.1f), overlay);
        ImGui::PopStyleColor();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Buttons");
    bool any = false;
    for (int b = 0; b < BUTTON_COUNT; b++) {
        if (state.buttons & button_mask((Button) b)) {
            const Color3& c = BUTTON_COLORS[b];
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.r, c.g, c.b, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.r, c.g, c.b, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.r, c.g, c.b, 1));
            ImGui::SmallButton(BUTTON_NAMES[b]);
            ImGui::PopStyleColor(3);
            any = true;
        }
    }
    if (!any) {
        ImGui::SameLine();
        ImGui::TextDisabled("none");
    }
    if (!g_app.running) {
        ImGui::TextDisabled("Stopped: the receiver gets nothing pressed.");
    }
}

// ---------------------------------------------------------------------------
// Mapping

// Where a button level (0..1) is, in axis units.
static float button_point(const AxisMapping& m, float level) {
    return m.deadzone + level * (m.range - m.deadzone);
}

// The axis value on a scale, with the deadzone shaded, the full output points
// marked, button thresholds ticked and the output filled in from the center.
static void draw_axis_bar(int a, const AxisMapping& m, float value, bool valid, float width) {
    const float em = ImGui::GetFontSize();
    const float h = em * 1.25f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + h);
    ImGui::Dummy(ImVec2(width, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float scale = std::max(AXIS_LIMITS[a].display, m.range * 1.15f);
    const float cx = (p0.x + p1.x) / 2;
    // Invert flips which side is which, so the bar follows the output.
    const float dir = m.invert ? -1.0f : 1.0f;
    auto x_of = [&](float v) { return cx + std::clamp(v / scale, -1.0f, 1.0f) * width / 2; };
    const bool buttons = m.button_pos != NO_BUTTON || m.button_neg != NO_BUTTON;
    const bool used = m.target != AxisTarget::None || buttons;

    dl->AddRectFilled(p0, p1, IM_COL32(32, 34, 40, 255), 3.0f);
    if (used) {
        const float level = valid ? axis_level(m, value) : 0.0f;
        if (level != 0.0f) {
            const float edge = button_point(m, std::abs(level));
            const float sign = level < 0 ? -1.0f : 1.0f;
            const float x0 = x_of(sign * m.deadzone), x1 = x_of(sign * edge);
            dl->AddRectFilled(ImVec2(std::min(x0, x1), p0.y + 2), ImVec2(std::max(x0, x1), p1.y - 2), COLOR_FILL, 2.0f);
        }
        dl->AddRectFilled(ImVec2(x_of(-m.deadzone), p0.y), ImVec2(x_of(m.deadzone), p1.y), IM_COL32(255, 255, 255, 22));
        for (float s : { -1.0f, 1.0f }) {
            const float x = x_of(s * m.range);
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 255, 255, 120), 1.5f);
        }
        const float bp = button_point(m, m.button_level);
        if (m.button_pos != NO_BUTTON) {
            const float x = x_of(bp);
            dl->AddTriangleFilled(ImVec2(x - em * 0.3f, p1.y), ImVec2(x + em * 0.3f, p1.y), ImVec2(x, p1.y - em * 0.4f),
                COLOR_BUTTON_MARK);
        }
        if (m.button_neg != NO_BUTTON) {
            const float x = x_of(-bp);
            dl->AddTriangleFilled(ImVec2(x - em * 0.3f, p1.y), ImVec2(x + em * 0.3f, p1.y), ImVec2(x, p1.y - em * 0.4f),
                COLOR_BUTTON_MARK);
        }
    }
    dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p1.y), IM_COL32(255, 255, 255, 50));
    if (valid) {
        const float x = x_of(value * dir);
        dl->AddTriangleFilled(ImVec2(x - em * 0.35f, p0.y), ImVec2(x + em * 0.35f, p0.y), ImVec2(x, p0.y + em * 0.45f),
            COLOR_MARKER);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), COLOR_MARKER, 2.0f);
    }
}

static void draw_axis(int a) {
    App& app = g_app;
    AxisMapping& m = app.config.axes[a];
    const AxisLimits& lim = AXIS_LIMITS[a];
    const float em = ImGui::GetFontSize();
    const BoardSnapshot* snap = app.snapshot.get();
    const bool valid = app.in_use();
    const float value = valid ? snap->mapped.relative[a] : 0.0f;
    bool changed = false;

    ImGui::PushID(a);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(BOARD_AXIS_NAMES[a]);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", BOARD_AXIS_DESCRIPTIONS[a]);
    ImGui::SameLine();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    char text[32];
    if (valid) {
        format_value(value, text, sizeof(text));
    } else {
        snprintf(text, sizeof(text), "--");
    }
    ImGui::SetCursorPosX(right - ImGui::CalcTextSize("+0.00").x);
    ImGui::TextUnformatted(text);

    draw_axis_bar(a, m, value, valid, ImGui::GetContentRegionAvail().x);

    const float label_w = em * 5.5f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Output");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(em * 9);
    if (ImGui::BeginCombo("##target", AXIS_TARGET_NAMES[(int) m.target])) {
        for (int t = 0; t < AXIS_TARGET_COUNT; t++) {
            if (ImGui::Selectable(AXIS_TARGET_NAMES[t], (int) m.target == t)) {
                m.target = (AxisTarget) t;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip(
        "Sticks: the + side (lean right, lean forward) moves the stick right / up.\n"
        "L2 / R2: the + side drives R2, the - side drives L2.\n"
        "L2 or R2 alone: driven by the + side only (invert for the other side).\n"
        "Several axes can drive the same output; they add up.");
    ImGui::SameLine(0, em);
    changed |= ImGui::Checkbox("Invert", &m.invert);
    ImGui::SetItemTooltip("Swap the + and - sides, for the analog output and the buttons.");
    ImGui::SameLine(0, em);
    const float out = valid && m.target != AxisTarget::None ? axis_output(m, value) : 0.0f;
    ImGui::TextDisabled("output %+.2f", out);

    const float slider_w = ImGui::GetContentRegionAvail().x - label_w;
    char format[32];
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Sensitivity");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(slider_w);
    snprintf(format, sizeof(format), "full at %%.2f");
    if (ImGui::SliderFloat("##range", &m.range, lim.range_min, lim.range_max, format)) {
        m.range = std::max(m.range, m.deadzone + lim.range_min / 2);
        changed = true;
    }
    ImGui::SetItemTooltip("How far from neutral for full output, on the -1..1 balance scale.\n"
                          "1 = all the weight on one side. Lower = more sensitive.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Deadzone");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(slider_w);
    snprintf(format, sizeof(format), "%%.2f");
    if (ImGui::SliderFloat("##deadzone", &m.deadzone, 0.0f, lim.deadzone_max, format)) {
        m.range = std::max(m.range, m.deadzone + lim.range_min / 2);
        changed = true;
    }
    ImGui::SetItemTooltip("How far from neutral with no output.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Curve");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(slider_w);
    ImGui::BeginDisabled(m.target == AxisTarget::None);
    changed |= ImGui::SliderFloat("##curve", &m.curve, 1.0f, 3.0f, m.curve == 1.0f ? "linear" : "%.1f");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("1 = linear. Higher = finer control near neutral, the same full output.");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Buttons");
    ImGui::SameLine(label_w);
    ImGui::TextUnformatted("+");
    ImGui::SameLine();
    changed |= button_combo("##bpos", m.button_pos);
    ImGui::SameLine(0, em * 0.7f);
    ImGui::TextUnformatted("-");
    ImGui::SameLine();
    changed |= button_combo("##bneg", m.button_neg);
    ImGui::SameLine(0, em * 0.7f);
    ImGui::BeginDisabled(m.button_pos == NO_BUTTON && m.button_neg == NO_BUTTON);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    changed |= ImGui::SliderFloat("##blevel", &m.button_level, 0.0f, 1.0f, "at %.2f");
    ImGui::SetItemTooltip("Buttons pressed when the axis is this far between the deadzone (0) and full (1),\n"
                          "on the + or - side. Marked under the bar. Works with or without an analog output.");
    ImGui::EndDisabled();

    ImGui::PopID();
    if (changed) {
        app.mapping_changed();
    }
}

// ---------------------------------------------------------------------------

static void draw_top_bar() {
    App& app = g_app;
    Config& cfg = app.config;
    const float em = ImGui::GetFontSize();
    const BoardSnapshot* snap = app.snapshot.get();

    const ImVec2 big(em * 9, ImGui::GetFrameHeight() * 1.7f);
    push_button_colors(app.running ? ImVec4(0.13f, 0.52f, 0.22f, 1) : ImVec4(0.45f, 0.17f, 0.17f, 1));
    if (ImGui::Button(app.running ? "RUNNING" : "STOPPED", big)) {
        app.running = !app.running;
    }
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip(app.running ? "Click to stop: releases everything and stops sending inputs.\n"
                                        "The board is still read and the output still shown."
                                      : "Click to start sending inputs to the receiver.");

    ImGui::SameLine(0, em * 1.5f);
    ImGui::BeginGroup();
    const float label_w = em * 6;
    // Receiver
    static char ip_buffer[64] = {};
    static bool ip_init = false;
    if (!ip_init) {
        strncpy_s(ip_buffer, cfg.receiver_ip.c_str(), _TRUNCATE);
        ip_init = true;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Receiver IP");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(em * 9);
    if (ImGui::InputTextWithHint("##ip", "e.g. 192.168.1.50", ip_buffer, sizeof(ip_buffer),
            ImGuiInputTextFlags_CharsNoBlank)) {
        cfg.receiver_ip = ip_buffer;
        app.sender.set_receiver(cfg.receiver_ip);
        app.mark_dirty();
    }
    ImGui::SetItemTooltip("IP address of the HID Receiver. Inputs are sent as UDP to port 42734.");
    ImGui::SameLine();
    if (cfg.receiver_ip.empty()) {
        ImGui::TextDisabled("not set");
    } else if (!app.sender.address_valid()) {
        ImGui::TextColored(COLOR_ERROR, "invalid IPv4 address");
    } else if (app.sender.send_error()) {
        ImGui::TextColored(COLOR_ERROR, "send failed");
    } else if (app.running) {
        ImGui::TextColored(COLOR_OK, "sending (%llu packets)", (unsigned long long) app.sender.packets_sent());
    } else {
        ImGui::TextDisabled("idle");
    }

    // Device
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Board");
    ImGui::SameLine(label_w);
    if (!snap) {
        ImGui::TextDisabled("looking...");
    } else if (!snap->connected) {
        ImGui::TextColored(COLOR_ERROR, "%s", snap->error.c_str());
    } else {
        ImGui::TextUnformatted(snap->device_name.c_str());
        ImGui::SameLine();
        if (!snap->have_frame) {
            ImGui::TextColored(COLOR_WARN, "connected, no data");
        } else {
            const bool in_use = snap->mapped.in_use;
            ImGui::TextColored(in_use ? COLOR_OK : COLOR_WARN, in_use ? "In use" : "Nobody on it");
            ImGui::SameLine();
            ImGui::TextDisabled("(%.0f reports/s)", snap->rate);
        }
    }
    ImGui::EndGroup();
}

static void draw_ui() {
    App& app = g_app;
    Config& cfg = app.config;
    ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

    draw_top_bar();
    ImGui::Spacing();

    const float left_w = std::max(em * 24, ImGui::GetContentRegionAvail().x * 0.45f);

    if (ImGui::BeginChild("left", ImVec2(left_w, 0))) {
        ImGui::SeparatorText("Balance Board");
        draw_device_panel();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("right", ImVec2(0, 0))) {
        ImGui::SeparatorText("Axes");
        for (int a = 0; a < BOARD_AXIS_COUNT; a++) {
            if (a > 0) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            }
            draw_axis(a);
        }
        ImGui::Spacing();
        if (ImGui::SmallButton("Reset to defaults")) {
            ImGui::OpenPopup("reset");
        }
        if (ImGui::BeginPopup("reset")) {
            ImGui::TextUnformatted("Reset the axes, the board button and the minimum weight?");
            if (ImGui::Button("Reset")) {
                default_mapping(cfg);
                app.mapping_changed();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SeparatorText("Output");
        draw_output();
    }
    ImGui::EndChild();

    ImGui::End();
}

// ---------------------------------------------------------------------------

static LRESULT WINAPI main_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return true;
    }
    switch (msg) {
        case WM_SIZE:
            capture_window_placement();
            if (wparam == SIZE_MINIMIZED) {
                return 0;
            }
            g_resize_width = (UINT) LOWORD(lparam);
            g_resize_height = (UINT) HIWORD(lparam);
            return 0;
        case WM_MOVE:
            capture_window_placement();
            return 0;
        case WM_CLOSE:
            capture_window_placement();
            break;
        case WM_GETMINMAXINFO: {
            float s = GetDpiForWindow(hwnd) / 96.0f;
            auto* mmi = (MINMAXINFO*) lparam;
            mmi->ptMinTrackSize.x = (LONG) (960 * s);
            mmi->ptMinTrackSize.y = (LONG) (660 * s);
            return 0;
        }
        case WM_DPICHANGED: {
            g_pending_dpi_scale = HIWORD(wparam) / 96.0f;
            const RECT* r = (const RECT*) lparam;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) {  // disable ALT application menu
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    App& app = g_app;
    app.config_path = app_data_dir() + L"\\settings.json";
    load_config(app.config_path, app.config);
    if (wcsstr(GetCommandLineW(), L"--start") != nullptr) {
        app.running = true;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BalanceBoardHidTransmitter";
    RegisterClassExW(&wc);

    // Restore the window where it was last time, but only if its title bar
    // would still be reachable (monitors may have changed since).
    const WindowRect& saved = app.config.window;
    bool restore = false;
    if (saved.valid) {
        RECT title = { saved.x, saved.y, saved.x + saved.w, saved.y + GetSystemMetrics(SM_CYCAPTION) };
        if (HMONITOR mon = MonitorFromRect(&title, MONITOR_DEFAULTTONULL)) {
            MONITORINFO mi = { sizeof(mi) };
            RECT visible;
            restore = GetMonitorInfoW(mon, &mi) && IntersectRect(&visible, &title, &mi.rcWork) &&
                      visible.right - visible.left >= 100;
        }
    }

    const float primary_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    HWND hwnd = restore
        ? CreateWindowW(wc.lpszClassName, L"Wii Balance Board HID Transmitter", WS_OVERLAPPEDWINDOW,
              saved.x, saved.y, saved.w, saved.h, nullptr, nullptr, instance, nullptr)
        : CreateWindowW(wc.lpszClassName, L"Wii Balance Board HID Transmitter", WS_OVERLAPPEDWINDOW,
              (int) (80 * primary_scale), (int) (80 * primary_scale), (int) (1180 * primary_scale),
              (int) (820 * primary_scale), nullptr, nullptr, instance, nullptr);
    app.main_hwnd = hwnd;
    update_icon(hwnd);
    const float scale = GetDpiForWindow(hwnd) / 96.0f;

    if (!create_device(hwnd)) {
        cleanup_device();
        MessageBoxW(nullptr, L"Failed to initialize Direct3D 11.", L"Wii Balance Board HID Transmitter", MB_ICONERROR);
        return 1;
    }

    app.sender.set_receiver(app.config.receiver_ip);
    app.sender.start();
    app.mapping_changed();
    app.config_dirty = false;
    app.reader.start(&app.sender);

    if (restore) {
        WINDOWPLACEMENT wp = { sizeof(wp) };
        GetWindowPlacement(hwnd, &wp);
        wp.rcNormalPosition = { saved.x, saved.y, saved.x + saved.w, saved.y + saved.h };
        wp.showCmd = show_cmd;
        if (saved.maximized && (show_cmd == SW_SHOWNORMAL || show_cmd == SW_SHOW || show_cmd == SW_SHOWDEFAULT)) {
            wp.showCmd = SW_SHOWMAXIMIZED;
        }
        SetWindowPlacement(hwnd, &wp);
    } else {
        ShowWindow(hwnd, show_cmd);
    }
    UpdateWindow(hwnd);
    g_track_placement = true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    apply_style(scale);
    // Latin-1.
    static const ImWchar glyph_ranges[] = { 0x0020, 0x00FF, 0 };
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, nullptr, glyph_ranges);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_device_context);

    bool done = false;
    while (!done) {
        const bool minimized = IsIconic(hwnd) != 0;
        if (minimized || g_swap_chain_occluded) {
            // Nothing to draw: sleep until something happens.
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        app.sender.set_active(app.running);
        if (app.config_dirty && GetTickCount64() - app.config_dirty_since >= 1000) {
            save_now();
        }

        // Also while minimized, the taskbar shows the icon.
        app.snapshot = app.reader.snapshot();
        app.auto_zero();
        update_icon(hwnd);

        if (minimized) {
            continue;
        }
        if (g_swap_chain_occluded && g_swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            continue;
        }
        g_swap_chain_occluded = false;

        if (g_resize_width != 0 && g_resize_height != 0) {
            cleanup_render_target();
            g_swap_chain->ResizeBuffers(0, g_resize_width, g_resize_height, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_width = g_resize_height = 0;
            create_render_target();
        }
        if (g_pending_dpi_scale > 0) {
            apply_style(g_pending_dpi_scale);
            g_pending_dpi_scale = 0;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_ui();
        ImGui::Render();

        const float clear[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
        g_device_context->OMSetRenderTargets(1, &g_render_target_view, nullptr);
        g_device_context->ClearRenderTargetView(g_render_target_view, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_swap_chain->Present(1, 0);
        g_swap_chain_occluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    app.running = false;
    app.reader.stop();
    app.sender.set_active(false);
    app.sender.stop();
    if (app.config_dirty) {
        save_now();
    }

    app.snapshot.reset();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    WSACleanup();
    CoUninitialize();
    return 0;
}
