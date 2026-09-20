/**
 * @file weather_card.h
 * @brief Compact weather card component for 1bpp ePaper display
 *
 * Displays current weather in a compact card layout:
 * - Large temperature value
 * - Weather icon (sunny/cloudy/rain/etc.)
 * - Feels like temperature
 * - Wind direction + scale
 * - City name + update time
 *
 * Layout avoids the top-right clock zone (kClockX=320, W=80, H=32).
 * Uses Style namespace constants for all spacing.
 *
 * Usage:
 * 1. Create WeatherCard with position
 * 2. SetData(WeatherData) to populate content
 * 3. Draw(fb, width, height) to render
 */

#ifndef RAWDRAW_WEATHER_CARD_H
#define RAWDRAW_WEATHER_CARD_H

#include <stdint.h>
#include <string>
#include "rawdraw.h"
#include "font_engine.h"
#include "framebuffer.h"
#include "refresh.h"
#include "style.h"
#include "common/weather_api.h"

// Forward declaration
struct WeatherData;

namespace rawdraw {

// ============================================================
// Weather card constants
// ============================================================

constexpr int kWeatherCardMaxWidth  = 400;  ///< Full screen width
constexpr int kWeatherCardMaxHeight = 132;  ///< Compact-but-readable height for 400x300
constexpr int kWeatherCardTempSize  = 48;   ///< Large temperature font
constexpr int kWeatherCardIconSize  = 48;   ///< Weather icon size

/**
 * @brief Compact weather card component
 *
 * Renders weather data in a rounded card with icon + temperature.
 * Uses independent RegionRefresh counter for partial refresh tracking.
 */
class WeatherCard {
public:
    /**
     * @brief Create weather card
     *
     * @param x Position x (default: 0, full width)
     * @param y Position y (default: below status bar)
     * @param w Card width (default: full screen width - margins)
     */
    WeatherCard(int x = 0,
                int y = Style::kStatusBarHeight + Style::kSpacingSM,
                int w = Style::kScreenWidth);

    ~WeatherCard();

    // ============================================================
    // Configuration
    // ============================================================

    void SetPosition(int x, int y);
    void SetWidth(int w);

    // ============================================================
    // Data
    // ============================================================

    /**
     * @brief Update card with new weather data
     */
    void SetData(const WeatherData& data);

    /**
     * @brief Set city name override
     */
    void SetCityName(const char* name);

    // ============================================================
    // Rendering
    // ============================================================

    /**
     * @brief Draw card to framebuffer
     *
     * @param fb Framebuffer pointer
     * @param width Framebuffer width
     * @param height Framebuffer height
     * @return true if card was drawn
     */
    bool Draw(uint8_t* fb, int width, int height);

    /**
     * @brief Draw using Framebuffer wrapper
     */
    bool Draw(Framebuffer* fb, int screen_width, int screen_height);

    /**
     * @brief Get card bounds (for dirty rect tracking)
     */
    Rect GetBounds() const;

    /**
     * @brief Get the refresh tracker for partial refresh control
     */
    RegionRefresh* GetRefreshTracker() { return &refresh_; }

private:
    // Icon drawing helpers
    void DrawWeatherIcon(uint8_t* fb, int width, int x, int y, WeatherIcon icon);

    // Layout constants
    int x_;
    int y_;
    int w_;

    // Data
    WeatherData data_;
    std::string city_name_;
    bool has_data_;

    // Refresh tracking
    RegionRefresh refresh_;

    // Fonts
    const lv_font_t* temp_font_;
    const lv_font_t* info_font_;
    const lv_font_t* icon_font_;
};

}  // namespace rawdraw

#endif  // RAWDRAW_WEATHER_CARD_H
