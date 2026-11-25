#include "ui/settings_window.h"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

#include "ui/draw.h"

namespace usb::ui {
namespace {

constexpr float kWidth = 400.0f;
constexpr float kHeaderHeight = 52.0f;
constexpr float kCardTop = 58.0f;
constexpr float kRowHeight = 46.0f;
constexpr float kSubRowHeight = 38.0f;
constexpr float kSubIndent = 20.0f;
constexpr float kFooterHeight = 36.0f;
constexpr float kSidePad = 14.0f;
constexpr float kToggleWidth = 42.0f;
constexpr float kToggleHeight = 23.0f;
constexpr float kSubToggleWidth = 36.0f;
constexpr float kSubToggleHeight = 20.0f;
constexpr float kChoiceWidth = 132.0f;
constexpr float kChoiceHeight = 27.0f;
constexpr float kShortcutWidth = 160.0f;
constexpr float kHintRowHeight = 26.0f;

// 命中区域只覆盖控件本身；仅把高度补到至少 28dip，保证触摸可点。
D2D1_RECT_F ControlHitRect(const D2D1_RECT_F& rect) {
    constexpr float kMinHitHeight = 28.0f;
    const float center = (rect.top + rect.bottom) * 0.5f;
    const float half = std::max((rect.bottom - rect.top) * 0.5f,
                                kMinHitHeight * 0.5f);
    return Rect(rect.left, center - half, rect.right, center + half);
}

// “显示无盘符分区”行的展开箭头：命中只限箭头本身，不再整行触发。
D2D1_RECT_F ChevronHitRect(const D2D1_RECT_F& row_rect) {
    constexpr float kHalf = 14.0f;
    const float center_x = row_rect.right - 16.0f - kToggleWidth - 14.0f;
    const float center_y = (row_rect.top + row_rect.bottom) * 0.5f;
    return Rect(center_x - kHalf, center_y - kHalf, center_x + kHalf,
                center_y + kHalf);
}

enum Row {
    RowAutoStart = 0,
    RowAutoCheckUpdates,
    RowTopMost,
    RowShortcut,
    RowPosition,
    RowCapacity,
    RowCloseAction,
    RowEmptyHide,
    RowEdgeCollapse,
    RowMainHeight,
    RowRefresh,
    RowHiddenPartitions,
    kRowCount,
    RowHiddenEsp = 100,
    RowHiddenMsr,
    RowHiddenOther,
    RowUpdateFrequency = 120,
    RowSilentUpdate,
    RowShortcutHint = 200,
};

bool IsHintRow(int row) { return row == RowShortcutHint; }

bool IsModifierKey(int vk) {
    switch (vk) {
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
            return true;
        default:
            return false;
    }
}

bool IsExtendedKey(int vk) {
    switch (vk) {
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_NUMLOCK:
        case VK_DIVIDE:
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
            return true;
        default:
            return false;
    }
}

std::wstring KeyName(int vk) {
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LONG key_data = static_cast<LONG>(scan & 0xFF) << 16;
    if (IsExtendedKey(vk)) key_data |= 1L << 24;
    wchar_t buffer[64] = {};
    const int length = GetKeyNameTextW(key_data, buffer, 64);
    if (length > 0) {
        return std::wstring(buffer, static_cast<std::size_t>(length));
    }
    if (vk >= 'A' && vk <= 'Z') {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= '0' && vk <= '9') {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    return L"按键";
}

std::wstring FormatHotkey(int mods, int vk) {
    if (vk == 0) return L"未设置";
    std::wstring text;
    if (mods & MOD_CONTROL) text += L"Ctrl+";
    if (mods & MOD_ALT) text += L"Alt+";
    if (mods & MOD_SHIFT) text += L"Shift+";
    if (mods & MOD_WIN) text += L"Win+";
    text += KeyName(vk);
    return text;
}

int CurrentModifiers() {
    int mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) & 0x8000) ||
        (GetKeyState(VK_RWIN) & 0x8000)) {
        mods |= MOD_WIN;
    }
    return mods;
}

struct SubRowSpec {
    int parent;
    int row;
    const wchar_t* label;
    bool choice;  // true 表示这一行是下拉选项，false 表示开关
};

constexpr SubRowSpec kSubRows[] = {
    {RowHiddenPartitions, RowHiddenEsp, L"ESP 分区", false},
    {RowHiddenPartitions, RowHiddenMsr, L"MSR 分区", false},
    {RowHiddenPartitions, RowHiddenOther, L"其他无盘符分区", false},
    {RowAutoCheckUpdates, RowUpdateFrequency, L"更新频率", true},
    {RowAutoCheckUpdates, RowSilentUpdate, L"静默更新", false},
};

const SubRowSpec* FindSubRow(int row) {
    for (const auto& sub : kSubRows) {
        if (sub.row == row) return &sub;
    }
    return nullptr;
}

struct RowSpec {
    const wchar_t* label;
    bool toggle;
};

constexpr RowSpec kRows[kRowCount] = {
    {L"开机自启动", true},
    {L"自动检查更新", true},
    {L"窗口始终置顶", true},
    {L"快捷键", false},
    {L"窗口默认位置", false},
    {L"容量显示风格", false},
    {L"关闭按钮的功能", false},
    {L"未插入 U 盘时自动隐藏", false},
    {L"靠边时隐藏到悬浮窗", true},
    {L"主界面最大高度", false},
    {L"数据刷新间隔", false},
    {L"显示无盘符分区", true},
};

struct Option {
    const wchar_t* text;
    int value;
};

const Option kPositions[] = {
    {L"左上", 0}, {L"左下", 1}, {L"右上", 2}, {L"右下", 3},
    {L"记住上次位置", 4},
};
const Option kCapacities[] = {{L"1024 进制", 1024}, {L"1000 进制", 1000}};
const Option kCloseActions[] = {{L"退出程序", 0}, {L"隐藏窗口", 1}};
const Option kEmptyHide[] = {{L"不隐藏", 0},   {L"立即隐藏", -1}, {L"1 秒", 1},
                             {L"3 秒", 3},    {L"10 秒", 10},
                             {L"20 秒", 20},  {L"30 秒", 30},
                             {L"60 秒", 60}};
const Option kMainHeights[] = {
    {L"40 %", 40}, {L"50 %", 50}, {L"60 %", 60}, {L"70 %", 70},
    {L"80 %", 80}, {L"90 %", 90}, {L"100 %", 100},
};
const Option kRefresh[] = {{L"关闭", 0},  {L"1 秒", 1},  {L"2 秒", 2},
                           {L"3 秒", 3},  {L"5 秒", 5},  {L"10 秒", 10}};
const Option kUpdateFrequencies[] = {{L"软件启动时", 0}, {L"每小时", 1},
                                     {L"每天", 2},      {L"每周", 3},
                                     {L"每月", 4}};

template <std::size_t N>
const Option* FindOption(const Option (&options)[N], int value) {
    for (const auto& option : options) {
        if (option.value == value) return &option;
    }
    return nullptr;
}

template <std::size_t N>
int NearestIndex(const Option (&options)[N], int value) {
    int best = 0;
    int distance = 0x7FFFFFFF;
    for (int i = 0; i < static_cast<int>(N); ++i) {
        const int diff = std::abs(options[i].value - value);
        if (diff < distance) {
            distance = diff;
            best = i;
        }
    }
    return best;
}

bool RowToggleValue(const Settings& settings, int row) {
    switch (row) {
        case RowAutoStart:
            return settings.auto_start;
        case RowTopMost:
            return settings.top_most;
        case RowEdgeCollapse:
            return settings.edge_collapse;
        case RowAutoCheckUpdates:
            return settings.auto_check_updates;
        case RowHiddenPartitions:
            return settings.show_hidden_partitions;
        case RowHiddenEsp:
            return settings.show_esp_partitions;
        case RowHiddenMsr:
            return settings.show_msr_partitions;
        case RowHiddenOther:
            return settings.show_other_hidden_partitions;
        case RowSilentUpdate:
            return settings.silent_update;
        default:
            return false;
    }
}

int CurrentChoiceIndex(const Settings& settings, int row) {
    switch (row) {
        case RowPosition:
            return settings.position_mode == 1
                       ? 4
                       : NearestIndex(kPositions, settings.corner);
        case RowCapacity:
            return settings.capacity_base == 1000 ? 1 : 0;
        case RowCloseAction:
            return settings.close_action == 1 ? 1 : 0;
        case RowEmptyHide:
            return NearestIndex(kEmptyHide, settings.empty_hide_seconds);
        case RowMainHeight:
            return NearestIndex(kMainHeights, settings.main_height_percent);
        case RowRefresh:
            return NearestIndex(kRefresh, settings.refresh_seconds);
        case RowUpdateFrequency:
            return NearestIndex(kUpdateFrequencies, settings.update_frequency);
        default:
            return 0;
    }
}

void ApplyChoice(Settings* settings, int row, int index) {
    switch (row) {
        case RowPosition:
            if (index == 4) {
                settings->position_mode = 1;
            } else {
                settings->position_mode = 0;
                settings->corner = std::clamp(index, 0, 3);
            }
            break;
        case RowCapacity:
            settings->capacity_base = index == 1 ? 1000 : 1024;
            break;
        case RowCloseAction:
            settings->close_action = index == 1 ? 1 : 0;
            break;
        case RowEmptyHide:
            settings->empty_hide_seconds = kEmptyHide[index].value;
            break;
        case RowMainHeight:
            settings->main_height_percent = kMainHeights[index].value;
            break;
        case RowRefresh:
            settings->refresh_seconds = kRefresh[index].value;
            break;
        case RowUpdateFrequency:
            settings->update_frequency = kUpdateFrequencies[index].value;
            break;
        default:
            break;
    }
}

int OptionCount(int row) {
    switch (row) {
        case RowPosition:
            return static_cast<int>(std::size(kPositions));
        case RowCapacity:
            return static_cast<int>(std::size(kCapacities));
        case RowCloseAction:
            return static_cast<int>(std::size(kCloseActions));
        case RowEmptyHide:
            return static_cast<int>(std::size(kEmptyHide));
        case RowMainHeight:
            return static_cast<int>(std::size(kMainHeights));
        case RowRefresh:
            return static_cast<int>(std::size(kRefresh));
        case RowUpdateFrequency:
            return static_cast<int>(std::size(kUpdateFrequencies));
        default:
            return 0;
    }
}

const Option* OptionAt(int row, int index) {
    switch (row) {
        case RowPosition:
            return &kPositions[index];
        case RowCapacity:
            return &kCapacities[index];
        case RowCloseAction:
            return &kCloseActions[index];
        case RowEmptyHide:
            return &kEmptyHide[index];
        case RowMainHeight:
            return &kMainHeights[index];
        case RowRefresh:
            return &kRefresh[index];
        case RowUpdateFrequency:
            return &kUpdateFrequencies[index];
        default:
            return nullptr;
    }
}

const wchar_t* ChoiceText(const Settings& settings, int row) {
    const int index = CurrentChoiceIndex(settings, row);
    const Option* option = OptionAt(row, index);
    return option ? option->text : L"";
}

float SystemDpi() {
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<float>(dpi) : 96.0f;
}

float WindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            fn = reinterpret_cast<GetDpiForWindowFn>(
                reinterpret_cast<void*>(
                    GetProcAddress(user32, "GetDpiForWindow")));
        }
    }
    if (fn && hwnd) {
        const UINT dpi = fn(hwnd);
        if (dpi > 0) return static_cast<float>(dpi);
    }
    return SystemDpi();
}

RECT WorkAreaForWindow(HWND hwnd) {
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    const HMONITOR monitor =
        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &info)) return info.rcWork;
    RECT fallback = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0);
    return fallback;
}

COLORREF BlendOver(const D2D1_COLOR_F& foreground,
                   const D2D1_COLOR_F& background) {
    const float alpha = std::min(std::max(foreground.a, 0.0f), 1.0f);
    const float red = foreground.r * alpha + background.r * (1.0f - alpha);
    const float green = foreground.g * alpha + background.g * (1.0f - alpha);
    const float blue = foreground.b * alpha + background.b * (1.0f - alpha);
    return RGB(static_cast<BYTE>(red * 255.0f),
               static_cast<BYTE>(green * 255.0f),
               static_cast<BYTE>(blue * 255.0f));
}

// ---------------------------------------------------------------------------
// 自绘下拉菜单：与托盘菜单同一套视觉语言，用临时弹出窗口 Modal 等待选择。
// ---------------------------------------------------------------------------
constexpr float kMenuItemHeight = 32.0f;
constexpr float kMenuPad = 6.0f;

class ChoicePopup {
public:
    int Show(HINSTANCE instance, HWND owner, const D2D1_RECT_F& anchor,
             float min_width, const std::vector<std::wstring>& items,
             int selected, const Palette& palette, float scale) {
        if (!owner || items.empty()) return -1;
        palette_ = palette;
        scale_ = scale > 0.0f ? scale : 1.0f;
        items_ = items;
        selected_ = selected;
        hover_ = -1;
        choice_ = -1;
        done_ = false;
        tracking_leave_ = false;

        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW window_class = {};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = &ChoicePopup::WndProc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.lpszClassName = L"UsbAssistantChoicePopup";
            RegisterClassExW(&window_class);
            registered = true;
        }

        const int width =
            static_cast<int>(std::lround(std::max(min_width, 148.0f) * scale_));
        const int height = static_cast<int>(
            std::lround((kMenuPad * 2.0f +
                         kMenuItemHeight * static_cast<float>(items.size())) *
                        scale_));

        POINT top_left = {static_cast<LONG>(std::lround(anchor.left * scale_)),
                          static_cast<LONG>(std::lround(anchor.top * scale_))};
        POINT bottom_left = {
            static_cast<LONG>(std::lround(anchor.left * scale_)),
            static_cast<LONG>(std::lround(anchor.bottom * scale_))};
        ClientToScreen(owner, &top_left);
        ClientToScreen(owner, &bottom_left);

        const RECT work = WorkAreaForWindow(owner);
        const int gap = static_cast<int>(std::lround(4.0f * scale_));
        int x = bottom_left.x;
        int y = bottom_left.y + gap;
        if (x + width > work.right - 4) x = work.right - 4 - width;
        if (x < work.left + 4) x = work.left + 4;
        // 下方放不下就翻到锚点上方，再不行就贴住屏幕边缘。
        if (y + height > work.bottom - 4) y = top_left.y - gap - height;
        if (y < work.top + 4) y = work.top + 4;
        if (y + height > work.bottom - 4) y = work.bottom - 4 - height;

        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                L"UsbAssistantChoicePopup", L"", WS_POPUP, x, y,
                                width, height, nullptr, nullptr, instance, this);
        if (!hwnd_) return -1;

        const int radius = static_cast<int>(std::lround(10.0f * scale_));
        SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                               radius, radius),
                     FALSE);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);

        bool previous_down = false;
        // 下拉是在鼠标按下时展开的：先等这次按键松开，再启用“点外面关闭”，
        // 否则初始的抬起事件会立刻把菜单关掉。
        bool armed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0;
        while (!done_) {
            MSG message = {};
            MsgWaitForMultipleObjectsEx(0, nullptr, 40, QS_ALLINPUT,
                                        MWMO_INPUTAVAILABLE);
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(message.wParam));
                    Finish(-1);
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (done_) break;

            // 每次循环都检查，而不是只在等待超时时检查：触摸输入会持续产生
            // WM_POINTERUPDATE（约 8ms 一条），按下期间等待几乎不会超时，
            // 只在超时时检查会让外部点击永远关不掉菜单。
            const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                              (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            const bool escape = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            if (escape) {
                Finish(-1);
                break;
            }
            if (!armed) {
                if (!down) armed = true;
                previous_down = down;
                continue;
            }
            POINT cursor = {};
            GetCursorPos(&cursor);
            RECT rect = {};
            if (hwnd_) GetWindowRect(hwnd_, &rect);
            const bool inside = hwnd_ && PtInRect(&rect, cursor) != FALSE;
            if ((down && !inside) || (previous_down && !down && !inside)) {
                if (down) {
                    outside_click_ = true;
                    outside_left_ =
                        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                    outside_point_ = cursor;
                }
                Finish(-1);
                break;
            }
            previous_down = down;
        }
        return choice_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
        ChoicePopup* self = reinterpret_cast<ChoicePopup*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = reinterpret_cast<ChoicePopup*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        return self->HandleMessage(hwnd, message, wparam, lparam);
    }

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                          LPARAM lparam) {
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT paint = {};
                const HDC dc = BeginPaint(hwnd, &paint);
                Paint(dc);
                EndPaint(hwnd, &paint);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_MOUSEMOVE: {
                if (!tracking_leave_) {
                    TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
                    TrackMouseEvent(&track);
                    tracking_leave_ = true;
                }
                const int hit = HitItem(GET_Y_LPARAM(lparam));
                if (hit != hover_) {
                    hover_ = hit;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                SetCursor(LoadCursorW(nullptr,
                                      hit >= 0 ? IDC_HAND : IDC_ARROW));
                return 0;
            }
            case WM_MOUSELEAVE:
                tracking_leave_ = false;
                if (hover_ != -1) {
                    hover_ = -1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_LBUTTONUP: {
                Finish(HitItem(GET_Y_LPARAM(lparam)));
                return 0;
            }
            case WM_RBUTTONUP:
                Finish(-1);
                return 0;
            case WM_CAPTURECHANGED:
                if (!done_) Finish(-1);
                return 0;
            case WM_NCDESTROY:
                hwnd_ = nullptr;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                return DefWindowProcW(hwnd, message, wparam, lparam);
            default:
                break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void Paint(HDC dc) {
        RECT rect = {};
        GetClientRect(hwnd_, &rect);
        const int width = rect.right - rect.left;

        const COLORREF background = ToColorRef(palette_.card);
        HBRUSH brush = CreateSolidBrush(background);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);

        SetBkMode(dc, TRANSPARENT);
        HFONT font = CreateFontW(
            -static_cast<int>(std::lround(13.0f * scale_)), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
            L"Microsoft YaHei UI");
        const HGDIOBJ old_font = SelectObject(dc, font);

        const int pad = static_cast<int>(std::lround(kMenuPad * scale_));
        const int inset = static_cast<int>(std::lround(6.0f * scale_));
        const int row_height =
            static_cast<int>(std::lround(kMenuItemHeight * scale_));

        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            const int top = pad + row_height * i;
            const int bottom = top + row_height;
            const bool selected = i == selected_;
            const bool hovered = i == hover_;

            if (hovered) {
                const COLORREF fill =
                    BlendOver(palette_.hover_fill, palette_.card);
                HBRUSH hover_brush = CreateSolidBrush(fill);
                HPEN hover_pen = CreatePen(PS_SOLID, 0, fill);
                const HGDIOBJ old_brush = SelectObject(dc, hover_brush);
                const HGDIOBJ old_pen = SelectObject(dc, hover_pen);
                const int radius = static_cast<int>(std::lround(8.0f * scale_));
                const int right = width - inset;
                RoundRect(dc, inset, top, right, bottom, radius, radius);
                SelectObject(dc, old_brush);
                SelectObject(dc, old_pen);
                DeleteObject(hover_brush);
                DeleteObject(hover_pen);
            }

            if (selected) {
                const int check_x = width - static_cast<int>(
                                                std::lround(18.0f * scale_));
                const int center_y = (top + bottom) / 2;
                HPEN pen = CreatePen(PS_SOLID, 2, ToColorRef(palette_.accent));
                const HGDIOBJ old_pen = SelectObject(dc, pen);
                MoveToEx(dc, check_x - static_cast<int>(std::lround(4.0f * scale_)),
                         center_y, nullptr);
                LineTo(dc, check_x, center_y +
                                       static_cast<int>(std::lround(4.0f * scale_)));
                LineTo(dc, check_x + static_cast<int>(std::lround(5.0f * scale_)),
                       center_y - static_cast<int>(std::lround(5.0f * scale_)));
                SelectObject(dc, old_pen);
                DeleteObject(pen);
            }

            SetTextColor(dc, ToColorRef(selected ? palette_.accent
                                                 : palette_.text_primary));
            RECT text_rect = {inset + static_cast<int>(std::lround(10.0f * scale_)),
                              top, width - static_cast<int>(
                                                std::lround(28.0f * scale_)),
                              bottom};
            DrawTextW(dc, items_[i].c_str(), -1, &text_rect,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        }

        SelectObject(dc, old_font);
        DeleteObject(font);
    }

    int HitItem(int y_px) const {
        const float y = static_cast<float>(y_px) / scale_;
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            const float top = kMenuPad + kMenuItemHeight * static_cast<float>(i);
            if (y >= top && y <= top + kMenuItemHeight) return i;
        }
        return -1;
    }

    void Finish(int choice) {
        choice_ = choice;
        done_ = true;
        if (hwnd_) {
            HWND hwnd = hwnd_;
            hwnd_ = nullptr;
            DestroyWindow(hwnd);
        }
    }

    HWND hwnd_ = nullptr;
    Palette palette_ = MakePalette(false);
    float scale_ = 1.0f;
    std::vector<std::wstring> items_;
    int selected_ = -1;
    int hover_ = -1;
    int choice_ = -1;
    bool done_ = false;
    bool tracking_leave_ = false;
    // 记录“点击弹层外部”的落点：关闭后由拥有者补发一次按下，避免这次点击
    // 被菜单的模态循环吞掉。
    bool outside_click_ = false;
    bool outside_left_ = false;
    POINT outside_point_ = {};

   public:
    bool outside_click() const { return outside_click_; }
    bool outside_left_button() const { return outside_left_; }
    POINT outside_click_point() const { return outside_point_; }
};

}  // namespace

// ---------------------------------------------------------------------------
// 窗口生命周期
// ---------------------------------------------------------------------------

SettingsWindow::~SettingsWindow() { ReleaseTarget(); }

bool SettingsWindow::Open(HINSTANCE instance, HWND owner, ID2D1Factory* d2d,
                          IDWriteFactory* dwrite, const Fonts* fonts,
                          const Palette& palette, const Settings& settings,
                          ChangeHandler on_change, HotkeyHandler on_hotkey,
                          CloseHandler on_close) {
    if (hwnd_) {
        settings_ = settings;
        palette_ = palette;
        on_change_ = std::move(on_change);
        on_hotkey_ = std::move(on_hotkey);
        on_close_ = std::move(on_close);
        dragging_ = false;
        tracking_leave_ = false;
        hover_row_ = -1;
        close_hover_ = false;
        clear_hover_ = false;
        capturing_hotkey_ = false;
        scroll_hover_ = false;
        scroll_dragging_ = false;
        touch_active_ = false;
        touch_scroll_drag_ = false;
        touch_pending_ = false;
        touch_panning_ = false;
        ShowWindow(hwnd_, SW_SHOW);
        RaiseToFront();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    instance_ = instance;
    d2d_ = d2d;
    dwrite_ = dwrite;
    fonts_ = fonts;
    palette_ = palette;
    settings_ = settings;
    on_change_ = std::move(on_change);
    on_hotkey_ = std::move(on_hotkey);
    on_close_ = std::move(on_close);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &SettingsWindow::WndProc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = L"UsbAssistantSettings";
        RegisterClassExW(&window_class);
        registered = true;
    }

    scale_ = WindowDpi(owner) / 96.0f;
    if (scale_ <= 0.0f) scale_ = 1.0f;

    const float full_height =
        kCardTop + BaseContentHeight() + kFooterHeight;
    RECT work_area = WorkAreaForWindow(owner);
    const float available = (work_area.bottom - work_area.top) / scale_ * 0.86f;
    view_height_ = std::min(full_height, std::max(320.0f, available));
    UpdateScrollBounds();
    scroll_ = 0.0f;

    const int width = static_cast<int>(std::lround(kWidth * scale_));
    const int height = static_cast<int>(std::lround(view_height_ * scale_));
    const int left = work_area.left +
                     ((work_area.right - work_area.left) - width) / 2;
    const int top = work_area.top +
                    ((work_area.bottom - work_area.top) - height) / 2;

    // 不设置 owner：打开时由 RaiseToFront 临时置顶并激活，随后恢复普通层级。
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"UsbAssistantSettings", L"设置",
                            WS_POPUP, left, top, width, height, nullptr, nullptr,
                            instance, this);
    if (!hwnd_) return false;
    // 注册触摸：滚动条与内容区都支持手指拖动。
    RegisterTouchWindow(hwnd_, 0);

    const int radius = static_cast<int>(std::lround(16.0f * scale_));
    SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, width + 1, height + 1, radius,
                                           radius),
                 FALSE);
    const BOOL dark = palette_.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));  // DWMWA_USE_IMMERSIVE_DARK_MODE

    ShowWindow(hwnd_, SW_SHOW);
    RaiseToFront();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void SettingsWindow::Close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool SettingsWindow::IsOpen() const { return hwnd_ != nullptr; }

void SettingsWindow::RaiseToFront() {
    if (!hwnd_) return;
    // 临时置顶保证 z 序，附加到前台线程后激活，随后恢复普通层级：
    // 不保留置顶状态，主窗口（即使置顶）可以重新遮挡本窗口。
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const HWND foreground = GetForegroundWindow();
    const DWORD foreground_thread =
        foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD current_thread = GetCurrentThreadId();
    const bool attached =
        foreground_thread != 0 && foreground_thread != current_thread &&
        AttachThreadInput(foreground_thread, current_thread, TRUE) != FALSE;
    SetForegroundWindow(hwnd_);
    if (attached) {
        AttachThreadInput(foreground_thread, current_thread, FALSE);
    }
    SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void SettingsWindow::SetPalette(const Palette& palette) {
    palette_ = palette;
    if (hwnd_) {
        const BOOL dark = palette_.dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void SettingsWindow::ApplyScale() {
    scale_ = WindowDpi(hwnd_) / 96.0f;
    if (scale_ <= 0.0f) scale_ = 1.0f;
    ResizeWindow();
}

void SettingsWindow::ResizeWindow() {
    if (!hwnd_) return;
    const int width = static_cast<int>(std::lround(kWidth * scale_));
    const int height = static_cast<int>(std::lround(view_height_ * scale_));
    RECT current = {};
    GetWindowRect(hwnd_, &current);
    SetWindowPos(hwnd_, nullptr, current.left, current.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    const int radius = static_cast<int>(std::lround(16.0f * scale_));
    SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, width + 1, height + 1, radius,
                                           radius),
                 TRUE);
    UpdateScrollBounds();
    if (target_) target_->Resize(D2D1::SizeU(width, height));
}

bool SettingsWindow::EnsureTarget() {
    if (!d2d_ || !hwnd_) return false;
    RECT client = {};
    GetClientRect(hwnd_, &client);
    const UINT32 width =
        static_cast<UINT32>(std::max<LONG>(1, client.right - client.left));
    const UINT32 height =
        static_cast<UINT32>(std::max<LONG>(1, client.bottom - client.top));
    const D2D1_SIZE_U size = D2D1::SizeU(width, height);
    if (target_) {
        const D2D1_SIZE_U current = target_->GetPixelSize();
        if (current.width != size.width || current.height != size.height) {
            target_->Resize(size);
        }
        return true;
    }

    D2D1_HWND_RENDER_TARGET_PROPERTIES properties =
        D2D1::HwndRenderTargetProperties(hwnd_, size);
    const D2D1_RENDER_TARGET_PROPERTIES render_properties =
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(d2d_->CreateHwndRenderTarget(render_properties, properties,
                                            &target_))) {
        target_ = nullptr;
        return false;
    }
    return true;
}

void SettingsWindow::ReleaseTarget() {
    if (target_) {
        target_->Release();
        target_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

void SettingsWindow::Paint() {
    if (!EnsureTarget() || !fonts_) return;
    target_->SetDpi(96.0f * scale_, 96.0f * scale_);
    target_->BeginDraw();
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target_->Clear(palette_.panel);

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(target_->CreateSolidColorBrush(palette_.text_primary, &brush)) ||
        !brush) {
        target_->EndDraw();
        return;
    }

    DrawText(target_, brush, L"设置", fonts_->title,
             Rect(kSidePad + 4.0f, 12.0f, kWidth - 60.0f, 38.0f),
             palette_.text_primary);

    const D2D1_RECT_F close = CloseRect();
    if (close_hover_) {
        FillRound(target_, brush, close, 13.0f,
                  WithAlpha(palette_.hover_fill, 1.0f));
    }
    DrawCloseGlyph(target_, brush, close, palette_.text_secondary, 9.0f);

    const D2D1_RECT_F card = Rect(kSidePad, kCardTop, kWidth - kSidePad,
                                  view_height_ - kFooterHeight + 4.0f);
    FillRound(target_, brush, card, 14.0f, palette_.card);
    StrokeRound(target_, brush, card, 14.0f, palette_.card_border, 1.0f);

    const D2D1_RECT_F clip = Rect(card.left, card.top + 1.0f, card.right,
                                  card.bottom - 1.0f);
    target_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    PaintRows(target_, brush);
    target_->PopAxisAlignedClip();

    if (scroll_max_ > 0.5f) {
        const bool active = scroll_hover_ || scroll_dragging_;
        const D2D1_RECT_F track = ScrollTrackRect();
        const float radius = (track.right - track.left) * 0.5f;
        FillRound(target_, brush, track, radius,
                  WithAlpha(palette_.text_primary,
                            active ? 0.16f : 0.10f));
        const D2D1_RECT_F thumb = ScrollThumbRect();
        FillRound(target_, brush, thumb, radius,
                  WithAlpha(palette_.text_primary,
                            active ? 0.54f : 0.32f));
    }

    DrawText(target_, brush, L"设置会自动保存到 config.ini", fonts_->meta,
             Rect(kSidePad + 4.0f, view_height_ - 28.0f,
                  kWidth - kSidePad, view_height_ - 6.0f),
             palette_.text_tertiary);

    brush->Release();
    const HRESULT result = target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) ReleaseTarget();
}

void SettingsWindow::PaintRows(ID2D1RenderTarget* target,
                               ID2D1SolidColorBrush* brush) {
    const std::vector<int> order = RowOrder();
    for (std::size_t i = 0; i < order.size(); ++i) {
        const int row = order[i];
        const D2D1_RECT_F rect = RowRect(row);
        if (rect.bottom < kCardTop || rect.top > view_height_) continue;
        const bool enabled =
            !IsHintRow(row) &&
            (!IsSubRow(row) ||
             RowToggleValue(settings_, SubRowParent(row)));
        PaintRow(target, brush, row, rect, row == hover_row_, enabled);
        if (!IsSubRow(row) && !IsHintRow(row) && i + 1 < order.size() &&
            !IsSubRow(order[i + 1]) && !IsHintRow(order[i + 1])) {
            brush->SetColor(WithAlpha(palette_.text_primary, 0.05f));
            target->DrawLine({rect.left + 16.0f, rect.bottom},
                             {rect.right - 16.0f, rect.bottom}, brush, 1.0f);
        }
    }
}

void SettingsWindow::PaintRow(ID2D1RenderTarget* target,
                              ID2D1SolidColorBrush* brush, int row,
                              const D2D1_RECT_F& rect, bool hovered,
                              bool enabled) {
    if (IsHintRow(row)) {
        DrawText(target, brush, L"快捷键用于快速显示或隐藏主面板",
                 fonts_->meta,
                 Rect(rect.left + 16.0f, rect.top, rect.right - 16.0f,
                      rect.bottom),
                 palette_.text_tertiary);
        return;
    }

    if (hovered && enabled) {
        FillRound(target, brush, rect, 8.0f,
                  WithAlpha(palette_.hover_fill, 0.7f));
    }

    if (IsSubRow(row)) {
        // 下拉子行的右侧要给选项按钮留出与主行一致的空间。
        const float label_right =
            rect.right - (SubRowIsChoice(row) ? 160.0f : 68.0f);
        DrawText(target, brush, SubRowLabel(row), fonts_->dialog_row,
                 Rect(rect.left + 12.0f, rect.top, label_right, rect.bottom),
                 enabled ? palette_.text_secondary : palette_.text_tertiary);

        if (SubRowIsChoice(row)) {
            PaintChoice(target, brush, row, enabled);
            return;
        }

        const bool on = RowToggleValue(settings_, row);
        if (enabled) {
            DrawSwitch(target, brush, ToggleRect(row), on, palette_.accent,
                       WithAlpha(palette_.text_tertiary, 0.28f),
                       palette_.card);
        } else {
            DrawSwitch(target, brush, ToggleRect(row), on,
                       WithAlpha(palette_.accent, 0.35f),
                       WithAlpha(palette_.text_tertiary, 0.16f),
                       WithAlpha(palette_.card, 0.75f));
        }
        return;
    }

    DrawText(target, brush, kRows[row].label, fonts_->part_name,
             Rect(rect.left + 16.0f, rect.top, rect.right - 160.0f,
                  rect.bottom),
             palette_.text_primary);

    if (row == RowShortcut) {
        const D2D1_RECT_F pill = ShortcutRect(row);
        const bool has_value = settings_.hotkey_vk != 0;
        FillRound(target, brush, pill, 8.0f,
                  capturing_hotkey_ ? palette_.accent_soft
                                    : palette_.secondary_button);
        if (capturing_hotkey_) {
            brush->SetColor(WithAlpha(palette_.accent, 0.85f));
            target->DrawRoundedRectangle(
                D2D1::RoundedRect(pill, 8.0f, 8.0f), brush, 1.2f);
        }

        const bool show_clear = has_value || capturing_hotkey_;
        D2D1_RECT_F text_rect = pill;
        if (show_clear) text_rect.right -= 25.0f;
        const std::wstring text =
            capturing_hotkey_
                ? L"请按下快捷键…"
                : FormatHotkey(settings_.hotkey_mods, settings_.hotkey_vk);
        DrawText(target, brush, text.c_str(), fonts_->button, text_rect,
                 capturing_hotkey_ ? palette_.accent
                                   : (has_value ? palette_.text_primary
                                                : palette_.text_tertiary));

        if (show_clear) {
            const D2D1_RECT_F clear = ShortcutClearRect(row);
            if (clear_hover_) {
                FillRound(target, brush, clear, 6.0f,
                          WithAlpha(palette_.danger, 0.10f));
            }
            const float cx = (clear.left + clear.right) * 0.5f;
            const float cy = (clear.top + clear.bottom) * 0.5f;
            brush->SetColor(clear_hover_
                                ? palette_.danger
                                : WithAlpha(palette_.text_secondary, 0.85f));
            target->DrawLine({cx - 3.5f, cy - 3.5f}, {cx + 3.5f, cy + 3.5f},
                             brush, 1.4f);
            target->DrawLine({cx + 3.5f, cy - 3.5f}, {cx - 3.5f, cy + 3.5f},
                             brush, 1.4f);
        }
        return;
    }

    if (kRows[row].toggle) {
        DrawSwitch(target, brush, ToggleRect(row),
                   RowToggleValue(settings_, row), palette_.accent,
                   WithAlpha(palette_.text_tertiary, 0.28f),
                   palette_.card);
        if (IsExpandable(row)) {
            const float x = rect.right - 16.0f - kToggleWidth - 14.0f;
            const float y = (rect.top + rect.bottom) * 0.5f;
            brush->SetColor(WithAlpha(palette_.text_tertiary, 0.85f));
            if (IsExpanded(row)) {
                target->DrawLine({x - 3.5f, y - 1.5f}, {x, y + 2.0f}, brush,
                                 1.4f);
                target->DrawLine({x, y + 2.0f}, {x + 3.5f, y - 1.5f}, brush,
                                 1.4f);
            } else {
                target->DrawLine({x - 1.5f, y - 3.5f}, {x + 2.0f, y}, brush,
                                 1.4f);
                target->DrawLine({x + 2.0f, y}, {x - 1.5f, y + 3.5f}, brush,
                                 1.4f);
            }
        }
        return;
    }

    PaintChoice(target, brush, row, true);
}

void SettingsWindow::PaintChoice(ID2D1RenderTarget* target,
                                 ID2D1SolidColorBrush* brush, int row,
                                 bool enabled) {
    const D2D1_RECT_F pill = ChoiceRect(row);
    const bool open = enabled && row == open_row_;
    FillRound(target, brush, pill, 8.0f,
              open ? palette_.accent_soft : palette_.secondary_button);
    DrawText(target, brush, ChoiceText(settings_, row), fonts_->button, pill,
             enabled ? (open ? palette_.accent : palette_.secondary_text)
                     : palette_.text_tertiary);

    // 右侧下拉箭头
    const float x = pill.right - 14.0f;
    const float y = (pill.top + pill.bottom) * 0.5f;
    brush->SetColor(WithAlpha(open ? palette_.accent : palette_.secondary_text,
                              enabled ? 0.75f : 0.35f));
    target->DrawLine({x - 3.0f, y - 1.5f}, {x, y + 1.5f}, brush, 1.2f);
    target->DrawLine({x, y + 1.5f}, {x + 3.0f, y - 1.5f}, brush, 1.2f);
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------

D2D1_RECT_F SettingsWindow::RowRect(int row) const {
    float top = kCardTop + 8.0f - scroll_;
    for (const int candidate : RowOrder()) {
        const float height = IsHintRow(candidate)
                                 ? kHintRowHeight
                                 : (IsSubRow(candidate) ? kSubRowHeight
                                                        : kRowHeight);
        if (candidate == row) {
            const float left = kSidePad + 8.0f +
                               (IsSubRow(row) ? kSubIndent : 0.0f);
            return Rect(left, top, kWidth - kSidePad - 8.0f, top + height);
        }
        top += height;
    }
    return {};
}

bool SettingsWindow::IsSubRow(int row) {
    return row >= RowHiddenEsp && row < RowShortcutHint;
}

int SettingsWindow::SubRowParent(int row) {
    const SubRowSpec* sub = FindSubRow(row);
    return sub ? sub->parent : -1;
}

bool SettingsWindow::SubRowIsChoice(int row) {
    const SubRowSpec* sub = FindSubRow(row);
    return sub ? sub->choice : false;
}

const wchar_t* SettingsWindow::SubRowLabel(int row) {
    const SubRowSpec* sub = FindSubRow(row);
    return sub ? sub->label : L"";
}

// 带展开箭头的父行：显示无盘符分区、自动检查更新，默认都折叠。
bool SettingsWindow::IsExpandable(int row) {
    return row == RowHiddenPartitions || row == RowAutoCheckUpdates;
}

bool SettingsWindow::IsExpanded(int row) const {
    if (row == RowHiddenPartitions) return hidden_expanded_;
    if (row == RowAutoCheckUpdates) return update_expanded_;
    return false;
}

void SettingsWindow::ToggleExpanded(int row) {
    if (row == RowHiddenPartitions) {
        hidden_expanded_ = !hidden_expanded_;
    } else if (row == RowAutoCheckUpdates) {
        update_expanded_ = !update_expanded_;
    } else {
        return;
    }
    UpdateScrollBounds();

    // 展开后只把新出现的子行滚进可视区，避免整页跳到最底部把父行顶出视野。
    int last_sub = -1;
    for (const auto& sub : kSubRows) {
        if (sub.parent == row) last_sub = sub.row;
    }
    if (IsExpanded(row) && last_sub >= 0) {
        const float card_bottom = view_height_ - kFooterHeight + 4.0f;
        const float overflow = RowRect(last_sub).bottom - card_bottom;
        if (overflow > 0.0f) {
            SetScroll(scroll_ + overflow);
            return;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::vector<int> SettingsWindow::RowOrder() const {
    std::vector<int> order;
    order.reserve(kRowCount + std::size(kSubRows) + 1);
    for (int row = 0; row < kRowCount; ++row) {
        order.push_back(row);
        if (row == RowShortcut) order.push_back(RowShortcutHint);
        if (!IsExpanded(row)) continue;
        for (const auto& sub : kSubRows) {
            if (sub.parent == row) order.push_back(sub.row);
        }
    }
    return order;
}

float SettingsWindow::ContentHeight() const {
    float height = 8.0f + 8.0f;
    for (const int row : RowOrder()) {
        height += IsHintRow(row) ? kHintRowHeight
                                 : (IsSubRow(row) ? kSubRowHeight
                                                  : kRowHeight);
    }
    return height;
}

float SettingsWindow::BaseContentHeight() const {
    return 8.0f + 8.0f + kRowHeight * static_cast<float>(kRowCount) +
           kHintRowHeight;
}

void SettingsWindow::UpdateScrollBounds() {
    const float card_height = (view_height_ - kFooterHeight + 4.0f) - kCardTop;
    scroll_max_ = std::max(0.0f, ContentHeight() - card_height);
    scroll_ = std::min(std::max(scroll_, 0.0f), scroll_max_);
}

D2D1_RECT_F SettingsWindow::ToggleRect(int row) const {
    const D2D1_RECT_F rect = RowRect(row);
    const float center = (rect.top + rect.bottom) * 0.5f;
    const float width = IsSubRow(row) ? kSubToggleWidth : kToggleWidth;
    const float height = IsSubRow(row) ? kSubToggleHeight : kToggleHeight;
    return Rect(rect.right - 16.0f - width, center - height * 0.5f,
                rect.right - 16.0f, center + height * 0.5f);
}

D2D1_RECT_F SettingsWindow::ChoiceRect(int row) const {
    const D2D1_RECT_F rect = RowRect(row);
    const float center = (rect.top + rect.bottom) * 0.5f;
    return Rect(rect.right - 16.0f - kChoiceWidth, center - kChoiceHeight * 0.5f,
                rect.right - 16.0f, center + kChoiceHeight * 0.5f);
}

D2D1_RECT_F SettingsWindow::ShortcutRect(int row) const {
    const D2D1_RECT_F rect = RowRect(row);
    const float center = (rect.top + rect.bottom) * 0.5f;
    return Rect(rect.right - 16.0f - kShortcutWidth,
                center - kChoiceHeight * 0.5f, rect.right - 16.0f,
                center + kChoiceHeight * 0.5f);
}

D2D1_RECT_F SettingsWindow::ShortcutClearRect(int row) const {
    const D2D1_RECT_F pill = ShortcutRect(row);
    constexpr float kSize = 20.0f;
    const float top = (pill.top + pill.bottom - kSize) * 0.5f;
    return Rect(pill.right - kSize - 3.0f, top, pill.right - 3.0f,
                top + kSize);
}

D2D1_RECT_F SettingsWindow::CloseRect() const {
    return Rect(kWidth - kSidePad - 28.0f, 12.0f, kWidth - kSidePad, 40.0f);
}

int SettingsWindow::HitRow(float x, float y) const {
    if (x < kSidePad || x > kWidth - kSidePad) return -1;
    if (y < kCardTop || y > view_height_ - kFooterHeight + 4.0f) return -1;
    for (const int row : RowOrder()) {
        if (Contains(RowRect(row), x, y)) return row;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// 设置变更
// ---------------------------------------------------------------------------

void SettingsWindow::ToggleRow(int row) {
    if (row == RowAutoStart) {
        const bool next = !settings_.auto_start;
        if (!SettingsStore::SetAutoStart(next)) {
            MessageBoxW(hwnd_, L"无法修改开机自启动快捷方式，请检查启动文件夹权限。",
                        L"U 盘助手", MB_ICONWARNING | MB_OK);
            return;
        }
        settings_.auto_start = next;
    } else if (row == RowTopMost) {
        settings_.top_most = !settings_.top_most;
    } else if (row == RowEdgeCollapse) {
        settings_.edge_collapse = !settings_.edge_collapse;
    } else if (row == RowAutoCheckUpdates) {
        settings_.auto_check_updates = !settings_.auto_check_updates;
    } else if (row == RowHiddenPartitions) {
        settings_.show_hidden_partitions = !settings_.show_hidden_partitions;
    } else if (row == RowHiddenEsp) {
        if (!settings_.show_hidden_partitions) return;
        settings_.show_esp_partitions = !settings_.show_esp_partitions;
    } else if (row == RowHiddenMsr) {
        if (!settings_.show_hidden_partitions) return;
        settings_.show_msr_partitions = !settings_.show_msr_partitions;
    } else if (row == RowHiddenOther) {
        if (!settings_.show_hidden_partitions) return;
        settings_.show_other_hidden_partitions =
            !settings_.show_other_hidden_partitions;
    } else if (row == RowSilentUpdate) {
        if (!settings_.auto_check_updates) return;
        settings_.silent_update = !settings_.silent_update;
    } else {
        return;
    }
    NotifyChange();
}

void SettingsWindow::ChooseRow(int row) {
    const int count = OptionCount(row);
    if (count <= 0) return;

    std::vector<std::wstring> items;
    items.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const Option* option = OptionAt(row, i);
        items.push_back(option ? option->text : L"");
    }
    const int current = CurrentChoiceIndex(settings_, row);

    ChoicePopup popup;
    menu_open_ = true;
    open_row_ = row;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    const int index = popup.Show(instance_, hwnd_, ChoiceRect(row),
                                 kChoiceWidth, items, current, palette_, scale_);
    open_row_ = -1;
    menu_open_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);

    // 菜单打开期间设置窗口会忽略输入，若不补发，关闭菜单的这一下就白点了。
    // 标题栏拖动需要连续的按下/移动，交给下一次手势；再次点同一个下拉则只关闭。
    if (popup.outside_click() && popup.outside_left_button()) {
        POINT point = popup.outside_click_point();
        ScreenToClient(hwnd_, &point);
        RECT client = {};
        GetClientRect(hwnd_, &client);
        const D2D1_RECT_F choice = ChoiceRect(row);
        const float dip_x = static_cast<float>(point.x) / scale_;
        const float dip_y = static_cast<float>(point.y) / scale_;
        const bool on_current = dip_x >= choice.left && dip_x <= choice.right &&
                                dip_y >= choice.top && dip_y <= choice.bottom;
        if (PtInRect(&client, point) && dip_y >= kHeaderHeight &&
            !on_current) {
            PostMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON,
                         MAKELPARAM(point.x, point.y));
        }
    }
    if (index >= 0 && index != current) {
        ApplyChoice(&settings_, row, index);
        NotifyChange();
    }
}

void SettingsWindow::HandleHotkeyCapture(int vk) {
    if (vk == VK_ESCAPE) {
        capturing_hotkey_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (vk == VK_PROCESSKEY || IsModifierKey(vk)) return;

    const int mods = CurrentModifiers();
    if ((mods & (MOD_CONTROL | MOD_ALT | MOD_WIN)) == 0) {
        MessageBoxW(hwnd_,
                    L"全局快捷键需要同时按住 Ctrl、Alt 或 Win 键，\n"
                    L"否则会影响正常打字。按 Esc 可取消。",
                    L"U 盘助手", MB_ICONINFORMATION | MB_OK);
        return;
    }
    ApplyHotkey(mods, vk);
}

void SettingsWindow::ClearHotkey() { ApplyHotkey(0, 0); }

void SettingsWindow::ApplyHotkey(int mods, int vk) {
    const std::wstring error =
        on_hotkey_ ? on_hotkey_(mods, vk) : std::wstring();
    if (!error.empty()) {
        MessageBoxW(hwnd_, error.c_str(), L"U 盘助手",
                    MB_ICONWARNING | MB_OK);
        capturing_hotkey_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    settings_.hotkey_mods = mods;
    settings_.hotkey_vk = vk;
    capturing_hotkey_ = false;
    NotifyChange();
}

void SettingsWindow::NotifyChange() {
    settings_.Normalize();
    if (on_change_) on_change_(settings_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::SetScroll(float value) {
    scroll_ = std::min(std::max(value, 0.0f), scroll_max_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// 滚动条：鼠标与触摸共用同一套几何和拖动逻辑
// ---------------------------------------------------------------------------

float SettingsWindow::ScrollBarCenterX() const {
    // 与主窗口一致：滚动条中心距面板右边缘 7 DIP。
    return kWidth - kSidePad - 7.0f;
}

float SettingsWindow::ScrollTrackTop() const { return kCardTop + 10.0f; }

float SettingsWindow::ScrollTrackBottom() const {
    return (view_height_ - kFooterHeight + 4.0f) - 10.0f;
}

D2D1_RECT_F SettingsWindow::ScrollTrackRect() const {
    const bool active = scroll_hover_ || scroll_dragging_;
    const float width = active ? 7.0f : 3.0f;
    const float center = ScrollBarCenterX();
    return Rect(center - width * 0.5f, ScrollTrackTop(), center + width * 0.5f,
                ScrollTrackBottom());
}

float SettingsWindow::ScrollThumbHeight() const {
    const float track_height = ScrollTrackBottom() - ScrollTrackTop();
    const float visible = (view_height_ - kFooterHeight + 4.0f - 1.0f) -
                          (kCardTop + 1.0f);
    const float content = visible + scroll_max_;
    return std::max(28.0f,
                    track_height * visible / std::max(content, 1.0f));
}

D2D1_RECT_F SettingsWindow::ScrollThumbRect() const {
    const D2D1_RECT_F track = ScrollTrackRect();
    const float thumb_height = ScrollThumbHeight();
    float progress = scroll_max_ > 0.0f ? scroll_ / scroll_max_ : 0.0f;
    progress = std::min(std::max(progress, 0.0f), 1.0f);
    const float top =
        track.top + (track.bottom - track.top - thumb_height) * progress;
    return Rect(track.left, top, track.right, top + thumb_height);
}

D2D1_RECT_F SettingsWindow::ScrollBarHitRect() const {
    const float center = ScrollBarCenterX();
    return Rect(center - 8.0f, ScrollTrackTop(), center + 8.0f,
                ScrollTrackBottom());
}

void SettingsWindow::BeginScrollDrag(float y) {
    scroll_dragging_ = true;
    const D2D1_RECT_F thumb = ScrollThumbRect();
    if (y < thumb.top || y > thumb.bottom) {
        const float track_height = ScrollTrackBottom() - ScrollTrackTop();
        const float thumb_height = ScrollThumbHeight();
        const float travel = std::max(1.0f, track_height - thumb_height);
        float progress =
            (y - ScrollTrackTop() - thumb_height * 0.5f) / travel;
        progress = std::min(std::max(progress, 0.0f), 1.0f);
        SetScroll(progress * scroll_max_);
    }
    scroll_drag_start_y_ = y;
    scroll_drag_start_scroll_ = scroll_;
}

void SettingsWindow::UpdateScrollDrag(float y) {
    const float track_height = ScrollTrackBottom() - ScrollTrackTop();
    const float thumb_height = ScrollThumbHeight();
    const float travel = std::max(1.0f, track_height - thumb_height);
    const float ratio = scroll_max_ > 0.0f ? scroll_max_ / travel : 0.0f;
    SetScroll(scroll_drag_start_scroll_ + (y - scroll_drag_start_y_) * ratio);
}

// ---------------------------------------------------------------------------
// 触摸：内容区上下拖动滚动，滚动条区域直接拖动滑块
// ---------------------------------------------------------------------------

void SettingsWindow::HandleTouch(UINT count, HTOUCHINPUT handle) {
    if (count == 0 || !handle) return;
    std::vector<TOUCHINPUT> inputs(count);
    if (!GetTouchInputInfo(handle, count, inputs.data(), sizeof(TOUCHINPUT))) {
        return;
    }
    bool close_after = false;
    for (const TOUCHINPUT& input : inputs) {
        // TOUCHINPUT 坐标是物理屏幕的百分之一像素。
        POINT point = {static_cast<LONG>(input.x / 100),
                       static_cast<LONG>(input.y / 100)};
        ScreenToClient(hwnd_, &point);
        const float x = point.x / scale_;
        const float y = point.y / scale_;
        if ((input.dwFlags & TOUCHEVENTF_DOWN) != 0) {
            if (touch_active_) continue;
            touch_active_ = true;
            touch_contact_id_ = input.dwID;
            touch_last_tick_ = GetTickCount();
            touch_panning_ = false;
            touch_pending_ = false;
            touch_scroll_drag_ = false;
            if (menu_open_) continue;
            touch_start_y_ = y;
            touch_start_scroll_ = scroll_;
            if (Contains(CloseRect(), x, y)) {
                // 关闭动作留到抬手，避免误触。
                touch_pending_ = true;
            } else if (scroll_max_ > 0.5f &&
                       Contains(ScrollBarHitRect(), x, y)) {
                touch_scroll_drag_ = true;
                BeginScrollDrag(y);
            } else if (y >= kHeaderHeight) {
                touch_pending_ = true;
            }
        } else if ((input.dwFlags & TOUCHEVENTF_MOVE) != 0) {
            if (!touch_active_ || input.dwID != touch_contact_id_) continue;
            touch_last_tick_ = GetTickCount();
            if (touch_scroll_drag_) {
                UpdateScrollDrag(y);
                continue;
            }
            if (!touch_pending_ && !touch_panning_) continue;
            const float delta = touch_start_y_ - y;
            if (!touch_panning_ && scroll_max_ > 0.5f &&
                std::fabs(delta) > 4.0f) {
                touch_panning_ = true;
                touch_pending_ = false;
            }
            if (touch_panning_) SetScroll(touch_start_scroll_ + delta);
        } else if ((input.dwFlags & TOUCHEVENTF_UP) != 0) {
            if (!touch_active_ || input.dwID != touch_contact_id_) continue;
            touch_last_tick_ = GetTickCount();
            touch_active_ = false;
            if (touch_scroll_drag_) {
                touch_scroll_drag_ = false;
                scroll_dragging_ = false;
                continue;
            }
            if (touch_panning_) {
                touch_panning_ = false;
                continue;
            }
            if (touch_pending_) {
                touch_pending_ = false;
                CancelHotkeyCaptureAt(x, y);
                if (Contains(CloseRect(), x, y)) {
                    close_after = true;
                } else if (y >= kHeaderHeight &&
                           !Contains(ScrollBarHitRect(), x, y)) {
                    HandleRowClick(x, y);
                }
            }
        }
    }
    CloseTouchInputHandle(handle);
    if (close_after) {
        Close();
        return;
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::CancelHotkeyCaptureAt(float x, float y) {
    if (!capturing_hotkey_) return;
    const int hit = HitRow(x, y);
    const bool on_control =
        hit == RowShortcut &&
        (Contains(ControlHitRect(ShortcutRect(hit)), x, y) ||
         Contains(ControlHitRect(ShortcutClearRect(hit)), x, y));
    if (on_control) return;
    capturing_hotkey_ = false;
    clear_hover_ = false;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// 行的点击动作：鼠标按下与触摸轻点共用。
void SettingsWindow::HandleRowClick(float x, float y) {
    const int row = HitRow(x, y);
    if (row < 0) return;

    if (row == RowShortcut) {
        if ((settings_.hotkey_vk != 0 || capturing_hotkey_) &&
            Contains(ControlHitRect(ShortcutClearRect(row)), x, y)) {
            ClearHotkey();
        } else if (Contains(ControlHitRect(ShortcutRect(row)), x, y)) {
            capturing_hotkey_ = true;
            clear_hover_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (IsSubRow(row)) {
        const bool parent_on = RowToggleValue(settings_, SubRowParent(row));
        if (parent_on) {
            if (SubRowIsChoice(row)) {
                if (Contains(ControlHitRect(ChoiceRect(row)), x, y)) {
                    ChooseRow(row);
                }
            } else if (Contains(ControlHitRect(ToggleRect(row)), x, y)) {
                ToggleRow(row);
            }
        }
        return;
    }
    if (kRows[row].toggle) {
        if (Contains(ControlHitRect(ToggleRect(row)), x, y)) {
            ToggleRow(row);
        } else if (IsExpandable(row) &&
                   Contains(ChevronHitRect(RowRect(row)), x, y)) {
            ToggleExpanded(row);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (Contains(ControlHitRect(ChoiceRect(row)), x, y)) {
        ChooseRow(row);
    }
}

// ---------------------------------------------------------------------------
// 消息处理
// ---------------------------------------------------------------------------

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
    SettingsWindow* self = reinterpret_cast<SettingsWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = reinterpret_cast<SettingsWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->HandleMessage(hwnd, message, wparam, lparam);
}

LRESULT SettingsWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                                      LPARAM lparam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            BeginPaint(hwnd, &paint);
            Paint();
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;

        case WM_TOUCH:
            HandleTouch(LOWORD(wparam),
                        reinterpret_cast<HTOUCHINPUT>(lparam));
            return 0;

        case WM_LBUTTONDOWN: {
            if (menu_open_ || SyntheticMouseSuppressed()) return 0;
            const float x = GET_X_LPARAM(lparam) / scale_;
            const float y = GET_Y_LPARAM(lparam) / scale_;
            CancelHotkeyCaptureAt(x, y);
            if (Contains(CloseRect(), x, y)) {
                Close();
                return 0;
            }
            if (y < kHeaderHeight) {
                dragging_ = true;
                SetCapture(hwnd);
                GetCursorPos(&drag_cursor_start_);
                GetWindowRect(hwnd, &drag_window_start_);
                return 0;
            }
            if (scroll_max_ > 0.5f && Contains(ScrollBarHitRect(), x, y)) {
                BeginScrollDrag(y);
                SetCapture(hwnd);
                return 0;
            }
            HandleRowClick(x, y);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (SyntheticMouseSuppressed()) return 0;
            if (scroll_dragging_) {
                UpdateScrollDrag(GET_Y_LPARAM(lparam) / scale_);
                return 0;
            }
            if (dragging_) {
                POINT cursor = {};
                GetCursorPos(&cursor);
                SetWindowPos(hwnd, nullptr,
                             drag_window_start_.left +
                                 (cursor.x - drag_cursor_start_.x),
                             drag_window_start_.top +
                                 (cursor.y - drag_cursor_start_.y),
                             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            if (!tracking_leave_) {
                TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                tracking_leave_ = true;
            }
            const float x = GET_X_LPARAM(lparam) / scale_;
            const float y = GET_Y_LPARAM(lparam) / scale_;
            const bool over_scroll =
                scroll_max_ > 0.5f && Contains(ScrollBarHitRect(), x, y);
            if (over_scroll != scroll_hover_) {
                scroll_hover_ = over_scroll;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            const bool close = Contains(CloseRect(), x, y);
            if (close != close_hover_) {
                close_hover_ = close;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            const int row = HitRow(x, y);
            if (row != hover_row_) {
                hover_row_ = row;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            const bool clear =
                row == RowShortcut &&
                (settings_.hotkey_vk != 0 || capturing_hotkey_) &&
                Contains(ControlHitRect(ShortcutClearRect(row)), x, y);
            if (clear != clear_hover_) {
                clear_hover_ = clear;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            bool interactive = false;
            if (row >= 0) {
                if (row == RowShortcut) {
                    interactive =
                        Contains(ControlHitRect(ShortcutRect(row)), x, y);
                } else if (IsSubRow(row)) {
                    const bool parent_on =
                        RowToggleValue(settings_, SubRowParent(row));
                    interactive =
                        parent_on &&
                        (SubRowIsChoice(row)
                             ? Contains(ControlHitRect(ChoiceRect(row)), x, y)
                             : Contains(ControlHitRect(ToggleRect(row)), x,
                                        y));
                } else if (kRows[row].toggle) {
                    interactive =
                        Contains(ControlHitRect(ToggleRect(row)), x, y) ||
                        (IsExpandable(row) &&
                         Contains(ChevronHitRect(RowRect(row)), x, y));
                } else {
                    interactive = Contains(ControlHitRect(ChoiceRect(row)), x,
                                           y);
                }
            }
            SetCursor(LoadCursorW(
                nullptr,
                (interactive || close || over_scroll) ? IDC_HAND : IDC_ARROW));
            return 0;
        }

        case WM_MOUSELEAVE:
            tracking_leave_ = false;
            close_hover_ = false;
            clear_hover_ = false;
            hover_row_ = -1;
            scroll_hover_ = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONUP:
            if (SyntheticMouseSuppressed()) return 0;
            if (scroll_dragging_) {
                scroll_dragging_ = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (dragging_) {
                dragging_ = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            break;

        case WM_CAPTURECHANGED:
            if (scroll_dragging_) {
                scroll_dragging_ = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (dragging_) {
                dragging_ = false;
                tracking_leave_ = false;
                hover_row_ = -1;
                close_hover_ = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL: {
            if (scroll_max_ <= 0.0f) return 0;
            if (menu_open_) return 0;
            const float delta = GET_WHEEL_DELTA_WPARAM(wparam) / 120.0f;
            SetScroll(scroll_ - delta * 48.0f);
            return 0;
        }

        case WM_SYSKEYDOWN:
        case WM_KEYDOWN:
            if (capturing_hotkey_) {
                HandleHotkeyCapture(static_cast<int>(wparam));
                return 0;
            }
            if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
                // 下拉展开时 Esc 只关闭下拉，由弹出菜单自己处理。
                if (menu_open_) return 0;
                Close();
                return 0;
            }
            break;

        case WM_KILLFOCUS:
            if (capturing_hotkey_) {
                capturing_hotkey_ = false;
                clear_hover_ = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;

        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) ? HIWORD(wparam) / 96.0f : SystemDpi() / 96.0f;
            if (scale_ <= 0.0f) scale_ = 1.0f;
            const float full_height =
                kCardTop + BaseContentHeight() + kFooterHeight;
            RECT work_area = WorkAreaForWindow(hwnd);
            const float available =
                (work_area.bottom - work_area.top) / scale_ * 0.86f;
            view_height_ =
                std::min(full_height, std::max(320.0f, available));
            ResizeWindow();
            if (RECT* suggested = reinterpret_cast<RECT*>(lparam)) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, 0,
                             0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            ReleaseTarget();
            return 0;

        case WM_NCDESTROY: {
            hwnd_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            CloseHandler handler = std::move(on_close_);
            on_close_ = nullptr;
            if (handler) handler();
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace usb::ui
