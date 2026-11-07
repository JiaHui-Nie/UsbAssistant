#include "settings_window.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <windowsx.h>

#include <cmath>
#include <cwchar>
#include <functional>
#include <string>

#include "theme.h"

namespace {

enum RowId {
    R_AUTOSTART = 0,
    R_TOPMOST,
    R_POS,
    R_CAPBASE,
    R_CLOSE,
    R_EMPTYHIDE,
    R_EDGE,
    R_HEIGHT,
    R_REFRESH,
    R_COUNT
};

const wchar_t* kRowLabels[R_COUNT] = {
    L"开机自启动",
    L"窗口始终置顶",
    L"窗口默认位置",
    L"容量显示风格",
    L"关闭按钮的功能",
    L"未插入 U 盘时自动隐藏",
    L"靠边时隐藏到悬浮窗",
    L"主界面最大高度",
    L"数据刷新间隔",
};

const int kPosValues[] = {0, 1, 2, 3}; // 4 = 记住上次位置
const wchar_t* kPosText[] = {L"左上", L"左下", L"右上", L"右下", L"记住上次位置"};
const wchar_t* kCapBaseText[] = {L"1024 进制", L"1000 进制"};
const wchar_t* kCloseText[] = {L"退出程序", L"隐藏窗口"};
const int kEmptyHideValues[] = {0, 3, 10, 20, 30, 60};
const wchar_t* kEmptyHideText[] = {L"不隐藏", L"3 秒", L"10 秒", L"20 秒",
                                   L"30 秒", L"60 秒"};
const int kRefreshValues[] = {0, 1, 2, 3, 5, 10};
const wchar_t* kRefreshText[] = {L"关闭", L"1 秒", L"2 秒", L"3 秒", L"5 秒",
                                 L"10 秒"};

HWND g_wnd = nullptr;
HWND g_listWnd = nullptr;      // 下拉列表弹出窗口
int g_listRow = -1;            // 当前展开的行
int g_listHover = -1;          // 列表里悬停的项
Settings* g_settings = nullptr;
std::function<void()> g_onApply;
ID2D1Factory* g_d2d = nullptr;
ID2D1DCRenderTarget* g_rt = nullptr;
ID2D1DCRenderTarget* g_listRt = nullptr;
IDWriteFactory* g_dwrite = nullptr;
IDWriteTextFormat* g_titleFont = nullptr;
IDWriteTextFormat* g_rowFont = nullptr;
IDWriteTextFormat* g_valueFont = nullptr;
IDWriteTextFormat* g_optionFont = nullptr;
IDWriteTextFormat* g_hintFont = nullptr;

bool g_closeHover = false;
bool g_tracking = false;

// 主界面最大高度的填空题状态
int g_inputRow = -1;
std::wstring g_inputText;

float g_scale = 1.0f;
const int kLogicalW = 400;
const int kLogicalH = 546;
const float kHeaderH = 52.0f;
const float kRowH = 50.0f;
const float kCardTop = 58.0f;

D2D1_RECT_F Rect(float l, float t, float r, float b) {
    D2D1_RECT_F out = {l, t, r, b};
    return out;
}

D2D1_ROUNDED_RECT Round(const D2D1_RECT_F& r, float radius) {
    D2D1_ROUNDED_RECT out = {};
    out.rect = r;
    out.radiusX = radius;
    out.radiusY = radius;
    return out;
}

void DrawTextAt(ID2D1RenderTarget* rt, IDWriteTextFormat* format,
                ID2D1SolidColorBrush* brush, const wchar_t* text,
                const D2D1_RECT_F& rect, const D2D1_COLOR_F& color) {
    if (!format) return;
    brush->SetColor(color);
    rt->DrawTextW(text, static_cast<UINT>(wcslen(text)), format, rect, brush,
                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

bool CreateFormat(IDWriteFactory* factory, float size, DWRITE_FONT_WEIGHT weight,
                  DWRITE_TEXT_ALIGNMENT align, IDWriteTextFormat** out) {
    HRESULT hr = factory->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", out);
    if (FAILED(hr) || !*out) {
        hr = factory->CreateTextFormat(L"Segoe UI", nullptr, weight,
                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                       size, L"zh-cn", out);
    }
    if (FAILED(hr) || !*out) return false;
    (*out)->SetTextAlignment(align);
    (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return true;
}

int ValueIndex(int row) {
    const Settings& s = *g_settings;
    switch (row) {
        case R_POS:
            return s.posMode == 1 ? 4 : s.corner;
        case R_CAPBASE:
            return s.capacityBase == 1000 ? 1 : 0;
        case R_CLOSE:
            return s.closeAction == 1 ? 1 : 0;
        case R_EMPTYHIDE:
            for (int i = 0; i < 6; ++i) {
                if (kEmptyHideValues[i] == s.emptyHideSec) return i;
            }
            return 0;
        case R_REFRESH:
            for (int i = 0; i < 6; ++i) {
                if (kRefreshValues[i] == s.refreshSec) return i;
            }
            return 0;
        default:
            return 0;
    }
}

const wchar_t* ValueText(int row) {
    int index = ValueIndex(row);
    switch (row) {
        case R_POS:
            return kPosText[index];
        case R_CAPBASE:
            return kCapBaseText[index];
        case R_CLOSE:
            return kCloseText[index];
        case R_EMPTYHIDE:
            return kEmptyHideText[index];
        case R_REFRESH:
            return kRefreshText[index];
        default:
            return L"";
    }
}

bool IsToggleRow(int row) {
    return row == R_AUTOSTART || row == R_TOPMOST || row == R_EDGE;
}

bool ToggleValue(int row) {
    const Settings& s = *g_settings;
    if (row == R_AUTOSTART) return s.autoStart;
    if (row == R_TOPMOST) return s.topMost;
    return s.edgeCollapse;
}

int OptionCount(int row) {
    switch (row) {
        case R_POS:
            return 5;
        case R_CAPBASE:
            return 2;
        case R_CLOSE:
            return 2;
        case R_EMPTYHIDE:
            return 6;
        case R_REFRESH:
            return 6;
        default:
            return 0;
    }
}

const wchar_t* OptionText(int row, int index) {
    switch (row) {
        case R_POS:
            return kPosText[index];
        case R_CAPBASE:
            return kCapBaseText[index];
        case R_CLOSE:
            return kCloseText[index];
        case R_EMPTYHIDE:
            return kEmptyHideText[index];
        case R_REFRESH:
            return kRefreshText[index];
        default:
            return L"";
    }
}

// 下拉框选中某一项后写回设置
void SetRowValue(int row, int index) {
    Settings& s = *g_settings;
    switch (row) {
        case R_POS:
            if (index == 4) {
                s.posMode = 1;
            } else {
                s.posMode = 0;
                s.corner = kPosValues[index];
            }
            break;
        case R_CAPBASE:
            s.capacityBase = index == 1 ? 1000 : 1024;
            break;
        case R_CLOSE:
            s.closeAction = index == 1 ? 1 : 0;
            break;
        case R_EMPTYHIDE:
            s.emptyHideSec = kEmptyHideValues[index];
            break;
        case R_REFRESH:
            s.refreshSec = kRefreshValues[index];
            break;
        default:
            return;
    }
    SaveSettings(s);
    if (g_onApply) g_onApply();
}

void ApplyRowChange(int row) {
    Settings& s = *g_settings;
    if (row == R_AUTOSTART) {
        s.autoStart = !s.autoStart;
        ApplyAutoStart(s.autoStart);
    } else if (row == R_TOPMOST) {
        s.topMost = !s.topMost;
    } else if (row == R_EDGE) {
        s.edgeCollapse = !s.edgeCollapse;
    } else {
        return;
    }
    SaveSettings(s);
    if (g_onApply) g_onApply();
}

D2D1_RECT_F RowRect(int row) {
    float top = kCardTop + 6.0f + row * kRowH;
    return Rect(14.0f, top, static_cast<float>(kLogicalW) - 14.0f, top + kRowH);
}

D2D1_RECT_F CloseRect() {
    return Rect(kLogicalW - 14.0f - 28.0f, (kHeaderH - 28.0f) * 0.5f,
                kLogicalW - 14.0f, (kHeaderH + 28.0f) * 0.5f);
}

D2D1_RECT_F PillRect(int row) {
    D2D1_RECT_F rr = RowRect(row);
    float pw = 132.0f;
    float ph = 27.0f;
    return Rect(rr.right - 16.0f - pw, (rr.top + rr.bottom - ph) * 0.5f,
                rr.right - 16.0f, (rr.top + rr.bottom + ph) * 0.5f);
}

// 开关控件的命中区域
D2D1_RECT_F SwitchRect(int row) {
    D2D1_RECT_F rr = RowRect(row);
    float sw = 40.0f;
    float sh = 22.0f;
    return Rect(rr.right - 16.0f - sw, (rr.top + rr.bottom - sh) * 0.5f,
                rr.right - 16.0f, (rr.top + rr.bottom + sh) * 0.5f);
}

// 填空题（百分比）的矩形
D2D1_RECT_F InputRect(int row) {
    D2D1_RECT_F rr = RowRect(row);
    float iw = 92.0f;
    float ih = 27.0f;
    return Rect(rr.right - 16.0f - iw, (rr.top + rr.bottom - ih) * 0.5f,
                rr.right - 16.0f, (rr.top + rr.bottom + ih) * 0.5f);
}

void CommitInput();
void BeginInput(int row);

void BeginInput(int row) {
    if (g_inputRow == row) return;
    CommitInput();
    g_inputRow = row;
    g_inputText = std::to_wstring(g_settings->mainHeightPercent);
    if (g_wnd) SetFocus(g_wnd);
}

void CommitInput() {
    if (g_inputRow < 0 || !g_settings) {
        g_inputRow = -1;
        return;
    }
    int value = g_settings->mainHeightPercent;
    if (!g_inputText.empty()) {
        value = static_cast<int>(wcstol(g_inputText.c_str(), nullptr, 10));
    }
    if (value < 20) value = 20;
    if (value > 100) value = 100;
    g_settings->mainHeightPercent = value;
    g_inputRow = -1;
    g_inputText.clear();
    SaveSettings(*g_settings);
    if (g_onApply) g_onApply();
}

// ---------- 下拉选择框 ----------

const float kListItemH = 34.0f;
const float kListPad = 6.0f;
const float kListW = 152.0f;

float ListHeight() {
    int count = g_listRow >= 0 ? OptionCount(g_listRow) : 0;
    return kListPad * 2.0f + count * kListItemH;
}

int HitListItem(int yPx) {
    float y = static_cast<float>(yPx) / g_scale - kListPad;
    int count = g_listRow >= 0 ? OptionCount(g_listRow) : 0;
    for (int i = 0; i < count; ++i) {
        if (y >= i * kListItemH && y <= (i + 1) * kListItemH) return i;
    }
    return -1;
}

void PaintList(HDC hdc) {
    if (!g_listRt || g_listRow < 0) return;
    float h = ListHeight();
    RECT bindRect = {0, 0, static_cast<LONG>(kListW * g_scale),
                     static_cast<LONG>(h * g_scale)};
    if (FAILED(g_listRt->BindDC(hdc, &bindRect))) return;

    g_listRt->SetDpi(96.0f * g_scale, 96.0f * g_scale);
    g_listRt->BeginDraw();
    g_listRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_listRt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    D2D1_MATRIX_3X2_F identity = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    g_listRt->SetTransform(&identity);

    Palette p = MakePalette(QuerySystemDarkMode());
    g_listRt->Clear(p.card);

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(g_listRt->CreateSolidColorBrush(p.textPrimary, &brush)) || !brush) {
        g_listRt->EndDraw();
        return;
    }

    brush->SetColor(p.cardBorder);
    g_listRt->DrawRoundedRectangle(Round(Rect(0.5f, 0.5f, kListW - 0.5f, h - 0.5f),
                                        10.0f), brush, 1.0f);

    int count = OptionCount(g_listRow);
    int selected = ValueIndex(g_listRow);
    for (int i = 0; i < count; ++i) {
        float top = kListPad + i * kListItemH;
        D2D1_RECT_F item = Rect(6.0f, top, kListW - 6.0f, top + kListItemH);
        if (i == g_listHover) {
            brush->SetColor(p.secondaryBtn);
            g_listRt->FillRoundedRectangle(Round(item, 7.0f), brush);
        }
        DrawTextAt(g_listRt, g_optionFont, brush, OptionText(g_listRow, i),
                   Rect(item.left + 10.0f, item.top, item.right - 26.0f, item.bottom),
                   i == selected ? p.accent : p.textPrimary);

        if (i == selected) {
            // 勾选标记
            float cx = kListW - 20.0f;
            float cy = top + kListItemH * 0.5f;
            brush->SetColor(p.accent);
            D2D1_POINT_2F a = {cx - 5.0f, cy};
            D2D1_POINT_2F b = {cx - 2.0f, cy + 3.0f};
            D2D1_POINT_2F c = {cx + 5.0f, cy - 4.0f};
            g_listRt->DrawLine(a, b, brush, 1.6f);
            g_listRt->DrawLine(b, c, brush, 1.6f);
        }
    }

    brush->Release();
    g_listRt->EndDraw();
}

void CloseDropdown();

LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintList(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            int hit = HitListItem(GET_Y_LPARAM(lp));
            if (hit != g_listHover) {
                g_listHover = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            SetCursor(LoadCursorW(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_LBUTTONUP: {
            int hit = HitListItem(GET_Y_LPARAM(lp));
            int row = g_listRow;
            CloseDropdown();
            if (hit >= 0 && row >= 0 && g_settings) {
                SetRowValue(row, hit);
                if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                CloseDropdown();
                return 0;
            }
            break;
        case WM_NCDESTROY:
            g_listWnd = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OpenDropdown(int row) {
    CloseDropdown();
    if (!g_wnd || !g_settings) return;
    int count = OptionCount(row);
    if (count <= 0) return;

    g_listRow = row;
    g_listHover = -1;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &ListProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"UsbAssistantDropdown";
        RegisterClassExW(&wc);
        registered = true;
    }

    D2D1_RECT_F pill = PillRect(row);
    POINT origin = {static_cast<LONG>(pill.left * g_scale),
                    static_cast<LONG>(pill.top * g_scale)};
    ClientToScreen(g_wnd, &origin);

    int w = static_cast<int>(std::lround(kListW * g_scale));
    int h = static_cast<int>(std::lround(ListHeight() * g_scale));
    int pillW = static_cast<int>(std::lround(132.0f * g_scale));
    int pillH = static_cast<int>(std::lround(27.0f * g_scale));
    int x = origin.x + pillW - w; // 与值胶囊右对齐
    int y = origin.y + pillH + static_cast<int>(std::lround(4.0f * g_scale));

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (x + w > work.right) x = work.right - w;
    if (x < work.left) x = work.left;
    if (y + h > work.bottom) { // 下方放不下就向上弹
        y = origin.y - static_cast<int>(std::lround(4.0f * g_scale)) - h;
        if (y < work.top) y = work.top;
    }

    g_listWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                L"UsbAssistantDropdown", L"", WS_POPUP, x, y, w, h,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_listWnd) {
        g_listRow = -1;
        return;
    }

    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1,
                                  static_cast<int>(12 * g_scale),
                                  static_cast<int>(12 * g_scale));
    SetWindowRgn(g_listWnd, rgn, FALSE);

    ShowWindow(g_listWnd, SW_SHOWNOACTIVATE);
    InvalidateRect(g_listWnd, nullptr, FALSE);
    UpdateWindow(g_listWnd);
    SetCapture(g_listWnd); // 点击列表以外任意位置都会关闭
}

void CloseDropdown() {
    if (g_listWnd) {
        HWND wnd = g_listWnd;
        g_listWnd = nullptr;
        if (GetCapture() == wnd) ReleaseCapture();
        DestroyWindow(wnd);
    }
    g_listRow = -1;
    g_listHover = -1;
    // 收起后胶囊恢复常态配色
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

void Paint(HDC hdc) {
    if (!g_rt) return;
    RECT bindRect = {0, 0, static_cast<LONG>(kLogicalW * g_scale),
                     static_cast<LONG>(kLogicalH * g_scale)};
    if (FAILED(g_rt->BindDC(hdc, &bindRect))) return;

    g_rt->SetDpi(96.0f * g_scale, 96.0f * g_scale);
    g_rt->BeginDraw();
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    D2D1_MATRIX_3X2_F identity = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    g_rt->SetTransform(&identity);

    Palette p = MakePalette(QuerySystemDarkMode());
    g_rt->Clear(p.panel);

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(g_rt->CreateSolidColorBrush(p.textPrimary, &brush)) || !brush) {
        g_rt->EndDraw();
        return;
    }

    float w = static_cast<float>(kLogicalW);
    float h = static_cast<float>(kLogicalH);

    brush->SetColor(p.panelBorder);
    g_rt->DrawRoundedRectangle(Round(Rect(0.5f, 0.5f, w - 0.5f, h - 0.5f), 16.0f),
                               brush, 1.0f);

    DrawTextAt(g_rt, g_titleFont, brush, L"设置", Rect(20.0f, 0.0f, 200.0f, kHeaderH),
               p.textPrimary);

    D2D1_RECT_F close = CloseRect();
    float cx = (close.left + close.right) * 0.5f;
    float cy = (close.top + close.bottom) * 0.5f;
    if (g_closeHover) {
        D2D1_COLOR_F red = {1.0f, 0.231f, 0.188f, 1.0f};
        D2D1_ELLIPSE halo = {{cx, cy}, 15.0f, 15.0f};
        brush->SetColor(red);
        g_rt->FillEllipse(&halo, brush);
    }
    D2D1_COLOR_F crossColor = {1.0f, 1.0f, 1.0f, 1.0f};
    brush->SetColor(g_closeHover ? crossColor : p.textSecondary);
    D2D1_POINT_2F a1 = {cx - 5.0f, cy - 5.0f};
    D2D1_POINT_2F a2 = {cx + 5.0f, cy + 5.0f};
    D2D1_POINT_2F b1 = {cx + 5.0f, cy - 5.0f};
    D2D1_POINT_2F b2 = {cx - 5.0f, cy + 5.0f};
    g_rt->DrawLine(a1, a2, brush, 1.5f);
    g_rt->DrawLine(b1, b2, brush, 1.5f);

    D2D1_RECT_F card = Rect(14.0f, kCardTop, w - 14.0f,
                            kCardTop + R_COUNT * kRowH + 8.0f);
    brush->SetColor(p.card);
    g_rt->FillRoundedRectangle(Round(card, 12.0f), brush);
    brush->SetColor(p.cardBorder);
    g_rt->DrawRoundedRectangle(Round(card, 12.0f), brush, 1.0f);

    for (int row = 0; row < R_COUNT; ++row) {
        D2D1_RECT_F rr = RowRect(row);

        if (row > 0) {
            brush->SetColor(p.cardBorder);
            D2D1_POINT_2F lineA = {rr.left + 14.0f, rr.top};
            D2D1_POINT_2F lineB = {rr.right - 14.0f, rr.top};
            g_rt->DrawLine(lineA, lineB, brush, 1.0f);
        }

        DrawTextAt(g_rt, g_rowFont, brush, kRowLabels[row],
                   Rect(rr.left + 16.0f, rr.top, rr.right - 150.0f, rr.bottom),
                   p.textPrimary);

        if (IsToggleRow(row)) {
            float sw = 40.0f;
            float sh = 22.0f;
            D2D1_RECT_F track = Rect(rr.right - 16.0f - sw,
                                     (rr.top + rr.bottom - sh) * 0.5f,
                                     rr.right - 16.0f,
                                     (rr.top + rr.bottom + sh) * 0.5f);
            bool on = ToggleValue(row);
            brush->SetColor(on ? p.accent : p.track);
            g_rt->FillRoundedRectangle(Round(track, sh * 0.5f), brush);

            float knob = sh - 5.0f;
            float knobCenter = on ? track.right - 2.5f - knob * 0.5f
                                  : track.left + 2.5f + knob * 0.5f;
            brush->SetColor(p.card);
            D2D1_ELLIPSE ellipse = {{knobCenter, (track.top + track.bottom) * 0.5f},
                                    knob * 0.5f, knob * 0.5f};
            g_rt->FillEllipse(&ellipse, brush);
        } else if (row == R_HEIGHT) {
            D2D1_RECT_F in = InputRect(row);
            bool focused = g_inputRow == row;
            std::wstring value =
                focused ? g_inputText
                        : std::to_wstring(g_settings->mainHeightPercent);
            brush->SetColor(p.panel);
            g_rt->FillRoundedRectangle(Round(in, 7.0f), brush);
            brush->SetColor(focused ? p.accent : p.cardBorder);
            g_rt->DrawRoundedRectangle(Round(in, 7.0f), brush, 1.0f);
            DrawTextAt(g_rt, g_optionFont, brush, value.c_str(),
                       Rect(in.left + 10.0f, in.top, in.right - 20.0f, in.bottom),
                       p.textPrimary);
            DrawTextAt(g_rt, g_optionFont, brush, L"%",
                       Rect(in.right - 18.0f, in.top, in.right - 6.0f, in.bottom),
                       p.textSecondary);
            if (focused) {
                float textWidth = 0.0f;
                IDWriteTextLayout* textLayout = nullptr;
                if (g_dwrite && g_optionFont &&
                    SUCCEEDED(g_dwrite->CreateTextLayout(
                        value.c_str(), static_cast<UINT>(value.size()), g_optionFont,
                        200.0f, 20.0f, &textLayout)) &&
                    textLayout) {
                    DWRITE_TEXT_METRICS metrics = {};
                    if (SUCCEEDED(textLayout->GetMetrics(&metrics))) {
                        textWidth = metrics.widthIncludingTrailingWhitespace;
                    }
                    textLayout->Release();
                }
                brush->SetColor(p.accent);
                D2D1_POINT_2F c1 = {in.left + 11.0f + textWidth, in.top + 5.0f};
                D2D1_POINT_2F c2 = {in.left + 11.0f + textWidth, in.bottom - 5.0f};
                g_rt->DrawLine(c1, c2, brush, 1.2f);
            }
        } else {
            D2D1_RECT_F pill = PillRect(row);
            bool open = g_listRow == row;
            brush->SetColor(open ? p.accent : p.secondaryBtn);
            g_rt->FillRoundedRectangle(Round(pill, 13.5f), brush);
            DrawTextAt(g_rt, g_valueFont, brush, ValueText(row),
                       Rect(pill.left + 6.0f, pill.top, pill.right - 22.0f, pill.bottom),
                       open ? D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 1.0f} : p.secondaryText);
            // 下拉箭头
            float ax = pill.right - 12.0f;
            float ay = (pill.top + pill.bottom) * 0.5f;
            D2D1_COLOR_F chevron = open ? D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 1.0f}
                                        : p.secondaryText;
            brush->SetColor(chevron);
            D2D1_POINT_2F p1 = {ax - 4.0f, ay - 2.0f};
            D2D1_POINT_2F p2 = {ax, ay + 2.0f};
            D2D1_POINT_2F p3 = {ax + 4.0f, ay - 2.0f};
            g_rt->DrawLine(p1, p2, brush, 1.4f);
            g_rt->DrawLine(p2, p3, brush, 1.4f);
        }
    }

    DrawTextAt(g_rt, g_hintFont, brush, L"设置保存在程序目录的 config.ini",
               Rect(20.0f, kCardTop + R_COUNT * kRowH + 14.0f, w - 20.0f, h - 6.0f),
               p.textTertiary);

    brush->Release();
    g_rt->EndDraw();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONUP: {
            float x = static_cast<float>(GET_X_LPARAM(lp)) / g_scale;
            float y = static_cast<float>(GET_Y_LPARAM(lp)) / g_scale;

            D2D1_RECT_F close = CloseRect();
            if (x >= close.left && x <= close.right && y >= close.top &&
                y <= close.bottom) {
                CommitInput();
                DestroyWindow(hwnd);
                return 0;
            }
            // 点击填空框以外的位置先提交输入
            if (g_inputRow >= 0) {
                D2D1_RECT_F in = InputRect(g_inputRow);
                bool insideInput = x >= in.left && x <= in.right && y >= in.top &&
                                   y <= in.bottom;
                if (!insideInput) CommitInput();
            }
            for (int row = 0; row < R_COUNT; ++row) {
                if (IsToggleRow(row)) {
                    D2D1_RECT_F sw = SwitchRect(row);
                    if (x >= sw.left && x <= sw.right && y >= sw.top &&
                        y <= sw.bottom) {
                        ApplyRowChange(row);
                        break;
                    }
                } else if (row == R_HEIGHT) {
                    D2D1_RECT_F in = InputRect(row);
                    if (x >= in.left && x <= in.right && y >= in.top &&
                        y <= in.bottom) {
                        BeginInput(row);
                        break;
                    }
                } else {
                    D2D1_RECT_F pill = PillRect(row);
                    if (x >= pill.left && x <= pill.right && y >= pill.top &&
                        y <= pill.bottom) {
                        OpenDropdown(row);
                        break;
                    }
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CHAR: {
            if (g_inputRow < 0) break;
            wchar_t ch = static_cast<wchar_t>(wp);
            if (ch == L'\b') {
                if (!g_inputText.empty()) g_inputText.pop_back();
            } else if (ch >= L'0' && ch <= L'9') {
                if (g_inputText.size() < 4) g_inputText.push_back(ch);
            } else if (ch == L'\r') {
                CommitInput();
            } else if (ch == 27) {
                // Esc：放弃本次输入
                g_inputRow = -1;
                g_inputText.clear();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!g_tracking) {
                TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                g_tracking = true;
            }
            float x = static_cast<float>(GET_X_LPARAM(lp)) / g_scale;
            float y = static_cast<float>(GET_Y_LPARAM(lp)) / g_scale;
            D2D1_RECT_F close = CloseRect();
            bool hover = x >= close.left && x <= close.right && y >= close.top &&
                         y <= close.bottom;
            if (hover != g_closeHover) {
                g_closeHover = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            if (g_closeHover) {
                g_closeHover = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR: {
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt = {};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                float x = static_cast<float>(pt.x) / g_scale;
                float y = static_cast<float>(pt.y) / g_scale;
                // 光标规则：开关/下拉框手型，填空框 I 字形，其余保持箭头
                LPCWSTR cursor = IDC_ARROW;
                D2D1_RECT_F close = CloseRect();
                if (x >= close.left && x <= close.right && y >= close.top &&
                    y <= close.bottom) {
                    cursor = IDC_HAND;
                } else {
                    for (int row = 0; row < R_COUNT; ++row) {
                        D2D1_RECT_F hit = IsToggleRow(row) ? SwitchRect(row)
                                          : (row == R_HEIGHT ? InputRect(row)
                                                             : PillRect(row));
                        if (x >= hit.left && x <= hit.right && y >= hit.top &&
                            y <= hit.bottom) {
                            cursor = row == R_HEIGHT ? IDC_IBEAM : IDC_HAND;
                            break;
                        }
                    }
                }
                SetCursor(LoadCursorW(nullptr, cursor));
                return TRUE;
            }
            break;
        }
        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            float x = static_cast<float>(pt.x) / g_scale;
            float y = static_cast<float>(pt.y) / g_scale;
            D2D1_RECT_F close = CloseRect();
            // 关闭按钮必须在客户区，否则会被当成标题栏拖动而收不到点击
            if (x >= close.left && x <= close.right && y >= close.top &&
                y <= close.bottom) {
                return HTCLIENT;
            }
            if (y < kHeaderH) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            Paint(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_ACTIVATE:
            // 失去激活时收起下拉列表
            if (LOWORD(wp) == WA_INACTIVE && g_listWnd) {
                CloseDropdown();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            CommitInput();
            CloseDropdown();
            g_closeHover = false;
            g_tracking = false;
            g_wnd = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void ShowSettingsWindow(HINSTANCE hinst, Settings* settings,
                        std::function<void()> onApply) {
    if (g_wnd) {
        SetForegroundWindow(g_wnd);
        return;
    }
    g_settings = settings;
    g_onApply = std::move(onApply);

    if (!g_d2d) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    if (!g_dwrite) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&g_dwrite));
    }
    if (!g_d2d || !g_dwrite) return;

    if (!g_titleFont) {
        CreateFormat(g_dwrite, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                     DWRITE_TEXT_ALIGNMENT_LEADING, &g_titleFont);
        CreateFormat(g_dwrite, 13.5f, DWRITE_FONT_WEIGHT_NORMAL,
                     DWRITE_TEXT_ALIGNMENT_LEADING, &g_rowFont);
        CreateFormat(g_dwrite, 12.5f, DWRITE_FONT_WEIGHT_MEDIUM,
                     DWRITE_TEXT_ALIGNMENT_CENTER, &g_valueFont);
        CreateFormat(g_dwrite, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
                     DWRITE_TEXT_ALIGNMENT_LEADING, &g_hintFont);
        CreateFormat(g_dwrite, 12.5f, DWRITE_FONT_WEIGHT_NORMAL,
                     DWRITE_TEXT_ALIGNMENT_LEADING, &g_optionFont);
    }
    if (!g_rt) {
        D2D1_RENDER_TARGET_PROPERTIES props = {};
        props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        props.usage = D2D1_RENDER_TARGET_USAGE_NONE;
        props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
        HRESULT hr = g_d2d->CreateDCRenderTarget(&props, &g_rt);
        if (FAILED(hr) || !g_rt) return;
    }
    if (!g_listRt) {
        D2D1_RENDER_TARGET_PROPERTIES props = {};
        props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        props.usage = D2D1_RENDER_TARGET_USAGE_NONE;
        props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
        HRESULT hr = g_d2d->CreateDCRenderTarget(&props, &g_listRt);
        if (FAILED(hr) || !g_listRt) return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &WndProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"UsbAssistantSettings";
        wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(1));
        wc.hIconSm = LoadIconW(hinst, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        registered = true;
    }

    HDC screen = GetDC(nullptr);
    int dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    g_scale = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = static_cast<int>(kLogicalW * g_scale);
    int h = static_cast<int>(kLogicalH * g_scale);
    int x = work.left + (work.right - work.left - w) / 2;
    int y = work.top + (work.bottom - work.top - h) / 2;

    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"UsbAssistantSettings",
                            L"U 盘助手 - 设置", WS_POPUP, x, y, w, h, nullptr,
                            nullptr, hinst, nullptr);
    if (!g_wnd) return;

    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1,
                                  static_cast<int>(16 * g_scale),
                                  static_cast<int>(16 * g_scale));
    SetWindowRgn(g_wnd, rgn, FALSE);

    ShowWindow(g_wnd, SW_SHOWNORMAL);
    UpdateWindow(g_wnd);
    SetForegroundWindow(g_wnd);
}
