#include "ui/text.h"

namespace usb::ui {

void Fonts::Release() {
    const auto release = [](IDWriteTextFormat*& format) {
        if (format) {
            format->Release();
            format = nullptr;
        }
    };
    release(title);
    release(subtitle);
    release(card_name);
    release(part_name);
    release(meta);
    release(pill);
    release(button);
    release(empty_title);
    release(empty_hint);
    release(toast);
    release(dialog_title);
    release(dialog_row);
}

IDWriteTextFormat* CreateFormat(IDWriteFactory* factory, const wchar_t* family,
                                float size, DWRITE_FONT_WEIGHT weight,
                                DWRITE_TEXT_ALIGNMENT align,
                                DWRITE_PARAGRAPH_ALIGNMENT paragraph,
                                bool wrap) {
    if (!factory) return nullptr;
    IDWriteTextFormat* format = nullptr;
    HRESULT result = factory->CreateTextFormat(
        family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", &format);
    if (FAILED(result) || !format) {
        result = factory->CreateTextFormat(
            L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", &format);
    }
    if (!format) return nullptr;

    format->SetTextAlignment(align);
    format->SetParagraphAlignment(paragraph);
    format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP
                                 : DWRITE_WORD_WRAPPING_NO_WRAP);
    return format;
}

bool CreateFonts(IDWriteFactory* factory, Fonts* fonts) {
    if (!factory || !fonts) return false;
    const wchar_t* family = L"Microsoft YaHei UI";

    fonts->title = CreateFormat(
        factory, family, 15.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->subtitle = CreateFormat(
        factory, family, 10.5f, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->card_name = CreateFormat(
        factory, family, 14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->part_name = CreateFormat(
        factory, family, 13.0f, DWRITE_FONT_WEIGHT_MEDIUM,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->meta = CreateFormat(
        factory, family, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->pill = CreateFormat(
        factory, family, 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->button = CreateFormat(
        factory, family, 12.5f, DWRITE_FONT_WEIGHT_MEDIUM,
        DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->empty_title = CreateFormat(
        factory, family, 14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->empty_hint = CreateFormat(
        factory, family, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fonts->toast = CreateFormat(
        factory, family, 11.5f, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    fonts->dialog_title = CreateFormat(
        factory, family, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    fonts->dialog_row = CreateFormat(
        factory, family, 12.0f, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);

    return fonts->title && fonts->subtitle && fonts->card_name &&
           fonts->part_name && fonts->meta && fonts->pill && fonts->button &&
           fonts->empty_title && fonts->empty_hint && fonts->toast &&
           fonts->dialog_title && fonts->dialog_row;
}

void ReleaseFonts(Fonts* fonts) {
    if (fonts) fonts->Release();
}

void DrawText(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
              const std::wstring& text, IDWriteTextFormat* format,
              const D2D1_RECT_F& rect, const D2D1_COLOR_F& color) {
    if (!target || !format || text.empty()) return;
    brush->SetColor(color);
    target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format,
                     &rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DrawTextTrim(IDWriteFactory* factory, ID2D1RenderTarget* target,
                  ID2D1SolidColorBrush* brush, const std::wstring& text,
                  IDWriteTextFormat* format, const D2D1_RECT_F& rect,
                  const D2D1_COLOR_F& color) {
    if (!factory || !format || text.empty()) return;
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    if (width <= 0.0f || height <= 0.0f) return;

    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory->CreateTextLayout(text.c_str(),
                                         static_cast<UINT32>(text.size()),
                                         format, width, height, &layout)) ||
        !layout) {
        DrawText(target, brush, text, format, rect, color);
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
    const D2D1_POINT_2F origin = {rect.left, rect.top};
    target->DrawTextLayout(origin, layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    layout->Release();
}

float MeasureWrappedHeight(IDWriteFactory* factory, IDWriteTextFormat* format,
                           const std::wstring& text, float width) {
    if (!factory || !format || text.empty() || width <= 0.0f) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory->CreateTextLayout(text.c_str(),
                                         static_cast<UINT32>(text.size()),
                                         format, width, 4096.0f, &layout)) ||
        !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics = {};
    const float height =
        SUCCEEDED(layout->GetMetrics(&metrics)) ? metrics.height : 0.0f;
    layout->Release();
    return height;
}

}  // namespace usb::ui
