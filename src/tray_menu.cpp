#include "tray_menu.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <windowsx.h>

#include <cmath>

#include "theme.h"

namespace {

HWND g_wnd = nullptr;
std::vector<std::wstring> g_items;
int g_danger = -1;
int g_hover = -1;
int g_choice = -1;
bool g_done = false;
float g_scale = 1.0f;

ID2D1Factory* g_d2d = nullptr;
ID2D1DCRenderTarget* g_rt = nullptr;
IDWriteFactory* g_dwrite = nullptr;
IDWriteTextFormat* g_font = nullptr;

const float kWidth = 172.0f;
const float kItemH = 34.0f;
const float kPad = 6.0f;
const float kGap = 6.0f; // 危险项前留出的间隔

float ItemTop(int index) {
    float top = kPad + index * kItemH;
    if (g_danger >= 0 && index >= g_danger) top += kGap;
    return top;
}

float MenuHeight() {
    float h = kPad * 2.0f + kItemH * static_cast<float>(g_items.size());
    if (g_danger >= 0) h += kGap;
    return h;
}

int HitItem(int yPx) {
    float y = static_cast<float>(yPx) / g_scale;
    for (int i = 0; i < static_cast<int>(g_items.size()); ++i) {
        float top = ItemTop(i);
        if (y >= top && y <= top + kItemH) return i;
    }
    return -1;
}

void CloseMenu() {
    g_done = true;
    if (g_wnd) {
        HWND wnd = g_wnd;
        g_wnd = nullptr;
        if (GetCapture() == wnd) ReleaseCapture();
        DestroyWindow(wnd);
    }
}

bool InitGraphics(HINSTANCE hinst) {
    if (!g_d2d) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
        if (FAILED(hr)) return false;
    }
    if (!g_dwrite) {
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                         __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(&g_dwrite));
        if (FAILED(hr)) return false;
    }
    if (!g_rt) {
        D2D1_RENDER_TARGET_PROPERTIES props = {};
        props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        props.usage = D2D1_RENDER_TARGET_USAGE_NONE;
        props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
        HRESULT hr = g_d2d->CreateDCRenderTarget(&props, &g_rt);
        if (FAILED(hr)) return false;
    }
    if (!g_font) {
        HRESULT hr = g_dwrite->CreateTextFormat(
            L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-cn",
            &g_font);
        if (FAILED(hr) || !g_font) {
            hr = g_dwrite->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f,
                L"zh-cn", &g_font);
        }
        if (!g_font) return false;
        g_font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return true;
}

void Paint(HDC hdc) {
    if (!g_rt || !g_font) return;
    float h = MenuHeight();
    RECT bindRect = {0, 0, static_cast<LONG>(kWidth * g_scale),
                     static_cast<LONG>(h * g_scale)};
    if (FAILED(g_rt->BindDC(hdc, &bindRect))) return;

    g_rt->SetDpi(96.0f * g_scale, 96.0f * g_scale);
    g_rt->BeginDraw();
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    D2D1_MATRIX_3X2_F identity = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    g_rt->SetTransform(&identity);

    Palette p = MakePalette(QuerySystemDarkMode());
    g_rt->Clear(p.card);

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(g_rt->CreateSolidColorBrush(p.textPrimary, &brush)) || !brush) {
        g_rt->EndDraw();
        return;
    }

    brush->SetColor(p.cardBorder);
    g_rt->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, kWidth - 0.5f, h - 0.5f), 12.0f,
                          12.0f),
        brush, 1.0f);

    for (int i = 0; i < static_cast<int>(g_items.size()); ++i) {
        float top = ItemTop(i);
        bool danger = i == g_danger;
        D2D1_RECT_F item = D2D1::RectF(6.0f, top, kWidth - 6.0f, top + kItemH);

        if (i == g_hover) {
            D2D1_COLOR_F fill = danger ? p.danger : p.hoverFill;
            fill.a = danger ? 0.16f : fill.a;
            brush->SetColor(fill);
            g_rt->FillRoundedRectangle(D2D1::RoundedRect(item, 8.0f, 8.0f), brush);
        }
        if (danger && i > 0) {
            brush->SetColor(p.cardBorder);
            g_rt->DrawLine(D2D1::Point2F(14.0f, top - kGap * 0.5f),
                           D2D1::Point2F(kWidth - 14.0f, top - kGap * 0.5f), brush,
                           1.0f);
        }

        brush->SetColor(danger ? p.danger : p.textPrimary);
        g_rt->DrawTextW(g_items[i].c_str(), static_cast<UINT>(g_items[i].size()),
                        g_font,
                        D2D1::RectF(item.left + 12.0f, item.top, item.right - 10.0f,
                                    item.bottom),
                        brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    brush->Release();
    g_rt->EndDraw();
}

LRESULT CALLBACK MenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            Paint(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            int hit = HitItem(GET_Y_LPARAM(lp));
            if (hit != g_hover) {
                g_hover = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            SetCursor(LoadCursorW(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_LBUTTONUP: {
            int hit = HitItem(GET_Y_LPARAM(lp));
            if (hit >= 0) g_choice = hit;
            CloseMenu();
            return 0;
        }
        case WM_RBUTTONUP:
            CloseMenu();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                CloseMenu();
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (!g_done) CloseMenu();
            return 0;
        case WM_NCDESTROY:
            g_wnd = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int ShowTrayMenuPopup(HINSTANCE hinst, POINT screenPoint,
                      const std::vector<std::wstring>& items, int dangerIndex) {
    g_items = items;
    g_danger = dangerIndex;
    g_hover = -1;
    g_choice = -1;
    g_done = false;

    HDC dc = GetDC(nullptr);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    g_scale = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;

    if (!InitGraphics(hinst) || g_items.empty()) return -1;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &MenuProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"UsbAssistantTrayMenu";
        RegisterClassExW(&wc);
        registered = true;
    }

    int w = static_cast<int>(std::lround(kWidth * g_scale));
    int h = static_cast<int>(std::lround(MenuHeight() * g_scale));
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = screenPoint.x - w + static_cast<int>(std::lround(12.0f * g_scale));
    int y = screenPoint.y - h - static_cast<int>(std::lround(10.0f * g_scale));
    if (x + w > work.right) x = work.right - w;
    if (x < work.left) x = work.left;
    if (y + h > work.bottom) y = work.bottom - h;
    if (y < work.top) y = work.top;

    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                            L"UsbAssistantTrayMenu",
                            L"", WS_POPUP, x, y, w, h, nullptr, nullptr, hinst,
                            nullptr);
    if (!g_wnd) return -1;

    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1,
                                  static_cast<int>(std::lround(12.0f * g_scale)),
                                  static_cast<int>(std::lround(12.0f * g_scale)));
    SetWindowRgn(g_wnd, rgn, FALSE);

    // 不抢前台/焦点/鼠标捕获：否则 Windows 托盘的折叠弹出区会被激活打断而自动消失
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    InvalidateRect(g_wnd, nullptr, FALSE);
    UpdateWindow(g_wnd);

    // 没有激活也收不到键盘消息，用轮询补上「点击菜单外 / 按 Esc 关闭」的行为
    bool prevDown = false;
    while (!g_done) {
        MSG msg = {};
        DWORD wait = MsgWaitForMultipleObjectsEx(0, nullptr, 40, QS_ALLINPUT,
                                                 MWMO_INPUTAVAILABLE);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                CloseMenu();
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_done) break;
        if (wait == WAIT_TIMEOUT) {
            bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            POINT pt = {};
            GetCursorPos(&pt);
            RECT rc = {};
            if (g_wnd) GetWindowRect(g_wnd, &rc);
            bool inside = g_wnd && PtInRect(&rc, pt);
            if (esc || (down && !inside) || (prevDown && !down && !inside)) {
                CloseMenu();
                break;
            }
            prevDown = down;
        }
    }
    return g_choice;
}
