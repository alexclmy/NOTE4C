#pragma once

#include <stdint.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Settings icons extracted from FontAwesome (fontawesome-webfont.ttf)
// Generated with lv_font_conv --bpp 1 --no-compress
// Source: https://fontawesome.com/v4/icons/

// 16px settings icons
LV_FONT_DECLARE(fa_settings_16);
extern const lv_font_t fa_settings_16;

// Unicode constants for settings icons (FontAwesome v4 codes)
// Sidebar category icons
#define FA_SETTINGS_GEAR        "\xef\x80\x93"  // U+F013 ⚙️ Gear/Cog (系统)
#define FA_SETTINGS_WIFI        "\xef\x87\xab"  // U+F1EB 📶 Wi-Fi (网络)
#define FA_SETTINGS_WRENCH      "\xef\x82\xad"  // U+F0AD 🔧 Wrench (功能)
#define FA_SETTINGS_DATABASE    "\xef\x87\x80"  // U+F1C0 🗄️ Database (存储)
#define FA_SETTINGS_INFO        "\xef\x81\x9a"  // U+F05A ℹ️ Info-circle (关于)

// Option row icons
#define FA_SETTINGS_VOLUME      "\xef\x80\xa8"  // U+F028 🔊 Volume-up
#define FA_SETTINGS_BATTERY     "\xef\x89\x80"  // U+F240 🔋 Battery-full
#define FA_SETTINGS_POWER       "\xef\x80\x91"  // U+F011 ⏻ Power-off (重启)
#define FA_SETTINGS_SERVER      "\xef\x88\xb3"  // U+F233 🖥️ Server
#define FA_SETTINGS_SYNC        "\xef\x80\xa1"  // U+F021 🔄 Sync/Refresh
#define FA_SETTINGS_CHECK       "\xef\x80\x8c"  // U+F00C ✓ Check
#define FA_SETTINGS_ARROW_RIGHT "\xef\x81\xa1"  // U+F061 → Arrow-right
#define FA_SETTINGS_EDIT        "\xef\x81\x84"  // U+F044 ✏️ Edit
#define FA_SETTINGS_TRASH       "\xef\x80\x94"  // U+F014 🗑️ Trash
#define FA_SETTINGS_CALENDAR    "\xef\x81\xb3"  // U+F073 📅 Calendar (日期格式)
#define FA_SETTINGS_COMMENT     "\xef\x81\xb5"  // U+F075 💬 Comment (AI对话长度)
#define FA_SETTINGS_LINK        "\xef\x8c\x81"  // U+F0C1 🔗 Link (服务地址)
#define FA_SETTINGS_CLOCK       "\xef\x80\x97"  // U+F017 🕐 Clock (时钟显示)
#define FA_SETTINGS_MAP_MARKER  "\xef\x81\x81"  // U+F041 📍 Map-marker (服务地址)
#define FA_SETTINGS_BLUETOOTH   "\xef\x8a\x93"  // U+F293 Bluetooth-b

// Menu icons (for quick switch)
#define FA_SETTINGS_BOOK        "\xef\x80\xad"  // U+F02D 📖 Book (电子书)
#define FA_SETTINGS_IMAGE       "\xef\x80\xbe"  // U+F03E 🖼️ Image (相册)
#define FA_SETTINGS_NEWSPAPER   "\xef\x87\xaa"  // U+F1EA 📰 Newspaper-o (热点)

#ifdef __cplusplus
}
#endif
