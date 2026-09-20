#!/usr/bin/env python3
"""
screen_preview.py — Simulate 400x300 1bpp e-ink screen after all bug fixes.

Generates 3 PNG previews:
1. preview_chat_page.png  — Chat with StatusBar + Bubbles (line-by-line >=24px) + Bottom bar
2. preview_settings_page.png — Settings flat list + Toggle + About Action
3. preview_settings_about_dialog.png — Settings + About dialog (centered, real MAC)
"""

import os
from PIL import Image, ImageDraw, ImageFont

# Style constants
kScreenWidth = 400
kScreenHeight = 300
kStatusBarHeight = 24
kStatusBarPadding = 4
kSpacingXXS = 2
kSpacingXS = 4
kSpacingSM = 6
kSpacingMD = 8
kSpacingLG = 12
kSpacingXL = 16
kSpacingXXL = 24
kCornerSafeInset = 15
kBorderRadiusSM = 4
kBorderRadiusMD = 8
kBorderRadiusLG = 12
kBorderThin = 1
kBorderMedium = 2
kItemPadding = kSpacingMD
kItemMinHeight = 36
kIconSize = 16
kBottomBarH = 18
kBubblePadding = 12
kBubbleMargin = 24
kBubbleLineSpacing = 6
kClockX = 320
kClockY = 4

def get_font(size):
    font_paths = [
        '/System/Library/Fonts/PingFang.ttc',
        '/System/Library/Fonts/STHeiti Light.ttc',
    ]
    for fp in font_paths:
        if os.path.exists(fp):
            return ImageFont.truetype(fp, size)
    return ImageFont.load_default()

font_regular = get_font(16)
font_medium = get_font(24)
font_icon = get_font(16)

def measure_text_width(text, font):
    bbox = font.getbbox(text)
    return bbox[2] - bbox[0]

def get_line_height(font):
    bbox = font.getbbox("Ay")
    return bbox[3] - bbox[1]

def draw_rect(draw, x, y, w, h, color='white'):
    draw.rectangle([x, y, x + w, y + h], fill=color)

def draw_hline(draw, y, x1, x2, color='black'):
    draw.line([(x1, y), (x2, y)], fill=color)

def draw_round_rect(draw, x, y, w, h, radius, fill='white', outline='black', border_w=1):
    """Draw a true rounded rectangle using PIL's rounded_rectangle."""
    # Clamp radius to half the smaller dimension
    r = min(radius, w // 2, h // 2)
    bbox = [x, y, x + w, y + h]
    if border_w > 0:
        draw.rounded_rectangle(bbox, radius=r, fill=fill, outline=outline, width=border_w)
    else:
        draw.rounded_rectangle(bbox, radius=r, fill=fill)

def draw_circle(draw, cx, cy, r, color='black'):
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=color)

def draw_text(draw, x, y, text, font, color='black'):
    draw.text((x, y), text, font=font, fill=color)

def draw_chevron(draw, x, y, size, color='black'):
    mid_y = y + size // 2
    draw.line([(x, y), (x + size, mid_y)], fill=color)
    draw.line([(x + size, mid_y), (x, y + size - 1)], fill=color)

# ============================================================
# Status bar (WiFi signal bars + server dot + title + battery icon + text)
# ============================================================
def draw_status_bar(draw, title="AI对话", wifi=True, server=True, battery=85):
    draw_rect(draw, 0, 0, kScreenWidth, kStatusBarHeight, 255)
    draw_hline(draw, kStatusBarHeight - 1, 0, kScreenWidth - 1, 0)

    y = (kStatusBarHeight - get_line_height(font_regular)) // 2

    # --- Left: WiFi signal bars (4 vertical bars, increasing height) ---
    x = kStatusBarPadding + kCornerSafeInset  # 19
    bar_w = 3
    bar_gap = 1
    bar_heights = [4, 6, 9, 12]  # signal level heights
    if wifi:
        # All 4 bars filled (connected)
        for i, bh in enumerate(bar_heights):
            bx = x + i * (bar_w + bar_gap)
            by = y + get_line_height(font_regular) - bh
            draw_rect(draw, bx, by, bar_w, bh, 0)
    else:
        # Only 1 bar filled + slash
        bx = x
        by = y + get_line_height(font_regular) - bar_heights[0]
        draw_rect(draw, bx, by, bar_w, bar_heights[0], 0)
        # Slash line through bars
        draw.line([(x, y + 2), (x + 4 * (bar_w + bar_gap), y + get_line_height(font_regular) - 2)], fill=0)

    # Server dot next to WiFi
    x += 4 * (bar_w + bar_gap) + kSpacingSM  # ~35
    dot_cy = y + get_line_height(font_regular) // 2
    if server:
        draw_circle(draw, x + 4, dot_cy, 3, 0)
    elif wifi:
        draw.ellipse([x + 1, dot_cy - 3, x + 7, dot_cy + 3], outline=0)

    # --- Center: Page title ---
    title_w = measure_text_width(title, font_regular)
    title_x = (kScreenWidth - title_w) // 2
    draw_text(draw, title_x, y, title, font_regular, 0)

    # --- Right: Battery icon (pixel-drawn) + percentage ---
    # Place battery to the LEFT of clock zone (clock starts at x=320)
    bat_text = f"{battery}%"
    bat_text_w = measure_text_width(bat_text, font_regular)
    # Battery shape: outline 16x10 with 2px nub
    bat_w = 16
    bat_h = 10
    bat_icon_x = kClockX - bat_text_w - bat_w - kSpacingXS  # Place left of clock
    bat_icon_y = y + (get_line_height(font_regular) - bat_h) // 2
    # Body outline
    draw.rectangle([bat_icon_x, bat_icon_y, bat_icon_x + bat_w - 3, bat_icon_y + bat_h], outline=0, width=1)
    # Nub (positive terminal)
    draw.rectangle([bat_icon_x + bat_w - 3, bat_icon_y + 2, bat_icon_x + bat_w, bat_icon_y + bat_h - 2], outline=0, width=1)
    # Fill segments (3 cells)
    cell_w = (bat_w - 4) // 3
    level_cells = 3 if battery >= 75 else (2 if battery >= 50 else (1 if battery > 0 else 0))
    for cell in range(level_cells):
        cx = bat_icon_x + 1 + cell * cell_w
        draw_rect(draw, cx, bat_icon_y + 1, cell_w - 1, bat_h - 2, 0)

    # Battery percentage text
    bat_text_x = bat_icon_x + bat_w + kSpacingXS
    draw_text(draw, bat_text_x, y, bat_text, font_regular, 0)

# ============================================================
# Clock overlay (drawn AFTER page content, at x=320 y=28)
# ============================================================
def draw_clock_overlay(draw):
    """Draw clock at top-right, overlaying page content (matches C++ RenderAll order)."""
    # Clear clock region first (matches DrawWithClear)
    draw_rect(draw, kClockX, kClockY, kScreenWidth - kClockX, 20, 255)
    draw_text(draw, kClockX, kClockY, "10:52", font_regular, 0)

# ============================================================
# Chat page (Bug 2 fix: bubble width accounts for scrollbar)
# ============================================================
def render_chat_page(draw):
    draw_rect(draw, 0, 0, kScreenWidth, kScreenHeight, 255)
    draw_status_bar(draw, title="AI对话", wifi=True, server=True, battery=85)

    # Content area
    content_y = kStatusBarHeight + kSpacingXXS  # 26
    line_step = max(get_line_height(font_regular) + kBubbleLineSpacing, 24)

    # AI bubble (left-aligned, white bg + black border)
    bubble_y = content_y + 4
    bubble_x = kBubbleMargin
    # Bug 2: account for scrollbar width in bubble layout
    content_width = kScreenWidth - 2 * kSpacingLG - 3 - 4  # scrollbar margin
    max_bubble_w = (content_width * 80) // 100

    # User bubble text
    user_text = "你好，请帮我写一个函数"
    ai_text = "好的，我来帮你写这个函数，请告诉我具体需求"

    # User bubble (right-aligned, black bg, rounded corners)
    # FIX: use kBubbleMargin=24 on right side for visible margin
    user_bubble_w = min(measure_text_width(user_text, font_regular) + kBubblePadding * 2, max_bubble_w)
    user_bubble_h = line_step + kBubblePadding * 2  # 1 line
    user_x = kScreenWidth - kBubbleMargin - user_bubble_w
    draw_round_rect(draw, user_x, bubble_y, user_bubble_w, user_bubble_h,
                  kBorderRadiusMD, fill=0, outline=0, border_w=0)
    # Text centered in bubble
    text_x = user_x + kBubblePadding
    text_y = bubble_y + kBubblePadding
    draw_text(draw, text_x, text_y, user_text, font_regular, 255)

    # AI bubble (left-aligned, white bg + black border)
    ai_bubble_y = bubble_y + user_bubble_h + 4
    ai_bubble_w = min(measure_text_width(ai_text, font_regular) + kBubblePadding * 2, max_bubble_w)
    # Wrap AI text if needed - calculate lines
    ai_text_area_w = ai_bubble_w - 2 * kBubblePadding
    ai_lines = []
    # Simple word-wrap simulation
    current_line = ""
    for ch in ai_text:
        test_line = current_line + ch
        if measure_text_width(test_line, font_regular) > ai_text_area_w and current_line:
            ai_lines.append(current_line)
            current_line = ch
        else:
            current_line = test_line
    if current_line:
        ai_lines.append(current_line)

    ai_bubble_h = len(ai_lines) * line_step + kBubblePadding * 2
    draw_round_rect(draw, bubble_x, ai_bubble_y, ai_bubble_w, ai_bubble_h,
                  kBorderRadiusMD, fill=255, outline=0, border_w=2)

    # Bug 9: render each line independently with >=24px spacing
    for i, line in enumerate(ai_lines):
        line_y = ai_bubble_y + kBubblePadding + i * line_step
        draw_text(draw, bubble_x + kBubblePadding, line_y, line, font_regular, 0)

    # Bottom status bar (18px)
    bar_y = kScreenHeight - kBottomBarH
    draw_rect(draw, 0, bar_y, kScreenWidth, kBottomBarH, 255)
    draw_hline(draw, bar_y, 0, kScreenWidth - 1, 0)
    # "正在聆听..." centered
    listen_text = "正在聆听..."
    listen_w = measure_text_width(listen_text, font_regular)
    listen_x = (kScreenWidth - listen_w) // 2
    listen_y = bar_y + (kBottomBarH - get_line_height(font_regular)) // 2
    draw_text(draw, listen_x, listen_y, listen_text, font_regular, 0)

    # Clock overlay (drawn AFTER page content, matching C++ RenderAll order)
    draw_clock_overlay(draw)

# ============================================================
# Settings page (Bug 4-8: flat list, Toggle, About, centered)
# ============================================================
def render_settings_page(draw):
    draw_rect(draw, 0, 0, kScreenWidth, kScreenHeight, 255)
    draw_status_bar(draw, title="设置", wifi=True, server=True, battery=85)

    content_top = kStatusBarHeight + kSpacingXXS  # 26
    content_bottom = kScreenHeight - kSpacingSM  # 294
    content_left = kSpacingMD  # 8

    # Flat list: Wi-Fi, 服务, 音量, 电池方向, 关于 (no icon for WiFi/Srv)
    items = [
        {'label': 'Wi-Fi', 'value': '已连接', 'icon': None, 'type': 'Normal'},
        {'label': '服务', 'value': '在线', 'icon': None, 'type': 'Normal'},
        {'label': '音量', 'value': '50%', 'icon': None, 'type': 'Normal'},
        {'label': '电池方向', 'value': None, 'icon': None, 'type': 'Checkbox', 'checked': False},
        {'label': '关于', 'value': None, 'icon': None, 'type': 'Action'},
    ]

    y = content_top
    font_h = get_line_height(font_regular)
    item_h = kItemMinHeight

    for i, item in enumerate(items):
        selected = (i == 0)
        fg_color = 255 if selected else 0  # white=255, black=0 in 'L' mode
        content_right = kScreenWidth - kSpacingMD

        # Selected: black background
        if selected:
            draw_rect(draw, content_left, y, content_right - content_left, item_h - 1, 0)

        # Icon
        icon_x = content_left
        if item.get('icon'):
            draw_text(draw, icon_x, y + (item_h - kIconSize) // 2, item['icon'], font_icon, fg_color)
            icon_x += kIconSize + kSpacingSM

        # Label
        label_y = y + (item_h - font_h) // 2
        draw_text(draw, icon_x, label_y, item['label'], font_regular, fg_color)

        # Right side
        if item['type'] == 'Checkbox':
            # Bug 5: Toggle switch (DrawRoundRect + DrawCircle)
            track_w = 30
            track_h = 12
            track_x = content_right - kSpacingMD - track_w
            track_y = y + (item_h - track_h) // 2
            checked = item.get('checked', False)
            fill = 0 if checked else 255
            draw_round_rect(draw, track_x, track_y, track_w, track_h,
                          track_h // 2, fill=fill, outline=0, border_w=1)
            thumb_r = 4
            if checked:
                thumb_cx = track_x + track_w - thumb_r * 2 - 2 + thumb_r
            else:
                thumb_cx = track_x + 2 + thumb_r
            thumb_cy = track_y + track_h // 2
            thumb_color = 255 if checked else 0
            draw_circle(draw, thumb_cx, thumb_cy, thumb_r, thumb_color)
        elif item.get('value'):
            val_w = measure_text_width(item['value'], font_regular)
            val_x = content_right - kSpacingMD - val_w
            draw_text(draw, val_x, label_y, item['value'], font_regular, fg_color)
            if item['type'] == 'Normal':
                draw_chevron(draw, val_x - kSpacingSM - 10, y + 2, 10, fg_color)
        elif item['type'] == 'Action':
            act_text = "关于"
            act_w = measure_text_width(act_text, font_regular) + kSpacingSM * 2
            act_h = font_h + kSpacingXS
            act_x = content_right - kSpacingMD - act_w
            act_y = y + (item_h - act_h) // 2
            btn_fill = 255 if selected else 0
            btn_outline = 0 if selected else 255
            draw_round_rect(draw, act_x, act_y, act_w, act_h,
                          kBorderRadiusSM, fill=btn_fill, outline=btn_outline, border_w=1)
            draw_text(draw, act_x + kSpacingSM, act_y + (act_h - font_h) // 2,
                     act_text, font_regular, 0 if selected else 255)

        # Divider
        divider_color = 255 if selected else 0
        draw_hline(draw, y + item_h - 1, content_left, content_right - 1, divider_color)
        y += item_h

    # Clock overlay (drawn AFTER page content)
    draw_clock_overlay(draw)

# ============================================================
# About dialog (Bug 6-8: real MAC, centered dialog, centered hint)
# ============================================================
def render_about_dialog(draw):
    dialog_w = 260
    dialog_h = 120
    # Bug 7: truly centered (no +20 offset)
    dialog_x = (kScreenWidth - dialog_w) // 2
    dialog_y = (kScreenHeight - dialog_h) // 2

    draw_round_rect(draw, dialog_x, dialog_y, dialog_w, dialog_h,
                  kBorderRadiusLG, fill=255, outline=0, border_w=kBorderMedium)

    font_h = get_line_height(font_regular)
    title_h = get_line_height(font_medium)

    text_y = dialog_y + kSpacingMD
    draw_text(draw, dialog_x + kSpacingMD, text_y, "关于", font_medium, 0)
    text_y += title_h + kSpacingXS

    draw_hline(draw, text_y, dialog_x + kSpacingMD, dialog_x + dialog_w - kSpacingMD, 0)
    text_y += kSpacingXS

    line_h = font_h + kSpacingXS
    draw_text(draw, dialog_x + kSpacingMD, text_y, "版本: v6.0.0-bugfix", font_regular, 0)
    text_y += line_h

    # ESP32-S3 MAC (esp_read_mac returns Espressif prefix + unique bytes)
    draw_text(draw, dialog_x + kSpacingMD, text_y, "MAC: 24:0A:C4:5E:9B:F1", font_regular, 0)
    text_y += line_h

    draw_text(draw, dialog_x + kSpacingMD, text_y, "芯片: ESP32-S3", font_regular, 0)

    # Bug 8: hint centered at bottom of dialog
    hint = "按任意键关闭"
    hint_w = measure_text_width(hint, font_regular)
    hint_x = dialog_x + (dialog_w - hint_w) // 2
    draw_text(draw, hint_x, dialog_y + dialog_h - font_h - kSpacingXS, hint, font_regular, 0)

def render_settings_about_dialog(draw):
    render_settings_page(draw)
    render_about_dialog(draw)

# ============================================================
# Main: Generate 3 PNG files
# ============================================================
def main():
    output_dir = os.path.expanduser("~/Documents/notellm/vibecoding-voice/firmware/pagepics")
    os.makedirs(output_dir, exist_ok=True)

    # 1. Chat page
    img1 = Image.new('L', (kScreenWidth, kScreenHeight), 255)
    draw1 = ImageDraw.Draw(img1)
    render_chat_page(draw1)
    img1.save(os.path.join(output_dir, 'preview_chat_page.png'))
    print(f"Saved: {output_dir}/preview_chat_page.png")

    # 2. Settings page
    img2 = Image.new('L', (kScreenWidth, kScreenHeight), 255)
    draw2 = ImageDraw.Draw(img2)
    render_settings_page(draw2)
    img2.save(os.path.join(output_dir, 'preview_settings_page.png'))
    print(f"Saved: {output_dir}/preview_settings_page.png")

    # 3. Settings + About dialog
    img3 = Image.new('L', (kScreenWidth, kScreenHeight), 255)
    draw3 = ImageDraw.Draw(img3)
    render_settings_about_dialog(draw3)
    img3.save(os.path.join(output_dir, 'preview_settings_about_dialog.png'))
    print(f"Saved: {output_dir}/preview_settings_about_dialog.png")

    print("\nAll 3 previews generated!")

if __name__ == '__main__':
    main()