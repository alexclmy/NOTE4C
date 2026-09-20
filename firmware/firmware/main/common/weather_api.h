/**
 * @file weather_api.h
 * @brief HeWeather (和风天气) API client for ESP32
 *
 * Fetches real-time weather data via HTTP GET. Uses esp_http_client
 * with select()-based timeout (NO setsockopt(SO_RCVTIMEO)).
 *
 * API: https://dev.qweather.com/docs/api/weather/weather-now/
 *
 * Usage:
 * 1. weather_api_init("YOUR_KEY", "hangzhou")
 * 2. Callback receives WeatherData on success
 * 3. Timer triggers hourly auto-refresh
 */

#ifndef WEATHER_API_H
#define WEATHER_API_H

#include <stdint.h>
#include <stdbool.h>
#include <string>
#include <functional>
#include <vector>

// ============================================================
// Weather data model
// ============================================================

/**
 * @brief Parsed weather data from HeWeather API
 */
struct WeatherForecastDay {
    std::string label;        // Today / Tomorrow / etc.
    std::string weather_text; // 晴 / 多云 / 小雨
    std::string icon_code;    // QWeather icon code string
    int32_t temp_min = 0;
    int32_t temp_max = 0;
};

struct WeatherData {
    std::string city;         // City name in Chinese
    std::string temp;         // Current temperature (e.g., "25")
    std::string feels_like;   // Feels like temperature (e.g., "27")
    std::string weather_icon; // QWeather icon code for current weather (e.g., "100")
    std::string weather_text; // Weather condition (e.g., "晴", "多云", "小雨")
    std::string wind_dir;     // Wind direction (e.g., "东南风")
    std::string wind_scale;   // Wind scale (e.g., "3")
    std::string humidity;     // Humidity percentage (e.g., "45")
    std::string update_time;  // Last update time (e.g., "14:30")
    std::string air_quality;  // Air quality text (e.g., "优")
    int32_t air_aqi = -1;     // AQI number
    int32_t temp_int;         // Numeric temperature for icon selection
    std::vector<WeatherForecastDay> forecast;
};

/**
 * @brief Weather icon codes for 1bpp rendering
 * Maps weather condition text to icon character codes.
 */
enum class WeatherIcon {
    Sunny,       // 晴
    Cloudy,      // 多云
    Overcast,    // 阴
    Rain,        // 雨 (any rain type)
    Snow,        // 雪
    Fog,         // 雾
    Unknown,     // Fallback
};

/**
 * @brief Map weather condition text to icon type
 */
WeatherIcon ParseWeatherIcon(const char* weather_text);

// ============================================================
// API interface
// ============================================================

/**
 * @brief Callback type for weather data delivery
 */
using WeatherCallback = std::function<void(const WeatherData&)>;

/**
 * @brief Initialize weather API client
 *
 * Sets up the hourly auto-refresh timer using esp_timer.
 *
 * @param api_key HeWeather API key
 * @param city_code City location ID (e.g., "101210101" for Hangzhou)
 * @param callback Function called when data arrives
 */
void weather_api_init(const char* api_key, const char* city_code, WeatherCallback callback);

/**
 * @brief Trigger a manual weather data fetch
 *
 * @return true if request started, false if already in progress
 */
bool weather_api_fetch_now();

/**
 * @brief Change the city
 *
 * @param city_code New city location ID
 */
void weather_api_set_city(const char* city_code);

/**
 * @brief Set the API key
 */
void weather_api_set_key(const char* api_key);

/**
 * @brief Get the current city code
 */
const char* weather_api_get_city();

/**
 * @brief Check if API client is initialized
 */
bool weather_api_is_ready();

/**
 * @brief Get the last fetched weather data
 */
const WeatherData* weather_api_get_last_data();

#endif  // WEATHER_API_H
