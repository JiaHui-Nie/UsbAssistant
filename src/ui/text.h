// DirectWrite 字体与文本绘制。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>

namespace usb::ui {

struct Fonts {
    IDWriteTextFormat* title = nullptr;
    IDWriteTextFormat* subtitle = nullptr;
    IDWriteTextFormat* card_name = nullptr;
    IDWriteTextFormat* part_name = nullptr;
    IDWriteTextFormat* meta = nullptr;
    IDWriteTextFormat* pill = nullptr;
    IDWriteTextFormat* button = nullptr;
    IDWriteTextFormat* empty_title = nullptr;
    IDWriteTextFormat* empty_hint = nullptr;
    IDWriteTextFormat* toast = nullptr;
    IDWriteTextFormat* dialog_title = nullptr;
    IDWriteTextFormat* dialog_row = nullptr;
    IDWriteTextFormat* dialog_message = nullptr;

    void Release();
};

bool CreateFonts(IDWriteFactory* factory, Fonts* fonts);
void ReleaseFonts(Fonts* fonts);

IDWriteTextFormat* CreateFormat(IDWriteFactory* factory, const wchar_t* family,
                                float size, DWRITE_FONT_WEIGHT weight,
                                DWRITE_TEXT_ALIGNMENT align,
                                DWRITE_PARAGRAPH_ALIGNMENT paragraph,
                                bool wrap);

void DrawText(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
              const std::wstring& text, IDWriteTextFormat* format,
              const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);

// 单行文本，超宽时用省略号收尾。
void DrawTextTrim(IDWriteFactory* factory, ID2D1RenderTarget* target,
                  ID2D1SolidColorBrush* brush, const std::wstring& text,
                  IDWriteTextFormat* format, const D2D1_RECT_F& rect,
                  const D2D1_COLOR_F& color);

float MeasureWrappedHeight(IDWriteFactory* factory, IDWriteTextFormat* format,
                           const std::wstring& text, float width);

}  // namespace usb::ui
