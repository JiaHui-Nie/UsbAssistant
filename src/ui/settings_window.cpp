#include "ui/settings_window.h"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

#include "core/log.h"
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

enum Row {
    RowAutoStart = 0,
    RowTopMost,
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
};

struct SubRowSpec {
    int row;
    const wchar_t* label;
};

constexpr SubRowSpec kSubRows[] = {
    {RowHiddenEsp, L"ESP 分区"},
    {RowHiddenMsr, L"MSR 分区"},
    {RowHiddenOther, L"其他无盘符分区"},
};

struct RowSpec {
    const wchar_t* label;
    bool toggle;
};

constexpr RowSpec kRows[kRowCount] = {
    {L"开机自启动", true},
    {L"窗口始终置顶", true},
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
const Option kEmptyHide[] = {{L"不隐藏", 0},  {L"3 秒", 3},  {L"10 秒", 10},
                             {L"20 秒", 20},    {L"30 秒", 30}, {L"60 秒", 60}};
const Option kMainHeights[] = {
    {L"40 %", 40}, {L"50 %", 50}, {L"60 %", 60}, {L"70 %", 70},
    {L"80 %", 80}, {L"90 %", 90}, {L"100 %", 100},
};
const Option kRefresh[] = {{L"关闭", 0},  {L"1 秒", 1},  {L"2 秒", 2},
                           {L"3 秒", 3},  {L"5 秒", 5},  {L"10 秒", 10}};

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
        case RowHiddenPartitions:
            return settings.show_hidden_partitions;
        case RowHiddenEsp:
            return settings.show_esp_partitions;
        case RowHiddenMsr:
            return settings.show_msr_partitions;
        case RowHiddenOther:
            return settings.show_other_hidden_partitions;
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

        // 弹出层必须跟随设置窗口的置顶层级：设置窗口是 topmost 时，
        // 非 topmost 的弹出层会被压在它后面，既看不见也收不到点击。
        const bool owner_topmost =
            (GetWindowLongPtrW(owner, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

        hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                (owner_topmost ? WS_EX_TOPMOST : 0),
            L"UsbAssistantChoicePopup", L"", WS_POPUP, x, y, width, height,
            nullptr, nullptr, instance, this);
        if (!hwnd_) return -1;

        const int radius = static_cast<int>(std::lround(10.0f * scale_));
        SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                               radius, radius),
                     FALSE);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, owner_topmost ? HWND_TOPMOST : HWND_TOP, 0, 0, 0,
                     0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);

        bool previous_down = false;
        // 下拉是在鼠标按下时展开的：先等这次按键松开，再启用“点外面关闭”，
        // 否则初始的抬起事件会立刻把菜单关掉。
        bool armed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0;
        RECT popup_rect = {};
        GetWindowRect(hwnd_, &popup_rect);
        USB_LOG_DEBUG(L"touch",
                      L"下拉打开：armed=%d rect=%ld,%ld,%ld,%ld items=%zu",
                      armed ? 1 : 0, popup_rect.left, popup_rect.top,
                      popup_rect.right, popup_rect.bottom, items_.size());
        while (!done_) {
            MSG message = {};
            const DWORD wait = MsgWaitForMultipleObjectsEx(
                0, nullptr, 40, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
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
            if (wait != WAIT_TIMEOUT) continue;

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
                USB_LOG_DEBUG(L"touch",
                              L"下拉关闭：外部点击 inside=%d down=%d armed=%d",
                              inside ? 1 : 0, down ? 1 : 0, armed ? 1 : 0);
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
                const int item = HitItem(GET_Y_LPARAM(lparam));
                USB_LOG_DEBUG(L"touch", L"下拉抬起：item=%d", item);
                Finish(item);
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
};

}  // namespace

// ---------------------------------------------------------------------------
// 窗口生命周期
// ---------------------------------------------------------------------------

SettingsWindow::~SettingsWindow() { ReleaseTarget(); }

bool SettingsWindow::Open(HINSTANCE instance, HWND owner, ID2D1Factory* d2d,
                          IDWriteFactory* dwrite, const Fonts* fonts,
                          const Palette& palette, const Settings& settings,
                          ChangeHandler on_change, CloseHandler on_close) {
    if (hwnd_) {
        settings_ = settings;
        palette_ = palette;
        on_change_ = std::move(on_change);
        on_close_ = std::move(on_close);
        dragging_ = false;
        tracking_leave_ = false;
        hover_row_ = -1;
        close_hover_ = false;
        SetTopMost(settings_.top_most);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
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

    // 不设置 owner：设置窗口与主窗口一起置顶，层级由 SetTopMost 统一维护。
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"UsbAssistantSettings", L"设置",
                            WS_POPUP, left, top, width, height, nullptr, nullptr,
                            instance, this);
    if (!hwnd_) return false;
    SetTopMost(settings_.top_most);

    const int radius = static_cast<int>(std::lround(16.0f * scale_));
    SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, width + 1, height + 1, radius,
                                           radius),
                 FALSE);
    const BOOL dark = palette_.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));  // DWMWA_USE_IMMERSIVE_DARK_MODE

    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void SettingsWindow::Close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool SettingsWindow::IsOpen() const { return hwnd_ != nullptr; }

void SettingsWindow::SetTopMost(bool enabled) {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
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
        const float track_left = card.right - 8.0f;
        const float track_top = card.top + 10.0f;
        const float track_bottom = card.bottom - 10.0f;
        FillRound(target_, brush,
                  Rect(track_left - 1.5f, track_top, track_left + 1.5f,
                       track_bottom),
                  1.5f, WithAlpha(palette_.text_primary, 0.10f));
        const float track_height = track_bottom - track_top;
        const float visible = clip.bottom - clip.top;
        const float content = visible + scroll_max_;
        const float thumb_height =
            std::max(24.0f, track_height * visible / std::max(content, 1.0f));
        float progress = scroll_max_ > 0.0f ? scroll_ / scroll_max_ : 0.0f;
        progress = std::min(std::max(progress, 0.0f), 1.0f);
        const float thumb_top =
            track_top + (track_height - thumb_height) * progress;
        FillRound(target_, brush,
                  Rect(track_left - 1.5f, thumb_top, track_left + 1.5f,
                       thumb_top + thumb_height),
                  1.5f, WithAlpha(palette_.text_primary, 0.32f));
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
            !IsSubRow(row) || settings_.show_hidden_partitions;
        PaintRow(target, brush, row, rect, row == hover_row_, enabled);
        if (!IsSubRow(row) && i + 1 < order.size() &&
            !IsSubRow(order[i + 1])) {
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
    if (hovered && enabled) {
        FillRound(target, brush, rect, 8.0f,
                  WithAlpha(palette_.hover_fill, 0.7f));
    }

    if (IsSubRow(row)) {
        const wchar_t* label = L"";
        for (const auto& sub : kSubRows) {
            if (sub.row == row) {
                label = sub.label;
                break;
            }
        }
        DrawText(target, brush, label, fonts_->dialog_row,
                 Rect(rect.left + 12.0f, rect.top, rect.right - 68.0f,
                      rect.bottom),
                 enabled ? palette_.text_secondary : palette_.text_tertiary);

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

    if (kRows[row].toggle) {
        DrawSwitch(target, brush, ToggleRect(row),
                   RowToggleValue(settings_, row), palette_.accent,
                   WithAlpha(palette_.text_tertiary, 0.28f),
                   palette_.card);
        if (row == RowHiddenPartitions) {
            const float x = rect.right - 16.0f - kToggleWidth - 14.0f;
            const float y = (rect.top + rect.bottom) * 0.5f;
            brush->SetColor(WithAlpha(palette_.text_tertiary, 0.85f));
            if (expanded_) {
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

    const D2D1_RECT_F pill = ChoiceRect(row);
    const bool open = row == open_row_;
    FillRound(target, brush, pill, 8.0f,
              open ? palette_.accent_soft : palette_.secondary_button);
    DrawText(target, brush, ChoiceText(settings_, row), fonts_->button, pill,
             open ? palette_.accent : palette_.secondary_text);

    // 右侧下拉箭头
    const float x = pill.right - 14.0f;
    const float y = (pill.top + pill.bottom) * 0.5f;
    brush->SetColor(WithAlpha(
        open ? palette_.accent : palette_.secondary_text, 0.75f));
    target->DrawLine({x - 3.0f, y - 1.5f}, {x, y + 1.5f}, brush, 1.2f);
    target->DrawLine({x, y + 1.5f}, {x + 3.0f, y - 1.5f}, brush, 1.2f);
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------

D2D1_RECT_F SettingsWindow::RowRect(int row) const {
    float top = kCardTop + 8.0f - scroll_;
    for (const int candidate : RowOrder()) {
        const float height = IsSubRow(candidate) ? kSubRowHeight : kRowHeight;
        if (candidate == row) {
            const float left =
                kSidePad + 8.0f + (IsSubRow(row) ? kSubIndent : 0.0f);
            return Rect(left, top, kWidth - kSidePad - 8.0f, top + height);
        }
        top += height;
    }
    return {};
}

bool SettingsWindow::IsSubRow(int row) { return row >= RowHiddenEsp; }

std::vector<int> SettingsWindow::RowOrder() const {
    std::vector<int> order;
    order.reserve(kRowCount + std::size(kSubRows));
    for (int row = 0; row < kRowCount; ++row) {
        order.push_back(row);
        if (row == RowHiddenPartitions && expanded_) {
            for (const auto& sub : kSubRows) order.push_back(sub.row);
        }
    }
    return order;
}

float SettingsWindow::ContentHeight() const {
    float height = 8.0f + 8.0f;
    for (const int row : RowOrder()) {
        height += IsSubRow(row) ? kSubRowHeight : kRowHeight;
    }
    return height;
}

float SettingsWindow::BaseContentHeight() const {
    return 8.0f + 8.0f + kRowHeight * static_cast<float>(kRowCount);
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
    if (index >= 0 && index != current) {
        ApplyChoice(&settings_, row, index);
        NotifyChange();
    }
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
            LARGE_INTEGER start = {};
            QueryPerformanceCounter(&start);
            Paint();
            LARGE_INTEGER end = {};
            QueryPerformanceCounter(&end);
            if (usb::log::Enabled()) {
                LARGE_INTEGER frequency = {};
                QueryPerformanceFrequency(&frequency);
                const double ms =
                    frequency.QuadPart > 0
                        ? (end.QuadPart - start.QuadPart) * 1000.0 /
                              static_cast<double>(frequency.QuadPart)
                        : 0.0;
                if (ms > 8.0) {
                    USB_LOG_WARN(L"ui", L"设置窗口整窗重绘耗时 %.1f ms", ms);
                }
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;

        case WM_LBUTTONDOWN: {
            if (menu_open_) return 0;
            const float x = GET_X_LPARAM(lparam) / scale_;
            const float y = GET_Y_LPARAM(lparam) / scale_;
            if (Contains(CloseRect(), x, y)) {
                Close();
                return 0;
            }
            if (y < kHeaderHeight) {
                dragging_ = true;
                drag_moves_ = 0;
                SetCapture(hwnd);
                GetCursorPos(&drag_cursor_start_);
                GetWindowRect(hwnd, &drag_window_start_);
                USB_LOG_DEBUG(L"touch", L"拖动开始：cursor=%ld,%ld",
                              drag_cursor_start_.x, drag_cursor_start_.y);
                return 0;
            }
            const int row = HitRow(x, y);
            USB_LOG_DEBUG(L"touch",
                          L"鼠标按下：x=%.0f y=%.0f row=%d menu_open=%d", x, y,
                          row, menu_open_ ? 1 : 0);
            if (row >= 0) {
                if (IsSubRow(row)) {
                    if (settings_.show_hidden_partitions) ToggleRow(row);
                } else if (kRows[row].toggle) {
                    if (row == RowHiddenPartitions &&
                        !Contains(ToggleRect(row), x, y)) {
                        expanded_ = !expanded_;
                        UpdateScrollBounds();
                        if (expanded_) scroll_ = scroll_max_;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    } else {
                        ToggleRow(row);
                    }
                } else {
                    ChooseRow(row);
                }
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (dragging_) {
                POINT cursor = {};
                GetCursorPos(&cursor);
                ++drag_moves_;
                if (drag_moves_ <= 400 && (drag_moves_ % 4) == 1) {
                    USB_LOG_DEBUG(L"touch",
                                  L"拖动移动 #%d cursor=%ld,%ld", drag_moves_,
                                  cursor.x, cursor.y);
                }
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
            const bool interactive =
                row >= 0 &&
                (!IsSubRow(row) || settings_.show_hidden_partitions);
            SetCursor(LoadCursorW(
                nullptr, (interactive || close) ? IDC_HAND : IDC_ARROW));
            return 0;
        }

        case WM_MOUSELEAVE:
            tracking_leave_ = false;
            close_hover_ = false;
            hover_row_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONUP:
            if (dragging_) {
                dragging_ = false;
                USB_LOG_DEBUG(L"touch", L"拖动结束：up，移动消息=%d", drag_moves_);
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            break;

        case WM_CAPTURECHANGED:
            if (dragging_) {
                dragging_ = false;
                USB_LOG_WARN(L"touch",
                             L"拖动被中断：捕获丢失，移动消息=%d", drag_moves_);
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

        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                // 下拉展开时 Esc 只关闭下拉，由弹出菜单自己处理。
                if (menu_open_) return 0;
                Close();
                return 0;
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

        // 临时取证：确认触摸设备上是否收到指针/触摸/手势消息，以及它们与
        // 鼠标消息的先后关系。不改变任何处理行为，交回 DefWindowProc。
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP:
        case WM_TOUCH:
        case WM_GESTURE:
            USB_LOG_DEBUG(L"touch", L"原始输入消息 0x%04X wparam=%llu",
                          message,
                          static_cast<unsigned long long>(wparam));
            break;

        // 触摸长按会被系统合成为右键按下；记录它以便区分“点击没反应”的原因。
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            USB_LOG_DEBUG(L"touch", L"右键消息 0x%04X（触摸长按会合成右键）",
                          message);
            break;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace usb::ui
