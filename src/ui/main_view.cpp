#include "ui/main_view.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "core/text.h"
#include "ui/draw.h"

namespace usb::ui {
namespace {

constexpr float kHeaderHeight = 52.0f;
constexpr float kPadX = 14.0f;
constexpr float kPadTop = 8.0f;
constexpr float kPadBottom = 14.0f;
constexpr float kCardGap = 10.0f;
constexpr float kCardHeaderHeight = 64.0f;
constexpr float kPartitionHeight = 62.0f;
constexpr float kCardBottom = 10.0f;
constexpr float kEmptyHeight = 176.0f;
constexpr float kButtonHeight = 26.0f;
constexpr float kOpenWidth = 56.0f;
constexpr float kMountWidth = 64.0f;
constexpr float kUnmountWidth = 78.0f;
constexpr float kActionGap = 6.0f;
constexpr float kEjectWidth = 88.0f;
constexpr float kCloseSize = 26.0f;
constexpr float kDialogRowHeight = 34.0f;

float CardHeight(const Device& device) {
    return kCardHeaderHeight +
           kPartitionHeight * static_cast<float>(device.partitions.size()) +
           kCardBottom;
}

bool IsUnknownFileSystem(const std::wstring& filesystem) {
    return filesystem == L"未知文件系统";
}

bool Ease(float& value, float target, float rate, float epsilon) {
    const float delta = target - value;
    if (std::fabs(delta) <= epsilon) {
        if (value != target) {
            value = target;
            return true;
        }
        return false;
    }
    value += delta * rate;
    return true;
}

// 在两个颜色之间线性插值（含 alpha）。用于悬停颜色过渡，保证淡入淡出
// 始终沿同一路径单调变化，不会在某个阈值处换成另一种颜色。
D2D1_COLOR_F MixColor(const D2D1_COLOR_F& from, const D2D1_COLOR_F& to,
                      float t) {
    const float k = std::min(std::max(t, 0.0f), 1.0f);
    D2D1_COLOR_F color;
    color.r = from.r + (to.r - from.r) * k;
    color.g = from.g + (to.g - from.g) * k;
    color.b = from.b + (to.b - from.b) * k;
    color.a = from.a + (to.a - from.a) * k;
    return color;
}

}  // namespace

// ---------------------------------------------------------------------------
// 模型与布局
// ---------------------------------------------------------------------------

MainView::~MainView() { shadow_.Release(); }

void MainView::ShadowCache::Release() {
    if (bitmap) {
        bitmap->Release();
        bitmap = nullptr;
    }
    owner = nullptr;
    width = 0;
    height = 0;
    dpi = 0.0f;
}

void MainView::SetModel(const Model& model) {
    const bool dialog_changed = model_.dialog != model.dialog;
    const auto previous_devices = model_.devices;
    model_ = model;
    if (dialog_changed) {
        pressed_ = Hit{};
        hovered_ = Hit{};
        hover_close_target_ = 0.0f;
        if (model.dialog == DialogKind::Busy) {
            busy_scroll_ = 0.0f;
            busy_scroll_target_ = 0.0f;
        }
    }
    if (previous_devices != model_.devices) {
        // 设备列表每几秒刷新一次，不能因此丢掉悬停：否则静止不动的指针
        // 会在下一次扫描后被淡出（且没有新的 WM_MOUSEMOVE 来恢复）。
        // 先保留状态，布局重建后按最后指针位置重新做命中判定。
        pressed_ = Hit{};
        if (pointer_inside_) {
            hover_revalidate_ = true;
        } else {
            hovered_ = Hit{};
        }
    }
}

float MainView::content_height() const {
    if (!model_.devices || model_.devices->empty()) return kEmptyHeight;
    float height = 0.0f;
    for (const auto& device : *model_.devices) height += CardHeight(device);
    height += static_cast<float>(model_.devices->size() - 1) * kCardGap;
    return height;
}

float MainView::preferred_panel_height(float max_panel_height) const {
    const float wanted = kHeaderHeight + kPadTop + content_height() + kPadBottom;
    const float limit = std::max(120.0f, max_panel_height);
    return std::min(wanted, limit);
}

void MainView::Layout(float panel_height, float scale) {
    scale_ = scale > 0.0f ? scale : 1.0f;
    BuildLayout(panel_height);
}

void MainView::BuildLayout(float panel_height) {
    ViewLayout layout;
    layout.window_w = kPanelWidth + kShadowPad * 2.0f;
    layout.window_h = panel_height + kShadowPad * 2.0f;
    layout.panel = Rect(kShadowPad, kShadowPad, kShadowPad + kPanelWidth,
                        kShadowPad + panel_height);

    const float center_y = layout.panel.top + kHeaderHeight * 0.5f;
    layout.close_button = Rect(layout.panel.right - 14.0f - kCloseSize,
                               center_y - kCloseSize * 0.5f,
                               layout.panel.right - 14.0f,
                               center_y + kCloseSize * 0.5f);

    layout.content_top = layout.panel.top + kHeaderHeight + kPadTop;
    float content_bottom = layout.panel.bottom - kPadBottom;
    if (content_bottom < layout.content_top) content_bottom = layout.content_top;
    layout.content_clip = Rect(layout.panel.left + kPadX, layout.content_top,
                               layout.panel.right - kPadX, content_bottom);

    const float viewport_height =
        layout.content_clip.bottom - layout.content_clip.top;
    layout.scroll_max = std::max(0.0f, content_height() - viewport_height);
    layout.scrolling = layout.scroll_max > 0.5f;
    if (layout.scrolling) {
        layout.scroll_bar = Rect(layout.panel.right - 12.0f,
                                 layout.content_clip.top,
                                 layout.panel.right - 2.0f,
                                 layout.content_clip.bottom);
    }
    scroll_y_ = std::min(std::max(scroll_y_, 0.0f), layout.scroll_max);
    scroll_target_ = std::min(std::max(scroll_target_, 0.0f), layout.scroll_max);

    if (model_.devices) {
        const float card_width =
            layout.content_clip.right - layout.content_clip.left;
        float top = layout.content_top - scroll_y_;
        for (const auto& device : *model_.devices) {
            const float height = CardHeight(device);
            CardLayout card;
            card.card = Rect(layout.content_clip.left, top,
                             layout.content_clip.left + card_width, top + height);
            const float right = card.card.right - 14.0f;
            card.eject_button =
                Rect(right - kEjectWidth, card.card.top + 8.0f, right,
                     card.card.top + kCardHeaderHeight - 8.0f);
            for (std::size_t i = 0; i < device.partitions.size(); ++i) {
                const float row_top =
                    card.card.top + kCardHeaderHeight +
                    kPartitionHeight * static_cast<float>(i);
                PartitionLayout part;
                part.row = Rect(card.card.left, row_top, card.card.right,
                                row_top + kPartitionHeight);
                part.pill = Rect(card.card.left + 14.0f, row_top + 8.0f,
                                 card.card.left + 54.0f, row_top + 28.0f);
                part.open_button =
                    Rect(right - kOpenWidth, row_top + 12.0f, right,
                         row_top + 12.0f + kButtonHeight);
                part.mount_button =
                    Rect(right - kMountWidth, row_top + 12.0f, right,
                         row_top + 12.0f + kButtonHeight);
                part.unmount_button = Rect(
                    part.open_button.left - kActionGap - kUnmountWidth,
                    row_top + 12.0f, part.open_button.left - kActionGap,
                    row_top + 12.0f + kButtonHeight);
                card.partitions.push_back(part);
            }
            layout.cards.push_back(std::move(card));
            top += height + kCardGap;
        }
    }

    // 退出确认弹层
    if (model_.dialog == DialogKind::ConfirmClose) {
        constexpr float kWidth = 300.0f;
        constexpr float kHeight = 156.0f;
        const float left = layout.panel.left +
                           (layout.panel.right - layout.panel.left - kWidth) *
                               0.5f;
        const float top = layout.panel.top +
                          (layout.panel.bottom - layout.panel.top - kHeight) *
                              0.5f;
        layout.confirm_card = Rect(left, top, left + kWidth, top + kHeight);
        layout.confirm_check = Rect(left + 18.0f, top + kHeight - 50.0f,
                                    left + 34.0f, top + kHeight - 34.0f);
        constexpr float kButtonW = 76.0f;
        constexpr float kButtonH = 28.0f;
        layout.confirm_ok =
            Rect(left + kWidth - 16.0f - kButtonW,
                 top + kHeight - 14.0f - kButtonH, left + kWidth - 16.0f,
                 top + kHeight - 14.0f);
        layout.confirm_cancel =
            Rect(layout.confirm_ok.left - 10.0f - kButtonW,
                 layout.confirm_ok.top, layout.confirm_ok.left - 10.0f,
                 layout.confirm_ok.bottom);
    }

    // 设备被占用弹层
    if (model_.dialog == DialogKind::Busy) {
        constexpr float kWidth = 332.0f;
        constexpr float kHeight = 236.0f;
        const float left = layout.panel.left +
                           (layout.panel.right - layout.panel.left - kWidth) *
                               0.5f;
        const float top = layout.panel.top +
                          (layout.panel.bottom - layout.panel.top - kHeight) *
                              0.5f;
        layout.busy_card = Rect(left, top, left + kWidth, top + kHeight);
        layout.busy_title =
            Rect(left + 18.0f, top + 18.0f, left + kWidth - 18.0f, top + 62.0f);
        layout.busy_list =
            Rect(left + 18.0f, top + 66.0f, left + kWidth - 18.0f,
                 top + kHeight - 52.0f);
        constexpr float kButtonW = 92.0f;
        constexpr float kButtonH = 28.0f;
        layout.busy_release =
            Rect(left + kWidth - 16.0f - kButtonW,
                 top + kHeight - 14.0f - kButtonH, left + kWidth - 16.0f,
                 top + kHeight - 14.0f);
        layout.busy_cancel =
            Rect(layout.busy_release.left - 10.0f - 72.0f,
                 layout.busy_release.top, layout.busy_release.left - 10.0f,
                 layout.busy_release.bottom);

        const float list_height =
            layout.busy_list.bottom - layout.busy_list.top;
        const float item_count =
            model_.occupants ? static_cast<float>(model_.occupants->size()) : 0.0f;
        const float content = item_count * kDialogRowHeight + 8.0f;
        layout.busy_scroll_max = std::max(0.0f, content - list_height);
        busy_scroll_ = std::min(std::max(busy_scroll_, 0.0f),
                                layout.busy_scroll_max);
        busy_scroll_target_ = std::min(
            std::max(busy_scroll_target_, 0.0f), layout.busy_scroll_max);
    }

    // 权限确认弹层
    if (model_.dialog == DialogKind::Admin) {
        constexpr float kWidth = 320.0f;
        constexpr float kHeight = 170.0f;
        const float left = layout.panel.left +
                           (layout.panel.right - layout.panel.left - kWidth) *
                               0.5f;
        const float top = layout.panel.top +
                          (layout.panel.bottom - layout.panel.top - kHeight) *
                              0.5f;
        layout.admin_card = Rect(left, top, left + kWidth, top + kHeight);
        constexpr float kButtonW = 76.0f;
        constexpr float kButtonH = 28.0f;
        layout.admin_elevate =
            Rect(left + kWidth - 16.0f - kButtonW,
                 top + kHeight - 14.0f - kButtonH, left + kWidth - 16.0f,
                 top + kHeight - 14.0f);
        layout.admin_cancel =
            Rect(layout.admin_elevate.left - 10.0f - kButtonW,
                 layout.admin_elevate.top, layout.admin_elevate.left - 10.0f,
                 layout.admin_elevate.bottom);
    }

    layout_ = std::move(layout);

    if (hover_revalidate_) {
        hover_revalidate_ = false;
        if (!pointer_inside_ || !hwnd_ || !IsWindowVisible(hwnd_)) {
            SetHover(Hit{});
            pointer_inside_ = false;
        } else {
            POINT cursor = pointer_screen_;
            if (ScreenToClient(hwnd_, &cursor)) {
                SetHover(HitTest(static_cast<float>(cursor.x) / scale_,
                                 static_cast<float>(cursor.y) / scale_));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

void MainView::Paint(Canvas& canvas, float dpi, float window_alpha) const {
    ID2D1RenderTarget* target = canvas.target();
    if (!target || !fonts_) return;
    (void)window_alpha;

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(target->CreateSolidColorBrush(
            D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 1.0f}, &brush)) ||
        !brush) {
        return;
    }

    DrawShadow(target, brush, dpi);
    FillRound(target, brush, layout_.panel, kPanelRadius, palette_.panel);
    StrokeRound(target, brush, layout_.panel, kPanelRadius,
                palette_.panel_border, 1.0f);

    PaintHeader(target, brush);

    target->PushAxisAlignedClip(layout_.content_clip,
                                D2D1_ANTIALIAS_MODE_ALIASED);
    if (!model_.devices || model_.devices->empty()) {
        PaintEmptyState(target, brush);
    } else {
        for (std::size_t i = 0;
             i < model_.devices->size() && i < layout_.cards.size(); ++i) {
            PaintDeviceCard(target, brush, (*model_.devices)[i], i,
                            layout_.cards[i]);
        }
    }
    target->PopAxisAlignedClip();

    PaintScrollBar(target, brush);

    if (model_.dialog == DialogKind::ConfirmClose) {
        PaintConfirmDialog(target, brush);
    } else if (model_.dialog == DialogKind::Busy) {
        PaintBusyDialog(target, brush);
    } else if (model_.dialog == DialogKind::Admin) {
        PaintAdminDialog(target, brush);
    }

    PaintToast(target, brush);
    brush->Release();
}

void MainView::DrawShadow(ID2D1RenderTarget* target,
                          ID2D1SolidColorBrush* brush, float dpi) const {
    const int width = static_cast<int>(std::lround(layout_.window_w));
    const int height = static_cast<int>(std::lround(layout_.window_h));
    const D2D1_RECT_F window =
        Rect(0.0f, 0.0f, layout_.window_w, layout_.window_h);

    if (shadow_.bitmap && shadow_.owner == target && shadow_.width == width &&
        shadow_.height == height && shadow_.dpi == dpi &&
        shadow_.dark == palette_.dark) {
        target->DrawBitmap(shadow_.bitmap, window);
        return;
    }

    shadow_.Release();
    ID2D1BitmapRenderTarget* cache_target = nullptr;
    if (SUCCEEDED(target->CreateCompatibleRenderTarget(&cache_target)) &&
        cache_target) {
        ID2D1SolidColorBrush* cache_brush = nullptr;
        cache_target->CreateSolidColorBrush(D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 1.0f},
                                            &cache_brush);
        if (cache_brush) {
            cache_target->BeginDraw();
            cache_target->Clear(D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 0.0f});
            DrawPanelShadow(cache_target, cache_brush, layout_.panel,
                            kPanelRadius, palette_.dark);
            if (SUCCEEDED(cache_target->EndDraw()) &&
                SUCCEEDED(cache_target->GetBitmap(&shadow_.bitmap))) {
                shadow_.owner = target;
                shadow_.width = width;
                shadow_.height = height;
                shadow_.dpi = dpi;
                shadow_.dark = palette_.dark;
            }
            cache_brush->Release();
        }
        cache_target->Release();
    }

    if (shadow_.bitmap) {
        target->DrawBitmap(shadow_.bitmap, window);
    } else {
        DrawPanelShadow(target, brush, layout_.panel, kPanelRadius,
                        palette_.dark);
    }
}

void MainView::PaintHeader(ID2D1RenderTarget* target,
                           ID2D1SolidColorBrush* brush) const {
    const D2D1_RECT_F& panel = layout_.panel;
    DrawText(target, brush, L"U 盘助手", fonts_->title,
             Rect(panel.left + 18.0f, panel.top + 10.0f, panel.right - 100.0f,
                  panel.top + 32.0f),
             palette_.text_primary);

    std::wstring subtitle = L"等待插入设备";
    if (model_.devices && !model_.devices->empty()) {
        std::size_t partitions = 0;
        for (const auto& device : *model_.devices) {
            partitions += device.partitions.size();
        }
        subtitle = L"已连接" + std::to_wstring(model_.devices->size()) +
                   L" 台设备 · " + std::to_wstring(partitions) + L" 个分区";
    }
    DrawText(target, brush, subtitle, fonts_->subtitle,
             Rect(panel.left + 18.0f, panel.top + 31.0f, panel.right - 100.0f,
                  panel.top + 47.0f),
             palette_.text_secondary);

    if (hover_close_ > 0.02f) {
        FillRound(target, brush, layout_.close_button, 13.0f,
                  WithAlpha(palette_.danger, hover_close_));
    }
    D2D1_COLOR_F close_color = palette_.text_secondary;
    close_color.r += (1.0f - close_color.r) * hover_close_;
    close_color.g += (1.0f - close_color.g) * hover_close_;
    close_color.b += (1.0f - close_color.b) * hover_close_;
    close_color.a = 0.55f + 0.45f * hover_close_;
    DrawCloseGlyph(target, brush, layout_.close_button, close_color, 9.0f);
}

void MainView::PaintDeviceCard(ID2D1RenderTarget* target,
                               ID2D1SolidColorBrush* brush,
                               const Device& device, std::size_t index,
                               const CardLayout& card) const {
    FillRound(target, brush, card.card, 12.0f, palette_.card);
    StrokeRound(target, brush, card.card, 12.0f, palette_.card_border, 1.0f);

    const D2D1_RECT_F tile = Rect(card.card.left + 14.0f,
                                  card.card.top + 14.0f,
                                  card.card.left + 50.0f,
                                  card.card.top + 50.0f);
    FillRound(target, brush, tile, 10.0f, palette_.icon_tile);
    const float icon_x = (tile.left + tile.right) * 0.5f;
    const float icon_y = (tile.top + tile.bottom) * 0.5f;
    if (device.removable_media) {
        DrawUsbGlyph(target, brush, icon_x, icon_y + 1.0f, 13.5f,
                     palette_.accent, 0.92f);
    } else if (device.solid_state) {
        DrawSsdGlyph(target, brush, icon_x, icon_y, 13.0f, palette_.accent,
                     0.92f);
    } else {
        DrawDriveGlyph(target, brush, icon_x, icon_y, 13.0f, palette_.accent,
                       0.92f);
    }

    DrawTextTrim(
        dwrite_, target, brush, device.title, fonts_->card_name,
        Rect(tile.right + 12.0f, card.card.top + 8.0f,
             card.eject_button.left - 10.0f, card.card.top + 31.0f),
        palette_.text_primary);

    std::wstring subtitle =
        std::to_wstring(device.partitions.size()) + L" 个分区";
    if (device.total > 0) {
        subtitle += L" · 共 " +
                    text::FormatBytes(device.total, model_.capacity_base);
    }
    DrawText(target, brush, subtitle, fonts_->meta,
             Rect(tile.right + 12.0f, card.card.top + 32.0f,
                  card.eject_button.left - 10.0f, card.card.top + 50.0f),
             palette_.text_secondary);

    const bool ejecting = model_.ejecting_disk == device.disk_number;
    // 有占用、正在等待用户解除时用危险态提示，避免与普通次要按钮混淆。
    const bool danger = !ejecting && model_.busy_disk == device.disk_number;
    const bool pressed = !ejecting && pressed_.kind == HitKind::Eject &&
                         pressed_.device == static_cast<int>(index);
    const float eject_hover = ejecting ? 0.0f : HoverEject(device.disk_number);

    const D2D1_RECT_F& slot = card.eject_button;
    const float center_y = (slot.top + slot.bottom) * 0.5f;
    const D2D1_RECT_F button =
        Rect(slot.left, center_y - 15.0f, slot.right, center_y + 15.0f);

    D2D1_COLOR_F button_fill = palette_.secondary_button;
    D2D1_COLOR_F border = WithAlpha(palette_.text_primary, 0.07f);
    D2D1_COLOR_F content = palette_.secondary_text;
    if (ejecting) {
        button_fill = WithAlpha(palette_.secondary_button, 0.5f);
        border = WithAlpha(palette_.text_primary, 0.04f);
        content = WithAlpha(palette_.text_secondary, 0.6f);
    } else if (danger) {
        button_fill = WithAlpha(palette_.danger, 0.10f + 0.08f * eject_hover);
        border = WithAlpha(palette_.danger, 0.30f + 0.12f * eject_hover);
        content = palette_.danger;
    } else {
        // 悬停填充改为两种不透明色之间的插值：终点仍是“强调色以 14%
        // 叠在卡片上”的原有观感，起点是基础按钮色。淡出时只会沿这条线
        // 单调回落，不会先透出白色卡片背景再跳回基础色。
        const float accent_amount = pressed ? 0.20f : 0.14f;
        const D2D1_COLOR_F hovered_fill =
            MixColor(palette_.card, palette_.accent, accent_amount);
        button_fill = MixColor(palette_.secondary_button, hovered_fill,
                               pressed ? 1.0f : eject_hover);
        border = WithAlpha(palette_.text_primary, 0.07f + 0.16f * eject_hover);
        content = MixColor(palette_.secondary_text, palette_.accent,
                           pressed ? 1.0f : eject_hover);
    }

    FillRound(target, brush, button, 8.0f, button_fill);
    StrokeRound(target, brush, button, 8.0f, border, 1.0f);

    if (ejecting) {
        DrawText(target, brush, L"正在弹出…", fonts_->button, button, content);
    } else {
        DrawEjectGlyph(target, brush, button.left + 16.0f, center_y - 1.0f,
                       11.0f, WithAlpha(content, 0.88f));
        DrawText(target, brush, L"安全弹出", fonts_->button,
                 Rect(button.left + 25.0f, button.top, button.right - 6.0f,
                      button.bottom),
                 content);
    }

    const float header_bottom =
        card.partitions.empty() ? card.card.bottom - 10.0f
                                : card.partitions.front().row.top;
    brush->SetColor(WithAlpha(palette_.text_primary, 0.06f));
    target->DrawLine({card.card.left + 14.0f, header_bottom},
                     {card.card.right - 14.0f, header_bottom}, brush, 1.0f);

    for (std::size_t i = 0; i < device.partitions.size() &&
                            i < card.partitions.size();
         ++i) {
        const Partition& part = device.partitions[i];
        const PartitionLayout& row = card.partitions[i];
        const float row_top = row.row.top;
        const float right = row.row.right - 14.0f;

        const bool managed = IsManagedMount(part.volume_path);
        const bool mountable = IsMountable(part);
        float actions_left = right;
        if (part.letter != 0) {
            actions_left = row.open_button.left;
            if (managed) actions_left = row.unmount_button.left;
        } else if (mountable) {
            actions_left = row.mount_button.left;
        }
        const float text_right =
            actions_left < right ? actions_left - 10.0f : right;

        if (i > 0) {
            brush->SetColor(WithAlpha(palette_.text_primary, 0.045f));
            target->DrawLine({row.row.left + 14.0f, row_top},
                             {row.row.right - 14.0f, row_top}, brush, 1.0f);
        }

        FillRound(target, brush, row.pill, 9.0f,
                  part.letter != 0 ? palette_.accent_soft
                                   : WithAlpha(palette_.track, 0.9f));
        std::wstring pill_text = L"—";
        D2D1_COLOR_F pill_color = palette_.text_tertiary;
        if (part.letter != 0) {
            pill_text = std::wstring(1, part.letter) + L":";
            pill_color = palette_.accent;
        } else if (part.kind == PartitionKind::Esp) {
            pill_text = L"ESP";
            pill_color = palette_.text_secondary;
        } else if (part.kind == PartitionKind::Msr) {
            pill_text = L"MSR";
            pill_color = palette_.text_secondary;
        }
        DrawText(target, brush, pill_text, fonts_->pill, row.pill, pill_color);

        DrawTextTrim(dwrite_, target, brush, part.label, fonts_->part_name,
                     Rect(row.pill.right + 10.0f, row_top + 5.0f, text_right,
                          row_top + 27.0f),
                     palette_.text_primary);

        const bool unknown_fs = IsUnknownFileSystem(part.filesystem);
        const std::uint64_t used =
            part.total > part.free_bytes ? part.total - part.free_bytes : 0;
        std::wstring meta;
        if (unknown_fs) {
            meta = part.filesystem + L" · 共 " +
                   text::FormatBytes(part.total, model_.capacity_base);
        } else {
            meta = part.filesystem + L" · 已用 " +
                   text::FormatBytes(used, model_.capacity_base) + L" · 共 " +
                   text::FormatBytes(part.total, model_.capacity_base);
        }
        const float meta_right =
            actions_left < right ? actions_left - 10.0f : right;
        DrawText(target, brush, meta, fonts_->meta,
                 Rect(row.row.left + 20.0f, row_top + 29.0f, meta_right,
                      row_top + 46.0f),
                 palette_.text_tertiary);

        if (!unknown_fs) {
            const double ratio =
                part.total == 0
                    ? 0.0
                    : static_cast<double>(used) /
                          static_cast<double>(part.total);
            const D2D1_RECT_F bar = Rect(row.row.left + 14.0f, row_top + 46.0f,
                                         right, row_top + 51.0f);
            FillRound(target, brush, bar, 2.5f, palette_.track);
            if (ratio > 0.0) {
                const float width = std::max(
                    static_cast<float>((bar.right - bar.left) * ratio), 6.0f);
                const D2D1_RECT_F fill =
                    Rect(bar.left, bar.top, bar.left + width, bar.bottom);
                const D2D1_COLOR_F color =
                    ratio > 0.90   ? palette_.danger
                    : ratio > 0.80 ? palette_.warn
                                   : palette_.accent;
                FillRound(target, brush, fill, 2.5f, color);
            }
        }

        if (part.letter != 0) {
            FillRound(target, brush, row.open_button, 8.0f, palette_.accent);
            const float hover = HoverOpen(part.letter);
            if (hover > 0.02f) {
                FillRound(target, brush, row.open_button, 8.0f,
                          D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 0.12f * hover});
            }
            DrawText(target, brush, L"打开", fonts_->button, row.open_button,
                     palette_.on_accent);

            if (managed) {
                const bool unmount_pressed =
                    pressed_.kind == HitKind::Unmount &&
                    pressed_.device == static_cast<int>(index) &&
                    pressed_.partition == static_cast<int>(i);
                const float volume_hover = HoverVolume(part.volume_path);
                FillRound(target, brush, row.unmount_button, 8.0f,
                          palette_.secondary_button);
                const float tint =
                    unmount_pressed ? 0.18f : 0.12f * volume_hover;
                if (tint > 0.002f) {
                    FillRound(target, brush, row.unmount_button, 8.0f,
                              WithAlpha(palette_.accent, tint));
                }
                const D2D1_COLOR_F unmount_color =
                    MixColor(palette_.secondary_text, palette_.accent,
                             unmount_pressed ? 1.0f : volume_hover);
                DrawText(target, brush, L"取消挂载", fonts_->button,
                         row.unmount_button, unmount_color);
            }
        } else if (mountable) {
            const bool mount_pressed =
                pressed_.kind == HitKind::Mount &&
                pressed_.device == static_cast<int>(index) &&
                pressed_.partition == static_cast<int>(i);
            const float volume_hover = HoverVolume(part.volume_path);
            FillRound(target, brush, row.mount_button, 8.0f,
                      palette_.accent_soft);
            StrokeRound(target, brush, row.mount_button, 8.0f,
                        WithAlpha(palette_.accent, 0.24f), 1.0f);
            const float tint = mount_pressed ? 0.20f : 0.12f * volume_hover;
            if (tint > 0.002f) {
                FillRound(target, brush, row.mount_button, 8.0f,
                          WithAlpha(palette_.accent, tint));
            }
            DrawText(target, brush, L"挂载", fonts_->button, row.mount_button,
                     palette_.accent);
        }
    }
}

void MainView::PaintEmptyState(ID2D1RenderTarget* target,
                               ID2D1SolidColorBrush* brush) const {
    const float center_x =
        (layout_.content_clip.left + layout_.content_clip.right) * 0.5f;
    const float top = layout_.content_top + 8.0f;

    brush->SetColor(WithAlpha(palette_.text_primary, 0.045f));
    target->FillEllipse(D2D1_ELLIPSE{{center_x, top + 42.0f}, 42.0f, 42.0f},
                        brush);
    DrawUsbGlyph(target, brush, center_x, top + 44.0f, 21.0f,
                 WithAlpha(palette_.text_tertiary, 0.85f), 0.0f);

    DrawText(target, brush, L"未检测到 USB 存储设备", fonts_->empty_title,
             Rect(layout_.content_clip.left, top + 96.0f,
                  layout_.content_clip.right, top + 122.0f),
             palette_.text_primary);
    DrawText(target, brush, L"插入 U 盘或移动硬盘后将自动显示",
             fonts_->empty_hint,
             Rect(layout_.content_clip.left, top + 124.0f,
                  layout_.content_clip.right, top + 146.0f),
             palette_.text_secondary);
}

void MainView::PaintScrollBar(ID2D1RenderTarget* target,
                              ID2D1SolidColorBrush* brush) const {
    if (!layout_.scrolling || layout_.scroll_max <= 0.0f) return;

    const float grow = std::min(std::max(scroll_grow_, 0.0f), 1.0f);
    const float width = 3.0f + 5.0f * grow;
    const float center_x = layout_.panel.right - 7.0f;
    const float viewport =
        layout_.content_clip.bottom - layout_.content_clip.top;
    const float content = viewport + layout_.scroll_max;
    const D2D1_RECT_F track =
        Rect(center_x - width * 0.5f, layout_.content_clip.top + 4.0f,
             center_x + width * 0.5f, layout_.content_clip.bottom - 4.0f);
    FillRound(target, brush, track, width * 0.5f,
              WithAlpha(palette_.text_primary, 0.10f + 0.06f * grow));

    const float track_height = track.bottom - track.top;
    const float thumb_height =
        std::max(28.0f, track_height * viewport / content);
    float progress =
        layout_.scroll_max > 0.0f ? scroll_y_ / layout_.scroll_max : 0.0f;
    progress = std::min(std::max(progress, 0.0f), 1.0f);
    const float thumb_top =
        track.top + (track_height - thumb_height) * progress;
    const D2D1_RECT_F thumb =
        Rect(track.left, thumb_top, track.right, thumb_top + thumb_height);
    FillRound(target, brush, thumb, width * 0.5f,
              WithAlpha(palette_.text_primary, 0.32f + 0.22f * grow));
}

void MainView::PaintToast(ID2D1RenderTarget* target,
                          ID2D1SolidColorBrush* brush) const {
    if (toast_opacity_ <= 0.02f || toast_text_.empty()) return;

    const float left = layout_.panel.left + 14.0f;
    const float right = layout_.panel.right - 14.0f;
    const float text_left = left + 30.0f;
    const float text_right = right - 14.0f;

    float text_height = 32.0f;
    const float measured = MeasureWrappedHeight(
        dwrite_, fonts_->toast, toast_text_, text_right - text_left);
    if (measured > 0.0f) text_height = measured;
    const float max_height =
        layout_.panel.bottom - layout_.panel.top - 12.0f;
    const float height =
        std::min(std::max(46.0f, text_height + 16.0f), max_height);

    const D2D1_RECT_F rect =
        Rect(left, layout_.panel.bottom - 14.0f - height, right,
             layout_.panel.bottom - 14.0f);
    FillRound(target, brush, rect, 10.0f,
              WithAlpha(palette_.toast, 0.97f * toast_opacity_));
    StrokeRound(target, brush, rect, 10.0f,
                WithAlpha(palette_.toast_border, toast_opacity_), 1.0f);

    const D2D1_COLOR_F dot_color =
        toast_ok_ ? palette_.success : palette_.danger;
    brush->SetColor(WithAlpha(dot_color, toast_opacity_));
    target->FillEllipse(
        D2D1_ELLIPSE{{rect.left + 16.0f, (rect.top + rect.bottom) * 0.5f},
                     3.5f,
                     3.5f},
        brush);

    DrawText(target, brush, toast_text_, fonts_->toast,
             Rect(rect.left + 30.0f, rect.top + 8.0f, rect.right - 14.0f,
                  rect.bottom - 8.0f),
             WithAlpha(palette_.on_toast, toast_opacity_));
}

void MainView::PaintConfirmDialog(ID2D1RenderTarget* target,
                                  ID2D1SolidColorBrush* brush) const {
    D2D1_COLOR_F overlay = palette_.panel;
    overlay.a = 0.62f;
    FillRound(target, brush, layout_.panel, kPanelRadius, overlay);

    const D2D1_RECT_F& card = layout_.confirm_card;
    FillRound(target, brush, card, 14.0f, palette_.card);
    StrokeRound(target, brush, card, 14.0f, palette_.panel_border, 1.0f);

    DrawText(target, brush, L"提示", fonts_->title,
             Rect(card.left + 18.0f, card.top + 14.0f, card.right - 18.0f,
                  card.top + 38.0f),
             palette_.text_primary);
    DrawText(target, brush,
             L"您是否想要退出本程序？您可在设置中修改关闭按钮的功能。",
             fonts_->toast,
             Rect(card.left + 18.0f, card.top + 42.0f, card.right - 18.0f,
                  card.top + 86.0f),
             palette_.text_secondary);

    DrawCheckBox(target, brush, layout_.confirm_check,
                 model_.confirm_dont_ask, palette_.accent,
                 WithAlpha(palette_.text_tertiary, 0.9f), palette_.on_accent);
    DrawText(target, brush, L"不再提示", fonts_->meta,
             Rect(layout_.confirm_check.right + 8.0f,
                  layout_.confirm_check.top - 3.0f,
                  layout_.confirm_check.right + 100.0f,
                  layout_.confirm_check.bottom + 3.0f),
             palette_.text_primary);

    FillRound(target, brush, layout_.confirm_cancel, 8.0f,
              palette_.secondary_button);
    DrawText(target, brush, L"取消", fonts_->button, layout_.confirm_cancel,
             palette_.secondary_text);

    FillRound(target, brush, layout_.confirm_ok, 8.0f, palette_.danger);
    DrawText(target, brush, L"退出", fonts_->button, layout_.confirm_ok,
             D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 1.0f});
}

void MainView::PaintBusyDialog(ID2D1RenderTarget* target,
                               ID2D1SolidColorBrush* brush) const {
    D2D1_COLOR_F overlay = palette_.panel;
    overlay.a = 0.62f;
    FillRound(target, brush, layout_.panel, kPanelRadius, overlay);

    const D2D1_RECT_F& card = layout_.busy_card;
    FillRound(target, brush, card, 14.0f, palette_.card);
    StrokeRound(target, brush, card, 14.0f, palette_.panel_border, 1.0f);

    const std::wstring title =
        L"「" + model_.busy_title + L"」正在被占用，是否要解除占用并弹出？";
    const float title_width =
        layout_.busy_title.right - layout_.busy_title.left;
    const float title_height = MeasureWrappedHeight(
        dwrite_, fonts_->dialog_title, title, title_width);
    if (title_height > 0.0f) {
        DrawText(target, brush, title, fonts_->dialog_title,
                 Rect(layout_.busy_title.left, layout_.busy_title.top,
                      layout_.busy_title.right,
                      layout_.busy_title.top + title_height + 4.0f),
                 palette_.text_primary);
    }

    const D2D1_RECT_F& list = layout_.busy_list;
    FillRound(target, brush, list, 8.0f,
              WithAlpha(palette_.text_primary, palette_.dark ? 0.06f : 0.04f));
    StrokeRound(target, brush, list, 8.0f,
                WithAlpha(palette_.card_border, 0.9f), 1.0f);

    const bool has_occupants =
        model_.occupants && !model_.occupants->empty();
    const bool show_bar = layout_.busy_scroll_max > 0.5f;
    const D2D1_RECT_F rows =
        Rect(list.left + 6.0f, list.top + 4.0f,
             list.right - (show_bar ? 14.0f : 6.0f), list.bottom - 4.0f);

    if (!has_occupants) {
        const wchar_t* hint = L"正在查找占用进程…";
        if (model_.busy_releasing) {
            hint = L"正在结束占用进程…";
        } else if (!model_.busy_scanning) {
            hint = L"未找到占用进程，可能由系统服务或驱动占用。";
        }
        DrawText(target, brush, hint, fonts_->empty_hint,
                 Rect(list.left + 10.0f, list.top, list.right - 10.0f,
                      list.bottom),
                 palette_.text_tertiary);
    } else {
        target->PushAxisAlignedClip(rows, D2D1_ANTIALIAS_MODE_ALIASED);
        for (std::size_t i = 0; i < model_.occupants->size(); ++i) {
            const float top =
                rows.top - busy_scroll_ + kDialogRowHeight * static_cast<float>(i);
            if (top > rows.bottom || top + kDialogRowHeight < rows.top) continue;

            const Occupant& occupant = (*model_.occupants)[i];
            const float center_y = top + kDialogRowHeight * 0.5f;
            const D2D1_RECT_F icon_rect =
                Rect(rows.left + 4.0f, center_y - 10.0f, rows.left + 24.0f,
                     center_y + 10.0f);
            ID2D1Bitmap* bitmap = nullptr;
            if (occupant.icon &&
                CreateBitmapFromIcon(target, occupant.icon, &bitmap) &&
                bitmap) {
                target->DrawBitmap(bitmap, icon_rect, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                bitmap->Release();
            } else {
                FillRound(target, brush, icon_rect, 4.0f,
                          WithAlpha(palette_.text_primary, 0.10f));
            }
            DrawTextTrim(dwrite_, target, brush, occupant.name,
                         fonts_->dialog_row,
                         Rect(rows.left + 32.0f, top, rows.right - 6.0f,
                              top + kDialogRowHeight),
                         palette_.text_primary);
        }
        target->PopAxisAlignedClip();
    }

    if (show_bar) {
        const float x = list.right - 9.0f;
        const D2D1_RECT_F track =
            Rect(x, list.top + 8.0f, x + 3.0f, list.bottom - 8.0f);
        FillRound(target, brush, track, 1.5f,
                  WithAlpha(palette_.text_primary, 0.10f));
        const float track_height = track.bottom - track.top;
        const float viewport = rows.bottom - rows.top;
        const float content = viewport + layout_.busy_scroll_max;
        const float thumb_height =
            std::max(24.0f, track_height * viewport / std::max(content, 1.0f));
        float progress = layout_.busy_scroll_max > 0.0f
                             ? busy_scroll_ / layout_.busy_scroll_max
                             : 0.0f;
        progress = std::min(std::max(progress, 0.0f), 1.0f);
        const float thumb_top =
            track.top + (track_height - thumb_height) * progress;
        FillRound(target, brush,
                  Rect(track.left, thumb_top, track.right,
                       thumb_top + thumb_height),
                  1.5f, WithAlpha(palette_.text_primary, 0.32f));
    }

    FillRound(target, brush, layout_.busy_cancel, 8.0f,
              model_.busy_releasing
                  ? WithAlpha(palette_.secondary_button, 0.5f)
                  : palette_.secondary_button);
    DrawText(target, brush, L"取消", fonts_->button, layout_.busy_cancel,
             model_.busy_releasing ? WithAlpha(palette_.secondary_text, 0.5f)
                                   : palette_.secondary_text);

    const bool busy_button_enabled =
        !model_.busy_scanning && !model_.busy_releasing;
    const wchar_t* release_text = L"解除占用";
    if (model_.busy_releasing) {
        release_text = L"正在解除…";
    } else if (!model_.busy_scanning && !has_occupants) {
        release_text = L"重试弹出";
    }
    FillRound(target, brush, layout_.busy_release, 8.0f,
              busy_button_enabled ? palette_.danger
                                  : WithAlpha(palette_.danger, 0.45f));
    DrawText(target, brush, release_text, fonts_->button,
             layout_.busy_release,
             busy_button_enabled ? D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 1.0f}
                                 : WithAlpha(palette_.on_accent, 0.7f));
}

void MainView::PaintAdminDialog(ID2D1RenderTarget* target,
                                ID2D1SolidColorBrush* brush) const {
    D2D1_COLOR_F overlay = palette_.panel;
    overlay.a = 0.62f;
    FillRound(target, brush, layout_.panel, kPanelRadius, overlay);

    const D2D1_RECT_F& card = layout_.admin_card;
    FillRound(target, brush, card, 14.0f, palette_.card);
    StrokeRound(target, brush, card, 14.0f, palette_.panel_border, 1.0f);

    DrawShieldGlyph(target, brush, card.left + 30.0f, card.top + 27.0f, 20.0f,
                    palette_.accent, palette_.on_accent);

    DrawText(target, brush, L"需要管理员权限", fonts_->title,
             Rect(card.left + 48.0f, card.top + 14.0f, card.right - 18.0f,
                  card.top + 40.0f),
             palette_.text_primary);

    DrawText(target, brush,
             L"当前操作需要管理员权限，是否以管理员模式运行？",
             fonts_->toast,
             Rect(card.left + 18.0f, card.top + 46.0f, card.right - 18.0f,
                  card.top + 92.0f),
             palette_.text_secondary);

    if (!model_.admin_detail.empty()) {
        DrawText(target, brush, model_.admin_detail, fonts_->meta,
                 Rect(card.left + 18.0f, card.top + 96.0f,
                      card.right - 18.0f, card.top + 118.0f),
                 palette_.text_tertiary);
    }

    FillRound(target, brush, layout_.admin_cancel, 8.0f,
              palette_.secondary_button);
    DrawText(target, brush, L"取消", fonts_->button, layout_.admin_cancel,
             palette_.secondary_text);

    FillRound(target, brush, layout_.admin_elevate, 8.0f, palette_.accent);
    DrawText(target, brush, L"确认", fonts_->button,
             layout_.admin_elevate, palette_.on_accent);
}

float MainView::ScrollThumbHeight() const {
    const float track_height =
        layout_.content_clip.bottom - layout_.content_clip.top;
    if (track_height <= 0.0f) return 0.0f;
    const float content = track_height + layout_.scroll_max;
    if (content <= 0.0f) return track_height;
    return std::max(28.0f, track_height * track_height / content);
}

D2D1_RECT_F MainView::ScrollThumbRect() const {
    const float track_height =
        layout_.content_clip.bottom - layout_.content_clip.top;
    const float thumb_height = ScrollThumbHeight();
    float progress =
        layout_.scroll_max > 0.0f ? scroll_y_ / layout_.scroll_max : 0.0f;
    progress = std::min(std::max(progress, 0.0f), 1.0f);
    const float top =
        layout_.content_clip.top + (track_height - thumb_height) * progress;
    return Rect(layout_.panel.right - 12.0f, top,
                layout_.panel.right - 2.0f, top + thumb_height);
}

// ---------------------------------------------------------------------------
// 命中测试
// ---------------------------------------------------------------------------

Hit MainView::HitTest(float x, float y) const {
    if (model_.dialog == DialogKind::Admin) {
        if (Contains(layout_.admin_elevate, x, y)) {
            return {HitKind::AdminElevate, -1, -1};
        }
        if (Contains(layout_.admin_cancel, x, y)) {
            return {HitKind::AdminCancel, -1, -1};
        }
        return {};
    }

    if (model_.dialog == DialogKind::Busy) {
        const bool enabled = !model_.busy_scanning && !model_.busy_releasing;
        if (enabled && Contains(layout_.busy_release, x, y)) {
            return {HitKind::BusyRelease, -1, -1};
        }
        if (!model_.busy_releasing && Contains(layout_.busy_cancel, x, y)) {
            return {HitKind::BusyCancel, -1, -1};
        }
        return {};
    }

    if (model_.dialog == DialogKind::ConfirmClose) {
        if (Contains(layout_.confirm_ok, x, y)) {
            return {HitKind::ConfirmOk, -1, -1};
        }
        if (Contains(layout_.confirm_cancel, x, y)) {
            return {HitKind::ConfirmCancel, -1, -1};
        }
        D2D1_RECT_F check_hit = layout_.confirm_check;
        check_hit.right += 90.0f;
        check_hit.top -= 4.0f;
        check_hit.bottom += 4.0f;
        if (Contains(check_hit, x, y)) {
            return {HitKind::ConfirmCheck, -1, -1};
        }
        return {};
    }

    if (Contains(layout_.close_button, x, y)) {
        return {HitKind::Close, -1, -1};
    }
    if (layout_.scrolling && Contains(layout_.scroll_bar, x, y)) {
        return {HitKind::ScrollBar, -1, -1};
    }
    if (!Contains(layout_.content_clip, x, y)) return {};

    if (model_.devices) {
        for (std::size_t i = 0;
             i < layout_.cards.size() && i < model_.devices->size(); ++i) {
            const CardLayout& card = layout_.cards[i];
            const Device& device = (*model_.devices)[i];
            if (model_.ejecting_disk != device.disk_number &&
                Contains(card.eject_button, x, y)) {
                return {HitKind::Eject, static_cast<int>(i), -1};
            }
            for (std::size_t j = 0;
                 j < card.partitions.size() && j < device.partitions.size();
                 ++j) {
                const Partition& part = device.partitions[j];
                const PartitionLayout& row = card.partitions[j];
                if (part.letter != 0) {
                    if (IsManagedMount(part.volume_path) &&
                        Contains(row.unmount_button, x, y)) {
                        return {HitKind::Unmount, static_cast<int>(i),
                                static_cast<int>(j)};
                    }
                    if (Contains(row.open_button, x, y)) {
                        return {HitKind::Open, static_cast<int>(i),
                                static_cast<int>(j)};
                    }
                } else if (IsMountable(part) &&
                           Contains(row.mount_button, x, y)) {
                    return {HitKind::Mount, static_cast<int>(i),
                            static_cast<int>(j)};
                }
            }
        }
    }
    return {};
}

bool MainView::OverInteractive(float x, float y) const {
    return HitTest(x, y).kind != HitKind::None;
}

bool MainView::OverPanel(float x, float y) const {
    return ContainsRounded(layout_.panel, kPanelRadius, x, y);
}

// ---------------------------------------------------------------------------
// 鼠标交互
// ---------------------------------------------------------------------------

ViewAction MainView::OnMouseDown(float x, float y) {
    ViewAction action;
    if (dragging_window_ || scroll_dragging_) return action;

    const Hit hit = HitTest(x, y);
    if (hit.kind == HitKind::ScrollBar) {
        scroll_dragging_ = true;
        pressed_ = hit;
        const D2D1_RECT_F thumb = ScrollThumbRect();
        if (y < thumb.top || y > thumb.bottom) {
            const float track_height =
                layout_.content_clip.bottom - layout_.content_clip.top;
            const float thumb_height = ScrollThumbHeight();
            const float travel = std::max(1.0f, track_height - thumb_height);
            float progress =
                (y - layout_.content_clip.top - thumb_height * 0.5f) / travel;
            progress = std::min(std::max(progress, 0.0f), 1.0f);
            SetScrollTarget(progress * layout_.scroll_max);
        }
        scroll_drag_start_y_ = y;
        scroll_drag_start_target_ = scroll_target_;
        if (hwnd_) SetCapture(hwnd_);
        return action;
    }

    if (hit.kind != HitKind::None) {
        pressed_ = hit;
        if (hwnd_) SetCapture(hwnd_);
        return action;
    }

    if (model_.dialog == DialogKind::None && hwnd_ &&
        y >= layout_.panel.top && y <= layout_.panel.top + kHeaderHeight &&
        x >= layout_.panel.left && x <= layout_.panel.right) {
        dragging_window_ = true;
        SetCapture(hwnd_);
        GetCursorPos(&drag_cursor_start_);
        GetWindowRect(hwnd_, &drag_window_start_);
    }
    return action;
}

ViewAction MainView::OnMouseMove(float x, float y) {
    ViewAction action;
    if (hwnd_) {
        POINT cursor = {static_cast<LONG>(std::lround(x * scale_)),
                        static_cast<LONG>(std::lround(y * scale_))};
        if (ClientToScreen(hwnd_, &cursor)) pointer_screen_ = cursor;
    }
    pointer_inside_ = true;
    if (scroll_dragging_) {
        const float track_height =
            layout_.content_clip.bottom - layout_.content_clip.top;
        const float thumb_height = ScrollThumbHeight();
        const float travel = std::max(1.0f, track_height - thumb_height);
        const float ratio =
            travel > 0.0f ? layout_.scroll_max / travel : 0.0f;
        SetScrollTarget(scroll_drag_start_target_ +
                        (y - scroll_drag_start_y_) * ratio);
        return action;
    }

    if (dragging_window_) {
        POINT cursor = {};
        GetCursorPos(&cursor);
        const int new_x =
            drag_window_start_.left + (cursor.x - drag_cursor_start_.x);
        const int new_y =
            drag_window_start_.top + (cursor.y - drag_cursor_start_.y);
        SetWindowPos(hwnd_, nullptr, new_x, new_y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return action;
    }

    const bool over_bar =
        layout_.scrolling && Contains(layout_.scroll_bar, x, y);
    if (over_bar != scroll_hover_) scroll_hover_ = over_bar;
    SetHover(HitTest(x, y));
    return action;
}

ViewAction MainView::OnMouseUp(float x, float y) {
    ViewAction action;
    if (scroll_dragging_) {
        scroll_dragging_ = false;
        pressed_ = Hit{};
        if (hwnd_ && GetCapture() == hwnd_) ReleaseCapture();
        return action;
    }
    if (dragging_window_) {
        dragging_window_ = false;
        if (hwnd_ && GetCapture() == hwnd_) ReleaseCapture();
        action.kind = ActionKind::DragEnded;
        return action;
    }

    const Hit hit = HitTest(x, y);
    if (hit.kind == pressed_.kind && hit.device == pressed_.device &&
        hit.partition == pressed_.partition) {
        switch (hit.kind) {
            case HitKind::Close:
                action.kind = ActionKind::Close;
                break;
            case HitKind::ConfirmOk:
                action.kind = ActionKind::ConfirmExit;
                break;
            case HitKind::ConfirmCancel:
            case HitKind::BusyCancel:
            case HitKind::AdminCancel:
                action.kind = ActionKind::CancelDialog;
                break;
            case HitKind::ConfirmCheck:
                action.kind = ActionKind::ToggleDontAsk;
                break;
            case HitKind::BusyRelease:
                action.kind = ActionKind::ReleaseOccupants;
                break;
            case HitKind::AdminElevate:
                action.kind = ActionKind::Elevate;
                break;
            case HitKind::Open:
                action.kind = ActionKind::Open;
                action.device = static_cast<std::size_t>(hit.device);
                action.partition = static_cast<std::size_t>(hit.partition);
                break;
            case HitKind::Mount:
                action.kind = ActionKind::Mount;
                action.device = static_cast<std::size_t>(hit.device);
                action.partition = static_cast<std::size_t>(hit.partition);
                break;
            case HitKind::Unmount:
                action.kind = ActionKind::Unmount;
                action.device = static_cast<std::size_t>(hit.device);
                action.partition = static_cast<std::size_t>(hit.partition);
                break;
            case HitKind::Eject:
                action.kind = ActionKind::Eject;
                action.device = static_cast<std::size_t>(hit.device);
                break;
            default:
                break;
        }
    }
    pressed_ = Hit{};
    if (hwnd_ && GetCapture() == hwnd_) ReleaseCapture();
    return action;
}

void MainView::OnMouseLeave() {
    pointer_inside_ = false;
    scroll_hover_ = false;
    SetHover(Hit{});
}

bool MainView::OnWheel(float delta) {
    if (model_.dialog == DialogKind::Busy) {
        busy_scroll_target_ -= delta * 40.0f;
        busy_scroll_target_ =
            std::min(std::max(busy_scroll_target_, 0.0f),
                     layout_.busy_scroll_max);
        return layout_.busy_scroll_max > 0.0f;
    }
    if (!layout_.scrolling) return false;
    SetScrollTarget(scroll_target_ - delta * 56.0f);
    return true;
}

void MainView::CancelInteraction() {
    if (hwnd_ && GetCapture() == hwnd_) ReleaseCapture();
    dragging_window_ = false;
    scroll_dragging_ = false;
    pressed_ = Hit{};
}

// ---------------------------------------------------------------------------
// 动画与提示
// ---------------------------------------------------------------------------

bool MainView::Tick() {
    bool more = false;

    more |= Ease(hover_close_, hover_close_target_, 0.26f, 0.008f);
    more |= Ease(scroll_y_, scroll_target_, 0.22f, 0.35f);
    more |= Ease(busy_scroll_, busy_scroll_target_, 0.22f, 0.35f);
    more |= Ease(toast_opacity_, toast_target_, 0.22f, 0.01f);
    more |= Ease(scroll_grow_,
                 (scroll_hover_ || scroll_dragging_) ? 1.0f : 0.0f, 0.30f,
                 0.008f);

    const auto* devices = model_.devices.get();
    for (auto it = hover_eject_.begin(); it != hover_eject_.end();) {
        float target = 0.0f;
        if (devices && hovered_.kind == HitKind::Eject &&
            hovered_.device >= 0 &&
            static_cast<std::size_t>(hovered_.device) < devices->size() &&
            (*devices)[static_cast<std::size_t>(hovered_.device)].disk_number ==
                it->first) {
            target = 1.0f;
        }
        more |= Ease(it->second, target, 0.26f, 0.008f);
        if (it->second <= 0.0f && target == 0.0f) {
            it = hover_eject_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = hover_open_.begin(); it != hover_open_.end();) {
        float target = 0.0f;
        if (devices && hovered_.kind == HitKind::Open &&
            hovered_.device >= 0 && hovered_.partition >= 0 &&
            static_cast<std::size_t>(hovered_.device) < devices->size()) {
            const Device& device =
                (*devices)[static_cast<std::size_t>(hovered_.device)];
            if (static_cast<std::size_t>(hovered_.partition) <
                    device.partitions.size() &&
                device.partitions[static_cast<std::size_t>(
                    hovered_.partition)]
                        .letter == it->first) {
                target = 1.0f;
            }
        }
        more |= Ease(it->second, target, 0.26f, 0.008f);
        if (it->second <= 0.0f && target == 0.0f) {
            it = hover_open_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = hover_volume_.begin(); it != hover_volume_.end();) {
        float target = 0.0f;
        if (devices &&
            (hovered_.kind == HitKind::Mount ||
             hovered_.kind == HitKind::Unmount) &&
            hovered_.device >= 0 && hovered_.partition >= 0 &&
            static_cast<std::size_t>(hovered_.device) < devices->size()) {
            const Device& device =
                (*devices)[static_cast<std::size_t>(hovered_.device)];
            if (static_cast<std::size_t>(hovered_.partition) <
                    device.partitions.size() &&
                device.partitions[static_cast<std::size_t>(
                    hovered_.partition)]
                        .volume_path == it->first) {
                target = 1.0f;
            }
        }
        more |= Ease(it->second, target, 0.26f, 0.008f);
        if (it->second <= 0.0f && target == 0.0f) {
            it = hover_volume_.erase(it);
        } else {
            ++it;
        }
    }

    // 本帧已把新登记的悬停目标推进了一次，后续靠动画值本身判断即可。
    hover_anim_pending_ = false;
    return more;
}

bool MainView::Animating() const {
    if (hover_anim_pending_) return true;
    if (std::fabs(hover_close_ - hover_close_target_) > 0.008f) return true;
    if (std::fabs(scroll_y_ - scroll_target_) > 0.35f) return true;
    if (std::fabs(busy_scroll_ - busy_scroll_target_) > 0.35f) return true;
    if (std::fabs(toast_opacity_ - toast_target_) > 0.01f) return true;
    const float scroll_goal = (scroll_hover_ || scroll_dragging_) ? 1.0f : 0.0f;
    if (std::fabs(scroll_grow_ - scroll_goal) > 0.008f) return true;
    for (const auto& item : hover_eject_) {
        if (item.second > 0.008f) return true;
    }
    for (const auto& item : hover_open_) {
        if (item.second > 0.008f) return true;
    }
    for (const auto& item : hover_volume_) {
        if (item.second > 0.008f) return true;
    }
    return false;
}

void MainView::ShowToast(std::wstring message, bool ok) {
    toast_text_ = std::move(message);
    toast_ok_ = ok;
    toast_target_ = 1.0f;
    if (toast_opacity_ < 0.05f) toast_opacity_ = 0.05f;
}

void MainView::HideToast() {
    toast_target_ = 0.0f;
}

void MainView::ResetHover() {
    hovered_ = Hit{};
    hover_close_target_ = 0.0f;
    scroll_hover_ = false;
}

void MainView::SetScrollTarget(float value) {
    scroll_target_ = std::min(std::max(value, 0.0f), layout_.scroll_max);
}

void MainView::SetHover(const Hit& hit) {
    if (hit.kind == hovered_.kind && hit.device == hovered_.device &&
        hit.partition == hovered_.partition) {
        return;
    }
    hovered_ = hit;
    hover_close_target_ = hit.kind == HitKind::Close ? 1.0f : 0.0f;
    if (hit.kind == HitKind::Eject && model_.devices &&
        hit.device >= 0 &&
        static_cast<std::size_t>(hit.device) < model_.devices->size()) {
        hover_eject_[(*model_.devices)[static_cast<std::size_t>(hit.device)]
                         .disk_number] = 0.0f;
    }
    if (hit.kind == HitKind::Open && model_.devices && hit.device >= 0 &&
        hit.partition >= 0 &&
        static_cast<std::size_t>(hit.device) < model_.devices->size()) {
        const Device& device =
            (*model_.devices)[static_cast<std::size_t>(hit.device)];
        if (static_cast<std::size_t>(hit.partition) <
            device.partitions.size()) {
            const wchar_t letter =
                device.partitions[static_cast<std::size_t>(hit.partition)]
                    .letter;
            if (letter != 0) hover_open_[letter] = 0.0f;
        }
    }
    if ((hit.kind == HitKind::Mount || hit.kind == HitKind::Unmount) &&
        model_.devices && hit.device >= 0 && hit.partition >= 0 &&
        static_cast<std::size_t>(hit.device) < model_.devices->size()) {
        const Device& device =
            (*model_.devices)[static_cast<std::size_t>(hit.device)];
        if (static_cast<std::size_t>(hit.partition) <
            device.partitions.size()) {
            const std::wstring& path =
                device.partitions[static_cast<std::size_t>(hit.partition)]
                    .volume_path;
            if (!path.empty()) hover_volume_[path] = 0.0f;
        }
    }
    // 悬停值刚被置为 0，真正的缓动在 Tick 中；先置位，保证外层据此
    // 启动动画计时器，否则首帧不会推进，按钮也就不会变色。
    hover_anim_pending_ =
        hit.kind == HitKind::Close || hit.kind == HitKind::Eject ||
        hit.kind == HitKind::Open || hit.kind == HitKind::Mount ||
        hit.kind == HitKind::Unmount;
}

float MainView::HoverEject(std::uint32_t disk_number) const {
    const auto it = hover_eject_.find(disk_number);
    return it == hover_eject_.end() ? 0.0f : it->second;
}

float MainView::HoverOpen(wchar_t letter) const {
    const auto it = hover_open_.find(letter);
    return it == hover_open_.end() ? 0.0f : it->second;
}

float MainView::HoverVolume(const std::wstring& volume_path) const {
    const auto it = hover_volume_.find(volume_path);
    return it == hover_volume_.end() ? 0.0f : it->second;
}

bool MainView::IsManagedMount(const std::wstring& volume_path) const {
    if (volume_path.empty()) return false;
    return std::find(model_.mounted_volumes.begin(),
                     model_.mounted_volumes.end(),
                     volume_path) != model_.mounted_volumes.end();
}

bool MainView::IsMountable(const Partition& part) {
    if (part.letter != 0 || part.kind == PartitionKind::Msr ||
        part.volume_path.empty()) {
        return false;
    }
    // ESP 的卷信息偶尔读不到文件系统，但只要卷存在就允许尝试分配盘符。
    if (part.kind == PartitionKind::Esp) return true;
    return text::IsOpenableFileSystem(part.filesystem);
}

}  // namespace usb::ui
