/**
 * @file weather_renderer.h
 * @brief Weather page renderer for rawdraw mode
 *
 * Rawdraw weather page renderer for 400x300 e-paper.
 */

#ifndef RAWDRAW_WEATHER_RENDERER_H
#define RAWDRAW_WEATHER_RENDERER_H

#include "common/weather_api.h"
#include "page_renderer.h"
#include "rawdraw/style.h"
#include <string>

namespace rawdraw {

class WeatherRenderer : public PageRenderer {
public:
    WeatherRenderer();
    ~WeatherRenderer() override;

    // PageRenderer interface
    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    // Data interface
    void Update(const WeatherData& data);
    void SetCityName(const char* name);
    void SetFirmwareVersion(const char* version);

private:
    WeatherData current_data_;
    std::string city_name_;
    std::string firmware_version_;
    bool has_data_;
    int page_index_ = 0;  // Selected forecast day index
    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;
};

}  // namespace rawdraw

#endif  // RAWDRAW_WEATHER_RENDERER_H
