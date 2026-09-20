/**
 * @file holiday_fetcher.h
 * @brief Chinese official holiday schedule (State Council / 国务院调休)
 *
 * Fetches from timor.tech free API, caches in NVS.
 *
 * API response format:
 *   {"code":0, "holiday":{"2026-01-01":{"name":"元旦","rest":1},...}}
 *
 * Usage:
 *   HolidayFetcher::Init();                    // at boot
 *   HolidayFetcher::Fetch(2026);               // fetch + cache
 *   HolidayFetcher::IsHoliday(2026, 5, 1);     // → true (劳动节)
 *   HolidayFetcher::IsMakeupWorkday(2026, 5, 4); // → true (补班)
 */

#ifndef HOLIDAY_FETCHER_H
#define HOLIDAY_FETCHER_H

#include <stdint.h>
#include <stdbool.h>

namespace holiday_fetcher {

/**
 * @brief Maximum holiday/adjustment entries per year
 */
static constexpr int kMaxHolidayEntries = 50;

/**
 * @brief Single holiday/adjustment entry
 */
struct HolidayEntry {
    int16_t year;    // e.g. 2026
    int8_t month;    // 1-12
    int8_t day;      // 1-31
    char name[16];   // "春节", "国庆节", etc.
    bool is_rest;    // true = holiday/rest, false = makeup workday (补班)
};

/**
 * @brief Cached holiday data for a year
 */
struct HolidayCache {
    int year;
    int entry_count;
    HolidayEntry entries[kMaxHolidayEntries];
};

// ============================================================
// Public API
// ============================================================

/**
 * @brief Initialize module. Loads cached data from NVS.
 * @return true if cache was loaded successfully
 */
bool Init();

/**
 * @brief Fetch holiday data for a year from the API.
 * Blocks until complete. Updates NVS cache on success.
 * @param year Year to fetch (e.g. 2026)
 * @return true if fetch and parse succeeded
 */
bool Fetch(int year);

/**
 * @brief Check if a date is an official rest day (休).
 * @return true if date is marked as rest (includes official holidays)
 */
bool IsHoliday(int year, int month, int day);

/**
 * @brief Check if a date is a compensatory workday (班).
 * @return true if a weekend date requires makeup work
 */
bool IsMakeupWorkday(int year, int month, int day);

/**
 * @brief Get the holiday name for a rest day.
 * @return Holiday name (e.g. "春节") or nullptr if not a holiday
 */
const char* GetHolidayName(int year, int month, int day);

/**
 * @brief Get the "班" label if the date is a makeup workday.
 * @return "班" or nullptr
 */
const char* GetMakeupLabel(int year, int month, int day);

}  // namespace holiday_fetcher

#endif  // HOLIDAY_FETCHER_H
