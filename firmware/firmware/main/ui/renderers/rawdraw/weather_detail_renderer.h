/**
 * @file weather_detail_renderer.h
 * @brief Weather detail page renderer with hourly timeline
 */

#ifndef RAWDRAW_WEATHER_DETAIL_RENDERER_H
#define RAWDRAW_WEATHER_DETAIL_RENDERER_H

#include "common/weather_api.h"
#include "page_renderer.h"
#include <string>
#include <vector>

namespace rawdraw {

struct WeatherHourPoint {
    std::string label;
    std::string icon_code;
    std::string weather_text;
    int32_t temp = 0;
};

class WeatherDetailRenderer : public PageRenderer {
public:
    WeatherDetailRenderer();
    ~WeatherDetailRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    void Update(const WeatherData& data);
    void SetHourlyForecast(const std::vector<WeatherHourPoint>& points);

private:
    void DrawHourDetailModal(uint8_t* fb, int width, int height);
    void BuildFallbackTimeline();

    WeatherData data_;
    std::vector<WeatherHourPoint> hourly_;
    int selected_hour_ = 0;
    bool detail_open_ = false;
    bool has_data_ = false;

    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;
    const lv_font_t* icon_font_ = nullptr;
};

}  // namespace rawdraw

#endif  // RAWDRAW_WEATHER_DETAIL_RENDERER_H
