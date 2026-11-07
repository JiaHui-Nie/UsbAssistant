#include "render.h"

#include "app.h"
#include "canvas.h"
#include "device.h"
#include "theme.h"

#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

static D2D1_RECT_F Rect(float left, float top, float right, float bottom) {
    D2D1_RECT_F r = {left, top, right, bottom};
    return r;
}

static D2D1_ROUNDED_RECT Round(const D2D1_RECT_F& r, float radius) {
    D2D1_ROUNDED_RECT out = {};
    out.rect = r;
    out.radiusX = radius;
    out.radiusY = radius;
    return out;
}

static D2D1_COLOR_F Alpha(const D2D1_COLOR_F& color, float alpha) {
    D2D1_COLOR_F out = color;
    out.a = color.a * alpha;
    return out;
}

static IDWriteTextFormat* MakeFormat(IDWriteFactory* factory, const wchar_t* family,
                                     float size, DWRITE_FONT_WEIGHT weight,
                                     DWRITE_TEXT_ALIGNMENT align,
                                     DWRITE_PARAGRAPH_ALIGNMENT para,
                                     bool wrap) {
    IDWriteTextFormat* format = nullptr;
    HRESULT hr = factory->CreateTextFormat(
        family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        size, L"zh-cn", &format);
    if (FAILED(hr) || !format) {
        hr = factory->CreateTextFormat(
            L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", &format);
    }
    if (!format) return nullptr;

    format->SetTextAlignment(align);
    format->SetParagraphAlignment(para);
    format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP
                                 : DWRITE_WORD_WRAPPING_NO_WRAP);
    return format;
}

bool InitFonts(IDWriteFactory* factory, Fonts* fonts) {
    if (!factory || !fonts) return false;
    const wchar_t* family = L"Microsoft YaHei UI";

    fonts->title = MakeFormat(factory, family, 15.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                              DWRITE_TEXT_ALIGNMENT_LEADING,
                              DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->subtitle = MakeFormat(factory, family, 10.5f, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_TEXT_ALIGNMENT_LEADING,
                                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->cardName = MakeFormat(factory, family, 14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                 DWRITE_TEXT_ALIGNMENT_LEADING,
                                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->partName = MakeFormat(factory, family, 13.0f, DWRITE_FONT_WEIGHT_MEDIUM,
                                 DWRITE_TEXT_ALIGNMENT_LEADING,
                                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->meta = MakeFormat(factory, family, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_TEXT_ALIGNMENT_LEADING,
                             DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->pill = MakeFormat(factory, family, 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_TEXT_ALIGNMENT_CENTER,
                             DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->button = MakeFormat(factory, family, 12.5f, DWRITE_FONT_WEIGHT_MEDIUM,
                               DWRITE_TEXT_ALIGNMENT_CENTER,
                               DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->emptyTitle = MakeFormat(factory, family, 14.5f,
                                   DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                   DWRITE_TEXT_ALIGNMENT_CENTER,
                                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->emptyHint = MakeFormat(factory, family, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_TEXT_ALIGNMENT_CENTER,
                                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->toast = MakeFormat(factory, family, 11.5f, DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_TEXT_ALIGNMENT_LEADING,
                              DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    fonts->dialogTitle = MakeFormat(factory, family, 13.5f,
                                    DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_TEXT_ALIGNMENT_LEADING,
                                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    fonts->dialogRow = MakeFormat(factory, family, 12.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_TEXT_ALIGNMENT_LEADING,
                                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);

    return fonts->title && fonts->subtitle && fonts->cardName && fonts->partName &&
           fonts->meta && fonts->pill && fonts->button && fonts->emptyTitle &&
           fonts->emptyHint && fonts->toast && fonts->dialogTitle &&
           fonts->dialogRow;
}

void Fonts::Release() {
    auto release = [](IDWriteTextFormat*& format) {
        if (format) {
            format->Release();
            format = nullptr;
        }
    };
    release(title);
    release(subtitle);
    release(cardName);
    release(partName);
    release(meta);
    release(pill);
    release(button);
    release(emptyTitle);
    release(emptyHint);
    release(toast);
    release(dialogTitle);
    release(dialogRow);
}

void ReleaseFonts(Fonts* fonts) {
    if (fonts) fonts->Release();
}

static void Text(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                 const std::wstring& text, IDWriteTextFormat* format,
                 const D2D1_RECT_F& rect, const D2D1_COLOR_F& color) {
    if (!format || text.empty()) return;
    brush->SetColor(color);
    rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, &rect,
                 brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// 单行文本，超宽时以省略号收尾
static void TextTrim(IDWriteFactory* factory, ID2D1RenderTarget* rt,
                     ID2D1SolidColorBrush* brush, const std::wstring& text,
                     IDWriteTextFormat* format, const D2D1_RECT_F& rect,
                     const D2D1_COLOR_F& color) {
    if (!factory || !format || text.empty()) return;
    float width = rect.right - rect.left;
    float height = rect.bottom - rect.top;
    if (width <= 0.0f || height <= 0.0f) return;

    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory->CreateTextLayout(text.c_str(),
                                         static_cast<UINT32>(text.size()), format,
                                         width, height, &layout)) ||
        !layout) {
        Text(rt, brush, text, format, rect, color);
        return;
    }

    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* sign = nullptr;
    if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format, &sign)) && sign) {
        layout->SetTrimming(&trimming, sign);
        sign->Release();
    }

    brush->SetColor(color);
    D2D1_POINT_2F origin = {rect.left, rect.top};
    rt->DrawTextLayout(origin, layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    layout->Release();
}

static void DrawPanelShadow(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                            const D2D1_RECT_F& panel, float radius, bool dark) {
    const int layers = 18;
    const float maxGrow = 15.0f;
    const float baseAlpha = dark ? 0.038f : 0.020f;
    for (int i = layers - 1; i >= 0; --i) {
        float t = static_cast<float>(i) / static_cast<float>(layers - 1);
        float grow = maxGrow * t;
        D2D1_COLOR_F color = {0.0f, 0.0f, 0.0f, baseAlpha};
        brush->SetColor(color);
        D2D1_RECT_F r = Rect(panel.left - grow, panel.top - grow + 2.0f,
                             panel.right + grow, panel.bottom + grow + 2.0f);
        rt->FillRoundedRectangle(Round(r, radius + grow), brush);
    }
}

// 横向 U 盘图标（接口帽 + 主体 + 指示灯）
static void DrawUsbGlyph(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                         float cx, float cy, float unit,
                         const D2D1_COLOR_F& color, float ledAlpha) {
    float bodyW = unit * 1.34f;
    float bodyH = unit * 0.86f;
    float connW = unit * 0.44f;
    float connH = unit * 0.40f;

    float total = connW + 1.0f + bodyW;
    float left = cx - total * 0.5f;
    float top = cy - bodyH * 0.5f;

    brush->SetColor(color);
    D2D1_RECT_F conn = Rect(left, cy - connH * 0.5f, left + connW,
                            cy + connH * 0.5f);
    rt->FillRoundedRectangle(Round(conn, connH * 0.24f), brush);

    D2D1_RECT_F body = Rect(left + connW + 1.0f, top, left + total, top + bodyH);
    rt->FillRoundedRectangle(Round(body, bodyH * 0.30f), brush);

    if (ledAlpha > 0.0f) {
        D2D1_COLOR_F led = {1.0f, 1.0f, 1.0f, ledAlpha};
        brush->SetColor(led);
        float ledSize = bodyH * 0.22f;
        float ledX = body.right - bodyH * 0.42f;
        D2D1_RECT_F ledRect = Rect(ledX - ledSize * 0.5f, cy - ledSize * 0.5f,
                                   ledX + ledSize * 0.5f, cy + ledSize * 0.5f);
        rt->FillRoundedRectangle(Round(ledRect, ledSize * 0.5f), brush);
    }
}

// 移动硬盘图标（圆角外壳 + 盘片 + 指示灯），与 U 盘图标形状区分
static void DrawHddGlyph(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                         float cx, float cy, float unit,
                         const D2D1_COLOR_F& color, float ledAlpha) {
    float bodyW = unit * 1.46f;
    float bodyH = unit * 1.02f;
    D2D1_RECT_F body = Rect(cx - bodyW * 0.5f, cy - bodyH * 0.5f,
                            cx + bodyW * 0.5f, cy + bodyH * 0.5f);
    brush->SetColor(color);
    rt->FillRoundedRectangle(Round(body, bodyH * 0.22f), brush);

    D2D1_COLOR_F white = {1.0f, 1.0f, 1.0f, 0.55f};
    brush->SetColor(white);
    D2D1_ELLIPSE platter = {{cx, cy}, bodyH * 0.29f, bodyH * 0.29f};
    rt->DrawEllipse(platter, brush, 1.5f);
    D2D1_ELLIPSE hub = {{cx, cy}, bodyH * 0.10f, bodyH * 0.10f};
    rt->FillEllipse(hub, brush);

    if (ledAlpha > 0.0f) {
        brush->SetColor({1.0f, 1.0f, 1.0f, ledAlpha});
        D2D1_ELLIPSE led = {{body.right - bodyH * 0.26f, body.bottom - bodyH * 0.22f},
                            bodyH * 0.09f, bodyH * 0.09f};
        rt->FillEllipse(led, brush);
    }
}

static void DrawHeader(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                       const App& app, const UiLayout& layout) {
    const Palette& p = app.Colors();
    const Fonts& f = app.Text();
    const D2D1_RECT_F& panel = layout.panel;

    Text(rt, brush, L"U 盘助手", f.title,
         Rect(panel.left + 18.0f, panel.top + 10.0f, panel.right - 100.0f,
              panel.top + 32.0f),
         p.textPrimary);

    std::wstring subtitle;
    if (app.Devices().empty()) {
        subtitle = L"等待插入设备";
    } else {
        size_t parts = 0;
        for (const auto& device : app.Devices()) parts += device.parts.size();
        subtitle = L"已连接 " + std::to_wstring(app.Devices().size()) + L" 台设备 · " +
                   std::to_wstring(parts) + L" 个分区";
    }
    Text(rt, brush, subtitle, f.subtitle,
         Rect(panel.left + 18.0f, panel.top + 31.0f, panel.right - 100.0f,
              panel.top + 47.0f),
         p.textSecondary);

    // 关闭（悬浮时变红）
    float hoverClose = app.HoverClose();
    if (hoverClose > 0.02f) {
        brush->SetColor(Alpha(p.danger, hoverClose));
        rt->FillRoundedRectangle(Round(layout.closeBtn, 13.0f), brush);
    }
    float closeCx = (layout.closeBtn.left + layout.closeBtn.right) * 0.5f;
    float closeCy = (layout.closeBtn.top + layout.closeBtn.bottom) * 0.5f;
    D2D1_COLOR_F xColor;
    xColor.r = p.textSecondary.r + (1.0f - p.textSecondary.r) * hoverClose;
    xColor.g = p.textSecondary.g + (1.0f - p.textSecondary.g) * hoverClose;
    xColor.b = p.textSecondary.b + (1.0f - p.textSecondary.b) * hoverClose;
    xColor.a = 0.55f + 0.45f * hoverClose;
    brush->SetColor(xColor);
    D2D1_POINT_2F c1a = {closeCx - 4.5f, closeCy - 4.5f};
    D2D1_POINT_2F c1b = {closeCx + 4.5f, closeCy + 4.5f};
    D2D1_POINT_2F c2a = {closeCx + 4.5f, closeCy - 4.5f};
    D2D1_POINT_2F c2b = {closeCx - 4.5f, closeCy + 4.5f};
    rt->DrawLine(c1a, c1b, brush, 1.4f);
    rt->DrawLine(c2a, c2b, brush, 1.4f);
}

static void DrawDeviceCard(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                           const App& app, const DeviceInfo& device,
                           const CardLayout& card) {
    const Palette& p = app.Colors();
    const Fonts& f = app.Text();

    brush->SetColor(p.card);
    rt->FillRoundedRectangle(Round(card.card, 12.0f), brush);
    brush->SetColor(p.cardBorder);
    rt->DrawRoundedRectangle(Round(card.card, 12.0f), brush, 1.0f);

    // 左侧图标块
    D2D1_RECT_F tile = Rect(card.card.left + 14.0f, card.card.top + 14.0f,
                            card.card.left + 50.0f, card.card.top + 50.0f);
    brush->SetColor(p.iconTile);
    rt->FillRoundedRectangle(Round(tile, 10.0f), brush);
    if (device.removableMedia) {
        DrawUsbGlyph(rt, brush, (tile.left + tile.right) * 0.5f,
                     (tile.top + tile.bottom) * 0.5f + 1.0f, 13.5f, p.accent, 0.92f);
    } else {
        DrawHddGlyph(rt, brush, (tile.left + tile.right) * 0.5f,
                     (tile.top + tile.bottom) * 0.5f, 13.0f, p.accent, 0.92f);
    }

    // 设备标题（硬件型号），过长时省略号收尾
    TextTrim(app.DWrite(), rt, brush, device.title, f.cardName,
             Rect(tile.right + 12.0f, card.card.top + 8.0f,
                  card.ejectBtn.left - 10.0f, card.card.top + 31.0f),
             p.textPrimary);

    // N 个分区 · 共 X
    std::wstring subtitle = std::to_wstring(device.parts.size()) + L" 个分区";
    if (device.total > 0) subtitle += L" · 共 " + FormatBytes(device.total);
    Text(rt, brush, subtitle, f.meta,
         Rect(tile.right + 12.0f, card.card.top + 32.0f,
              card.ejectBtn.left - 10.0f, card.card.top + 50.0f),
         p.textSecondary);

    // 安全弹出：对整个物理设备生效
    bool ejecting = device.ejecting;
    brush->SetColor(ejecting ? Alpha(p.secondaryBtn, 0.55f) : p.secondaryBtn);
    rt->FillRoundedRectangle(Round(card.ejectBtn, 8.0f), brush);
    if (!ejecting && device.hoverEject > 0.02f) {
        brush->SetColor(Alpha(p.hoverFill, device.hoverEject));
        rt->FillRoundedRectangle(Round(card.ejectBtn, 8.0f), brush);
    }
    Text(rt, brush, ejecting ? L"正在弹出…" : L"安全弹出", f.button, card.ejectBtn,
         ejecting ? Alpha(p.textSecondary, 0.65f) : p.secondaryText);

    // 头部与分区之间的分隔线
    float headerBottom = card.parts.empty() ? card.card.bottom - 10.0f
                                            : card.parts.front().row.top;
    brush->SetColor(Alpha(p.textPrimary, 0.06f));
    D2D1_POINT_2F lineA = {card.card.left + 14.0f, headerBottom};
    D2D1_POINT_2F lineB = {card.card.right - 14.0f, headerBottom};
    rt->DrawLine(lineA, lineB, brush, 1.0f);

    // 分区行
    for (size_t i = 0; i < device.parts.size() && i < card.parts.size(); ++i) {
        const PartitionInfo& part = device.parts[i];
        const PartitionLayout& row = card.parts[i];
        const float rowTop = row.row.top;
        const float right = row.row.right - 14.0f;

        // 分区之间的细分隔线
        if (i > 0) {
            brush->SetColor(Alpha(p.textPrimary, 0.045f));
            D2D1_POINT_2F partLineA = {row.row.left + 14.0f, rowTop};
            D2D1_POINT_2F partLineB = {row.row.right - 14.0f, rowTop};
            rt->DrawLine(partLineA, partLineB, brush, 1.0f);
        }

        // 盘符徽标
        brush->SetColor(part.letter ? p.accentSoft : Alpha(p.track, 0.9f));
        rt->FillRoundedRectangle(Round(row.pill, 9.0f), brush);
        Text(rt, brush,
             part.letter ? (std::wstring(1, part.letter) + L":") : std::wstring(L"—"),
             f.pill, row.pill, part.letter ? p.accent : p.textTertiary);

        // 卷标（无卷标时已回退为盘符）
        TextTrim(app.DWrite(), rt, brush, part.name, f.partName,
                 Rect(row.pill.right + 10.0f, rowTop + 5.0f,
                      row.openBtn.left - 10.0f, rowTop + 27.0f),
                 p.textPrimary);

        uint64_t used = part.total > part.freeBytes ? part.total - part.freeBytes : 0;

        // 文件系统 · 已用 · 可用
        std::wstring meta = part.fs + L" · 已用 " + FormatBytes(used) + L" · 可用 " +
                            FormatBytes(part.freeBytes);
        float metaRight = part.openable ? row.openBtn.left - 10.0f : right;
        Text(rt, brush, meta, f.meta,
             Rect(row.row.left + 20.0f, rowTop + 29.0f, metaRight, rowTop + 46.0f),
             p.textTertiary);

        // 容量条
        double ratio = part.total == 0
                           ? 0.0
                           : static_cast<double>(used) / static_cast<double>(part.total);
        D2D1_RECT_F bar = Rect(row.row.left + 14.0f, rowTop + 46.0f, right,
                               rowTop + 51.0f);
        brush->SetColor(p.track);
        rt->FillRoundedRectangle(Round(bar, 2.5f), brush);
        if (ratio > 0.0) {
            float barWidth = std::max(
                static_cast<float>((bar.right - bar.left) * ratio), 6.0f);
            D2D1_RECT_F fill = Rect(bar.left, bar.top, bar.left + barWidth,
                                    bar.bottom);
            D2D1_COLOR_F fillColor = ratio > 0.90   ? p.danger
                                     : ratio > 0.80 ? p.warn
                                                    : p.accent;
            brush->SetColor(fillColor);
            rt->FillRoundedRectangle(Round(fill, 2.5f), brush);
        }

        // 仅资源管理器支持的文件系统显示"打开"（单个分区操作）
        if (part.openable) {
            brush->SetColor(p.accent);
            rt->FillRoundedRectangle(Round(row.openBtn, 8.0f), brush);
            if (part.hoverOpen > 0.02f) {
                D2D1_COLOR_F highlight = {1.0f, 1.0f, 1.0f, 0.12f * part.hoverOpen};
                brush->SetColor(highlight);
                rt->FillRoundedRectangle(Round(row.openBtn, 8.0f), brush);
            }
            Text(rt, brush, L"打开", f.button, row.openBtn, p.onAccent);
        }
    }
}

static void DrawEmptyState(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                           const App& app, const UiLayout& layout) {
    const Palette& p = app.Colors();
    const Fonts& f = app.Text();

    float centerX = (layout.contentClip.left + layout.contentClip.right) * 0.5f;
    float top = layout.contentTop + 8.0f;

    brush->SetColor(Alpha(p.textPrimary, 0.045f));
    D2D1_ELLIPSE circle = {{centerX, top + 42.0f}, 42.0f, 42.0f};
    rt->FillEllipse(circle, brush);

    DrawUsbGlyph(rt, brush, centerX, top + 44.0f, 21.0f,
                 Alpha(p.textTertiary, 0.85f), 0.0f);

    Text(rt, brush, L"未检测到 USB 存储设备", f.emptyTitle,
         Rect(layout.contentClip.left, top + 96.0f, layout.contentClip.right,
              top + 122.0f),
         p.textPrimary);
    Text(rt, brush, L"插入 U 盘或移动硬盘后将自动显示", f.emptyHint,
         Rect(layout.contentClip.left, top + 124.0f, layout.contentClip.right,
              top + 146.0f),
         p.textSecondary);
}

static void DrawScrollBar(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                          const App& app, const UiLayout& layout) {
    if (!layout.scrolling || layout.scrollMax <= 0.0f) return;
    const Palette& p = app.Colors();

    // 悬浮或被拖动时变粗、变明显
    float grow = std::min(std::max(app.ScrollGrow(), 0.0f), 1.0f);
    float width = 3.0f + 5.0f * grow;
    float centerX = layout.panel.right - 7.0f;

    float viewport = layout.contentClip.bottom - layout.contentClip.top;
    float content = viewport + layout.scrollMax;
    D2D1_RECT_F track = Rect(centerX - width * 0.5f,
                             layout.contentClip.top + 4.0f,
                             centerX + width * 0.5f,
                             layout.contentClip.bottom - 4.0f);
    brush->SetColor(Alpha(p.textPrimary, 0.10f + 0.06f * grow));
    rt->FillRoundedRectangle(Round(track, width * 0.5f), brush);

    float trackHeight = track.bottom - track.top;
    float thumbHeight = std::max(28.0f, trackHeight * viewport / content);
    float progress = layout.scrollMax > 0.0f ? app.ScrollY() / layout.scrollMax : 0.0f;
    progress = std::min(std::max(progress, 0.0f), 1.0f);
    float thumbTop = track.top + (trackHeight - thumbHeight) * progress;
    D2D1_RECT_F thumb = Rect(track.left, thumbTop, track.right, thumbTop + thumbHeight);
    brush->SetColor(Alpha(p.textPrimary, 0.32f + 0.22f * grow));
    rt->FillRoundedRectangle(Round(thumb, width * 0.5f), brush);
}

static void DrawToast(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                      const App& app, const UiLayout& layout) {
    float opacity = app.ToastOpacity();
    if (!app.ToastOpacity() || opacity <= 0.02f) return;

    const Palette& p = app.Colors();
    const Fonts& f = app.Text();
    const std::wstring& message = app.ToastText();
    float left = layout.panel.left + 14.0f;
    float right = layout.panel.right - 14.0f;
    float textLeft = left + 30.0f;
    float textRight = right - 14.0f;

    // 按实际排版高度撑开气泡，长提示不会再被固定高度挤压
    float textHeight = 32.0f;
    IDWriteFactory* dwrite = app.DWrite();
    if (dwrite && f.toast && !message.empty()) {
        IDWriteTextLayout* textLayout = nullptr;
        HRESULT hr = dwrite->CreateTextLayout(
            message.c_str(), static_cast<UINT>(message.size()), f.toast,
            textRight - textLeft, 240.0f, &textLayout);
        if (SUCCEEDED(hr) && textLayout) {
            DWRITE_TEXT_METRICS metrics = {};
            if (SUCCEEDED(textLayout->GetMetrics(&metrics)) && metrics.height > 0) {
                textHeight = metrics.height;
            }
            textLayout->Release();
        }
    }
    float maxHeight = layout.panel.bottom - layout.panel.top - 12.0f;
    float height = std::min(std::max(46.0f, textHeight + 16.0f), maxHeight);

    D2D1_RECT_F rect = Rect(left, layout.panel.bottom - 14.0f - height, right,
                            layout.panel.bottom - 14.0f);

    brush->SetColor(Alpha(p.toast, 0.97f * opacity));
    rt->FillRoundedRectangle(Round(rect, 10.0f), brush);

    D2D1_COLOR_F dotColor = app.ToastSuccess() ? p.success : p.danger;
    brush->SetColor(Alpha(dotColor, opacity));
    D2D1_ELLIPSE dot = {{rect.left + 16.0f, (rect.top + rect.bottom) * 0.5f}, 3.5f,
                        3.5f};
    rt->FillEllipse(dot, brush);

    Text(rt, brush, message, f.toast,
         Rect(rect.left + 30.0f, rect.top + 8.0f, rect.right - 14.0f,
              rect.bottom - 8.0f),
         Alpha(p.onToast, opacity));
}

// 退出确认弹层：遮罩 + 居中卡片
static void DrawConfirmDialog(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                              const App& app, const UiLayout& layout) {
    const Palette& p = app.Colors();
    const Fonts& f = app.Text();
    const D2D1_RECT_F& d = layout.dlgCard;

    // 遮罩：随系统深浅色使用主题底色，不再压黑整屏
    brush->SetColor(Alpha(p.panel, 0.62f));
    rt->FillRoundedRectangle(Round(layout.panel, 16.0f), brush);

    brush->SetColor(p.card);
    rt->FillRoundedRectangle(Round(d, 14.0f), brush);
    brush->SetColor(p.panelBorder);
    rt->DrawRoundedRectangle(Round(d, 14.0f), brush, 1.0f);

    Text(rt, brush, L"提示", f.title,
         Rect(d.left + 18.0f, d.top + 14.0f, d.right - 18.0f, d.top + 38.0f),
         p.textPrimary);
    Text(rt, brush,
         L"您是否想要退出本程序？您可在设置中修改关闭按钮的功能。", f.toast,
         Rect(d.left + 18.0f, d.top + 42.0f, d.right - 18.0f, d.top + 86.0f),
         p.textSecondary);

    // 「不再提示」复选框
    const D2D1_RECT_F& box = layout.dlgCheck;
    if (app.ConfirmDontAsk()) {
        brush->SetColor(p.accent);
        rt->FillRoundedRectangle(Round(box, 4.0f), brush);
        brush->SetColor(p.onAccent);
        D2D1_POINT_2F t1 = {box.left + 3.5f, (box.top + box.bottom) * 0.5f + 0.5f};
        D2D1_POINT_2F t2 = {box.left + 6.8f, box.bottom - 4.0f};
        D2D1_POINT_2F t3 = {box.right - 3.5f, box.top + 3.8f};
        rt->DrawLine(t1, t2, brush, 1.7f);
        rt->DrawLine(t2, t3, brush, 1.7f);
    } else {
        brush->SetColor(Alpha(p.textTertiary, 0.9f));
        rt->DrawRoundedRectangle(Round(box, 4.0f), brush, 1.4f);
    }
    Text(rt, brush, L"不再提示", f.meta,
         Rect(box.right + 8.0f, box.top - 3.0f, box.right + 100.0f, box.bottom + 3.0f),
         p.textPrimary);

    // 取消（次按钮）/ 退出（红色主按钮）
    brush->SetColor(p.secondaryBtn);
    rt->FillRoundedRectangle(Round(layout.dlgCancel, 8.0f), brush);
    Text(rt, brush, L"取消", f.button, layout.dlgCancel, p.secondaryText);

    brush->SetColor(p.danger);
    rt->FillRoundedRectangle(Round(layout.dlgOk, 8.0f), brush);
    D2D1_COLOR_F white = {1.0f, 1.0f, 1.0f, 1.0f};
    Text(rt, brush, L"退出", f.button, layout.dlgOk, white);
}

// HICON -> D2D 位图（经 GDI+ 读取为预乘 alpha 的 BGRA 像素）
static bool CreateBitmapFromIcon(ID2D1RenderTarget* rt, HICON icon,
                                 ID2D1Bitmap** out) {
    if (!rt || !icon || !out) return false;
    *out = nullptr;
    static ULONG_PTR gdiplusToken = 0;
    if (!gdiplusToken) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr) != Gdiplus::Ok) {
            gdiplusToken = 0;
            return false;
        }
    }
    Gdiplus::Bitmap* source = Gdiplus::Bitmap::FromHICON(icon);
    if (!source) return false;
    UINT width = source->GetWidth();
    UINT height = source->GetHeight();
    if (width == 0 || height == 0) {
        delete source;
        return false;
    }
    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    Gdiplus::BitmapData data = {};
    if (source->LockBits(&rect, Gdiplus::ImageLockModeRead,
                         PixelFormat32bppPARGB, &data) != Gdiplus::Ok) {
        delete source;
        return false;
    }
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    HRESULT hr = rt->CreateBitmap(D2D1::SizeU(width, height), data.Scan0,
                                  static_cast<UINT>(data.Stride), props, out);
    source->UnlockBits(&data);
    delete source;
    return SUCCEEDED(hr) && *out;
}

// 设备被占用弹层：标题 + 可滚动的进程列表 + 取消 / 解除占用
static void DrawBusyDialog(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
                           const App& app, const UiLayout& layout) {
    const Palette& p = app.Colors();
    const Fonts& f = app.Text();
    const D2D1_RECT_F& d = layout.busyCard;

    brush->SetColor(Alpha(p.panel, 0.62f));
    rt->FillRoundedRectangle(Round(layout.panel, 16.0f), brush);

    brush->SetColor(p.card);
    rt->FillRoundedRectangle(Round(d, 14.0f), brush);
    brush->SetColor(p.panelBorder);
    rt->DrawRoundedRectangle(Round(d, 14.0f), brush, 1.0f);

    // 标题：设备名过长时自动换行，最多两行
    std::wstring title =
        L"「" + app.BusyTitle() + L"」正在被占用，是否要解除占用并弹出？";
    IDWriteFactory* dwrite = app.DWrite();
    if (dwrite && f.dialogTitle && !title.empty()) {
        IDWriteTextLayout* textLayout = nullptr;
        if (SUCCEEDED(dwrite->CreateTextLayout(
                title.c_str(), static_cast<UINT32>(title.size()), f.dialogTitle,
                layout.busyTitle.right - layout.busyTitle.left,
                layout.busyTitle.bottom - layout.busyTitle.top, &textLayout)) &&
            textLayout) {
            brush->SetColor(p.textPrimary);
            D2D1_POINT_2F origin = {layout.busyTitle.left, layout.busyTitle.top};
            rt->DrawTextLayout(origin, textLayout, brush,
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            textLayout->Release();
        }
    }

    // 列表底
    D2D1_RECT_F list = layout.busyList;
    brush->SetColor(Alpha(p.textPrimary, p.dark ? 0.06f : 0.04f));
    rt->FillRoundedRectangle(Round(list, 8.0f), brush);
    brush->SetColor(Alpha(p.cardBorder, 0.9f));
    rt->DrawRoundedRectangle(Round(list, 8.0f), brush, 1.0f);

    const float rowH = 34.0f;
    const std::vector<VolumeOccupant>& list_items = app.BusyOccupants();
    const float scroll = app.BusyScroll();
    bool showBar = layout.busyScrollMax > 0.5f;

    D2D1_RECT_F rows = {list.left + 6.0f, list.top + 4.0f,
                        list.right - (showBar ? 14.0f : 6.0f), list.bottom - 4.0f};

    if (list_items.empty()) {
        const wchar_t* hint = app.BusyScanning()
                                  ? L"正在查找占用进程…"
                                  : L"未找到占用进程，可能由系统服务或驱动占用。";
        Text(rt, brush, hint, f.emptyHint,
             Rect(list.left + 10.0f, list.top, list.right - 10.0f, list.bottom),
             p.textTertiary);
    } else {
        rt->PushAxisAlignedClip(rows, D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t i = 0; i < list_items.size(); ++i) {
            float top = rows.top - scroll + rowH * static_cast<float>(i);
            if (top > rows.bottom || top + rowH < rows.top) continue;
            const VolumeOccupant& item = list_items[i];
            float centerY = top + rowH * 0.5f;
            D2D1_RECT_F iconRect = Rect(rows.left + 4.0f, centerY - 10.0f,
                                        rows.left + 24.0f, centerY + 10.0f);
            ID2D1Bitmap* iconBitmap = nullptr;
            if (item.icon && CreateBitmapFromIcon(rt, item.icon, &iconBitmap) &&
                iconBitmap) {
                rt->DrawBitmap(iconBitmap, iconRect, 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                iconBitmap->Release();
            } else {
                // 拿不到图标时画一个占位方块
                brush->SetColor(Alpha(p.textPrimary, 0.10f));
                rt->FillRoundedRectangle(Round(iconRect, 4.0f), brush);
            }
            TextTrim(dwrite, rt, brush, item.name, f.dialogRow,
                     Rect(rows.left + 32.0f, top, rows.right - 6.0f, top + rowH),
                     p.textPrimary);
        }
        rt->PopAxisAlignedClip();
    }

    // 列表滚动条
    if (showBar) {
        float x = list.right - 9.0f;
        D2D1_RECT_F track = Rect(x, list.top + 8.0f, x + 3.0f, list.bottom - 8.0f);
        brush->SetColor(Alpha(p.textPrimary, 0.10f));
        rt->FillRoundedRectangle(Round(track, 1.5f), brush);

        float trackHeight = track.bottom - track.top;
        float viewport = rows.bottom - rows.top;
        float content = viewport + layout.busyScrollMax;
        float thumbHeight =
            std::max(24.0f, trackHeight * viewport / std::max(content, 1.0f));
        float progress = layout.busyScrollMax > 0.0f ? scroll / layout.busyScrollMax
                                                     : 0.0f;
        progress = std::min(std::max(progress, 0.0f), 1.0f);
        float thumbTop = track.top + (trackHeight - thumbHeight) * progress;
        brush->SetColor(Alpha(p.textPrimary, 0.32f));
        rt->FillRoundedRectangle(
            Round(Rect(track.left, thumbTop, track.right, thumbTop + thumbHeight),
                  1.5f),
            brush);
    }

    // 取消（左）/ 解除占用（右，红色）
    brush->SetColor(p.secondaryBtn);
    rt->FillRoundedRectangle(Round(layout.busyCancel, 8.0f), brush);
    Text(rt, brush, L"取消", f.button, layout.busyCancel, p.secondaryText);

    brush->SetColor(p.danger);
    rt->FillRoundedRectangle(Round(layout.busyRelease, 8.0f), brush);
    D2D1_COLOR_F white = {1.0f, 1.0f, 1.0f, 1.0f};
    Text(rt, brush, L"解除占用", f.button, layout.busyRelease, white);
}

void PaintApp(const App& app, Canvas& canvas, const UiLayout& layout) {
    ID2D1RenderTarget* rt = canvas.Rt();
    if (!rt) return;

    D2D1_COLOR_F white = {1.0f, 1.0f, 1.0f, 1.0f};
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(white, &brush)) || !brush) return;

    const Palette& p = app.Colors();
    const D2D1_RECT_F& panel = layout.panel;

    DrawPanelShadow(rt, brush, panel, 16.0f, p.dark);
    brush->SetColor(p.panel);
    rt->FillRoundedRectangle(Round(panel, 16.0f), brush);
    brush->SetColor(p.panelBorder);
    rt->DrawRoundedRectangle(Round(panel, 16.0f), brush, 1.0f);

    DrawHeader(rt, brush, app, layout);

    rt->PushAxisAlignedClip(layout.contentClip, D2D1_ANTIALIAS_MODE_ALIASED);

    const std::vector<DeviceInfo>& devices = app.Devices();
    if (devices.empty()) {
        DrawEmptyState(rt, brush, app, layout);
    } else {
        for (size_t i = 0; i < devices.size() && i < layout.cards.size(); ++i) {
            DrawDeviceCard(rt, brush, app, devices[i], layout.cards[i]);
        }
    }

    rt->PopAxisAlignedClip();

    DrawScrollBar(rt, brush, app, layout);
    if (app.ConfirmVisible()) DrawConfirmDialog(rt, brush, app, layout);
    if (app.BusyVisible()) DrawBusyDialog(rt, brush, app, layout);
    // 提示气泡画在弹层之上，扫描中的提示不会被遮罩盖住
    DrawToast(rt, brush, app, layout);

    brush->Release();
}
