/**
 * @file settings_renderer.h
 * @brief Modernized settings page renderer for rawdraw mode
 *
 * Features: card-based layout with clear sections, chevron indicators
 * for navigation, icon+label+value layout, consistent Style constants.
 */

#ifndef RAWDRAW_SETTINGS_RENDERER_H
#define RAWDRAW_SETTINGS_RENDERER_H

#include "page_renderer.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"
#include <vector>
#include <string>
#include <functional>

namespace rawdraw {

/**
 * @brief Settings item type
 */
enum class SettingsItemType {
    Normal,    ///< Navigable item with chevron indicator
    Checkbox,  ///< Toggleable checkbox
    Action,    ///< Action button (highlighted style)
    Section,   ///< Section header (non-interactive label)
};

/**
 * @brief Settings item definition
 */
struct SettingsItemDef {
    std::string label;
    std::string value;
    const char* icon = nullptr;             ///< font_zectrix icon code
    SettingsItemType type = SettingsItemType::Normal;
    bool checked = false;
    std::function<void()> on_click;
};

/**
 * @brief Modernized settings page renderer
 *
 * Card-based layout with:
 * - Compact icon + label + value rows
 * - Single outer card per option
 * - Inverted selected state
 * - Checkbox/Action variants
 */
class SettingsRenderer : public PageRenderer {
public:
    SettingsRenderer();
    ~SettingsRenderer() override;

    // PageRenderer interface
    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    // Data interface
    void SetItems(const std::vector<SettingsItemDef>& items);
    void UpdateItem(int index, const std::string& value);
    void UpdateChecked(int index, bool checked);
    int GetItemCount() const { return static_cast<int>(items_.size()); }
    int GetSelectedIndex() const { return selected_index_; }

    /**
     * @brief Everything the About surface reports.
     *
     * Supplied by the owner rather than read from globals by the renderer, so
     * the page cannot quietly start claiming something nobody decided it
     * should claim. Every field is a string the caller has already resolved:
     * there is no branch in here that turns "unknown" into a plausible value.
     */
    struct AboutInfo {
        std::string product;      ///< the name of the thing on the wall
        std::string firmware;     ///< the version of *this* firmware
        std::string hardware;     ///< panel family and colour count
        std::string lan_address;  ///< address, or why there is not one
        std::string pairing;      ///< dashboard pairing state
        std::string voice;        ///< compiled, muted, hub, in that order
        std::string build;        ///< which build-time switches produced this
        std::string diagnostic;   ///< the one counter worth a row on the panel
        std::string upstream;     ///< the version this was forked from
    };

    // Debug info
    void ShowDebugInfo();
    void SetFirmwareVersion(const char* version) { firmware_version_ = version; }
    void SetDeviceInfo(const char* mac, const char* chip) {
        mac_address_ = mac;
        chip_model_ = chip;
    }
    void SetAboutInfo(const AboutInfo& info);
    void ShowVolumeDialog(int volume);
    void SetVolumeDialogHandler(std::function<void(int, bool)> handler) {
        volume_dialog_handler_ = std::move(handler);
    }
    void ShowCategoryHint(int duration_ms = 2000);
    bool IsCategoryHintVisible() const;

    // About dialog
    void ShowAboutDialog() { showing_about_dialog_ = true; needs_full_refresh_ = true; }
    void HideAboutDialog() { showing_about_dialog_ = false; needs_full_refresh_ = true; }
    bool IsAboutDialogShowing() const { return showing_about_dialog_; }

    // Storage dialog
    void ShowStorageDialog(const std::string& used, const std::string& total,
                           int photos, int txts) {
        storage_used_ = used;
        storage_total_ = total;
        storage_photos_ = photos;
        storage_txts_ = txts;
        showing_storage_dialog_ = true;
        needs_full_refresh_ = true;
    }
    void HideStorageDialog() { showing_storage_dialog_ = false; needs_full_refresh_ = true; }
    bool IsStorageDialogShowing() const { return showing_storage_dialog_; }

    // Server address dialog
    void ShowServerDialog(const std::string& current_addr,
                          const std::string& local_addr,
                          const std::string& remote_addr) {
        server_current_addr_ = current_addr;
        server_local_addr_ = local_addr;
        server_remote_addr_ = remote_addr;
        server_selected_ = (current_addr == remote_addr) ? 1 : 0;
        showing_server_dialog_ = true;
        needs_full_refresh_ = true;
    }
    void HideServerDialog() { showing_server_dialog_ = false; needs_full_refresh_ = true; }
    bool IsServerDialogShowing() const { return showing_server_dialog_; }
    int GetServerDialogSelection() const { return server_selected_; }
    void SetServerDialogHandler(std::function<void(int)> handler) {
        server_dialog_handler_ = std::move(handler);
    }

    // Server address list dialog (scrollable list of history addresses)
    void ShowServerListDialog(const std::vector<std::string>& addresses,
                              const std::string& current_addr) {
        server_list_addresses_ = addresses;
        server_list_current_ = current_addr;
        server_list_selected_ = 0;
        server_list_scroll_offset_ = 0;
        // Find current address index
        for (size_t i = 0; i < addresses.size(); ++i) {
            if (addresses[i] == current_addr) {
                server_list_selected_ = static_cast<int>(i);
                break;
            }
        }
        showing_server_list_dialog_ = true;
        needs_full_refresh_ = true;
    }
    void HideServerListDialog() { showing_server_list_dialog_ = false; needs_full_refresh_ = true; }
    bool IsServerListDialogShowing() const { return showing_server_list_dialog_; }
    std::string GetServerListSelection() const {
        if (server_list_selected_ >= 0 && 
            server_list_selected_ < static_cast<int>(server_list_addresses_.size())) {
            return server_list_addresses_[server_list_selected_];
        }
        return "";
    }
    void SetServerListDialogHandler(std::function<void(const std::string&)> handler) {
        server_list_dialog_handler_ = std::move(handler);
    }

    void ShowThemeDialog(rawdraw::ThemeId current_theme) {
        theme_selected_ = 0;
        for (int i = 0; i < rawdraw::ThemeManager::ThemeCount(); ++i) {
            if (rawdraw::ThemeManager::ThemeAt(i) == current_theme) {
                theme_selected_ = i;
                break;
            }
        }
        showing_theme_dialog_ = true;
        needs_full_refresh_ = true;
    }
    void HideThemeDialog() { showing_theme_dialog_ = false; needs_full_refresh_ = true; }
    bool IsThemeDialogShowing() const { return showing_theme_dialog_; }
    void SetThemeDialogHandler(std::function<void(rawdraw::ThemeId)> handler) {
        theme_dialog_handler_ = std::move(handler);
    }

    // OTA firmware update dialog
    void ShowOtaDialog(const std::vector<std::string>& versions,
                       const std::string& current_version,
                       int selected_index,
                       int progress_percent,
                       const std::string& status_text,
                       int state);
    void HideOtaDialog() { showing_ota_dialog_ = false; needs_full_refresh_ = true; }
    bool IsOtaDialogShowing() const { return showing_ota_dialog_; }
    void SetOtaDialogHandler(std::function<void(int, bool, bool)> handler) {
        ota_dialog_handler_ = std::move(handler);
    }

private:
    // Layout computed from Style constants
    int CalcItemHeight(const SettingsItemDef& item) const;
    int CalcTotalContentHeight() const;
    int GetFirstSelectableIndex() const;
    int GetLastSelectableIndex() const;
    int FindPrevSelectable(int index) const;
    int FindNextSelectable(int index) const;
    void EnsureSelectionVisible();
    void DrawSelectedBackground(uint8_t* fb, int width, int x, int y, int w, int h) const;

    // Render a single settings item as a card row
    void RenderItem(uint8_t* fb, int width, int y, int content_left,
                    int index, bool selected, int row_h);

    // Get checkbox icon code
    const char* GetCheckboxIcon(bool checked) const;

    // Chevron right icon
    void DrawChevron(uint8_t* fb, int width, int x, int y, int size, Color color);

    // Draw debug info section
    void RenderDebugInfo(uint8_t* fb, int width, int height, int bottom_y);

    // Draw version info bar at bottom of settings page
    void RenderVersionBar(uint8_t* fb, int width, int height, int y);

    // Draw about dialog overlay
    void RenderAboutDialog(uint8_t* fb, int width, int height);
    void RenderVolumeDialog(uint8_t* fb, int width, int height);
    void RenderStorageDialog(uint8_t* fb, int width, int height);
    void RenderServerDialog(uint8_t* fb, int width, int height);
    void RenderServerListDialog(uint8_t* fb, int width, int height);
    void RenderThemeDialog(uint8_t* fb, int width, int height);
    void RenderOtaDialog(uint8_t* fb, int width, int height);
    void RenderOtaConfirmDialog(uint8_t* fb, int width, int height);  // OTA 确认弹窗
    void UpdateVolumeValue(int delta, bool commit);

    /// One About row, resolved. Label and value, nothing computed later.
    struct AboutRow {
        const char* label;
        std::string value;
    };

    /**
     * @brief The About rows, built once and used by both surfaces.
     *
     * The section list and the dialog used to carry their own copies of the
     * same six literals, which is how one of them ended up naming a vendor
     * website while the other did not. One builder means they cannot disagree.
     */
    std::vector<AboutRow> BuildAboutRows() const;

    /**
     * @brief Width of the left category rail, measured rather than assumed.
     *
     * Wide enough for the longest section label at the current font, clamped
     * between the historical 90 px and a third of the panel. Deriving it from
     * the measurement is what makes a clipped label structurally impossible
     * instead of merely unlikely.
     */
    int NavRailWidth() const;

    std::vector<SettingsItemDef> items_;
    int selected_index_ = 0;
    int scroll_offset_ = 0;  // preserved for compatibility/debug; item-window scrolling is primary
    int first_visible_index_ = 0;

    // Debug info state
    bool showing_debug_info_ = false;
    int64_t debug_hint_until_us_ = 0;  // timestamp when hint expires
    int64_t category_hint_until_us_ = 0;  // bottom category hint auto-hide timestamp
    std::string firmware_version_;
    std::string mac_address_;
    std::string chip_model_;
    AboutInfo about_;

    // About dialog state
    bool showing_about_dialog_ = false;
    bool showing_volume_dialog_ = false;
    bool showing_storage_dialog_ = false;
    int volume_dialog_value_ = 70;
    std::function<void(int, bool)> volume_dialog_handler_;

    // Storage dialog state
    std::string storage_used_;
    std::string storage_total_;
    int storage_photos_ = 0;
    int storage_txts_ = 0;

    // Server dialog state
    bool showing_server_dialog_ = false;
    std::string server_current_addr_;
    std::string server_local_addr_;
    std::string server_remote_addr_;
    int server_selected_ = 0;  // 0=local, 1=remote
    std::function<void(int)> server_dialog_handler_;

    // Server list dialog state (scrollable list of history addresses)
    bool showing_server_list_dialog_ = false;
    std::vector<std::string> server_list_addresses_;
    std::string server_list_current_;
    int server_list_selected_ = 0;
    int server_list_scroll_offset_ = 0;
    std::function<void(const std::string&)> server_list_dialog_handler_;

    bool showing_theme_dialog_ = false;
    int theme_selected_ = 0;
    std::function<void(rawdraw::ThemeId)> theme_dialog_handler_;
    static constexpr int kServerListVisibleRows = 5;  // Max visible rows in list

    // OTA dialog state
    bool showing_ota_dialog_ = false;
    std::vector<std::string> ota_versions_;
    std::string ota_current_version_;
    int ota_selected_index_ = 0;
    int ota_progress_percent_ = 0;
    std::string ota_status_text_;
    int ota_state_ = 0;
    std::function<void(int, bool, bool)> ota_dialog_handler_;
    static constexpr int kOtaVisibleRows = 4;

    // OTA confirm dialog state (弹窗确认固件更新)
    bool showing_ota_confirm_dialog_ = false;
    int ota_confirm_selected_ = 0;  // 0=确认更新, 1=取消
    std::string ota_confirm_firmware_name_;  // 待更新的固件名称

    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;
    const lv_font_t* icon_font_ = nullptr;
    const lv_font_t* value_font_ = nullptr;

    // Layout constants derived from Style
    static constexpr int kTitleBarH = 28;      // Title bar height
    static constexpr int kItemPadding = Style::kSpacingXS + 1;
    static constexpr int kIconSize = Style::kFontSizeSM;       // 16px
    static constexpr int kItemMinHeight = 30;
    // Right pane capacity: 8 compact option rows fit the 400x300 e-paper page.
    // Keep this as the only scroll-window threshold so future settings rows
    // can be added without hunting through the renderer.
    static constexpr int kVisibleOptionCount = 8;
};

}  // namespace rawdraw

#endif  // RAWDRAW_SETTINGS_RENDERER_H
