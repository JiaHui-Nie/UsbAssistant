#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

class App;
class Canvas;
struct UiLayout;
struct Fonts;

bool InitFonts(IDWriteFactory* factory, Fonts* fonts);
void ReleaseFonts(Fonts* fonts);

// 自绘整个悬浮窗（逻辑坐标，单位为 DIP）
void PaintApp(const App& app, Canvas& canvas, const UiLayout& layout);
