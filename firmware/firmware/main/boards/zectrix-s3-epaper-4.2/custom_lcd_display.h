#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <stdint.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#ifdef HAVE_LVGL
#include <lvgl.h>
#endif

#include <atomic>
#include <functional>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "lcd_display.h"

/* Display color */
typedef enum {
    DRIVER_COLOR_WHITE  = 0xff,
    DRIVER_COLOR_BLACK  = 0x00,
    FONT_BACKGROUND = DRIVER_COLOR_WHITE,
}COLOR_IMAGE;

typedef struct {
    uint8_t cs;
    uint8_t dc;
    uint8_t rst;
    uint8_t busy;
    uint8_t mosi;
    uint8_t scl;
    uint8_t power;
    int spi_host;
    int buffer_len;
    int panel_type;
}custom_lcd_spi_t;

typedef enum {
    EPD_PANEL_1BPP = 0,
    EPD_PANEL_4COLOR_SSD2683 = 1,
} epd_panel_type_t;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} Rect;

class CustomLcdDisplay : public LcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,custom_lcd_spi_t _lcd_spi_data);
    ~CustomLcdDisplay();

    void WriteRaw1bpp(int x, int y, int w, int h, const uint8_t* data, size_t len) override;
    void InvertRegion(int x, int y, int w, int h) override;
    void DrawTexts(const std::vector<TextItem>& texts, bool clear) override;

    void EPD_Init();
    void EPD_Clear();
    void EPD_Display();

    void EPD_DisplayPartBaseImage();
    void EPD_Init_Partial();
    void EPD_DisplayPart();
    void EPD_DrawColorPixel(uint16_t x, uint16_t y,uint8_t color);

    // Immediate refresh without forcing a full e-paper update.
    void RequestUrgentRefresh() override;
    // Force a full e-paper refresh on the next immediate update.
    void RequestUrgentFullRefresh() override;

    // Refresh state for sleep gating
    bool IsRefreshPending();

    /**
     * @brief Discard any completion signals left over from earlier refreshes.
     *
     * Must be called before triggering a refresh whose completion you intend to
     * wait for; otherwise a stale signal satisfies the wait immediately and the
     * caller concludes the panel is done before it has started.
     * @return number of stale signals discarded.
     */
    int DrainRefreshDoneSignals();

    /**
     * @brief Block until the refresh task reports the panel is done.
     *
     * @param timeout_ms upper bound. This must exceed the driver's own worst
     *        case: read_busy() waits up to 120 s per call and
     *        EPD_TurnOnDisplay() calls it three times, so a single four-color
     *        refresh can legitimately occupy the driver for about six minutes.
     * @return true if a completion signal arrived within the timeout.
     */
    bool WaitRefreshDone(uint32_t timeout_ms);

    /**
     * @brief Whether the last refresh completed without a BUSY-pin timeout.
     *
     * False means read_busy() gave up and the driver carried on regardless, so
     * the image on the panel is not known to match the framebuffer.
     */
    bool LastRefreshOk() const { return last_refresh_ok_; }

    /// Total number of BUSY-pin timeouts since boot.
    uint32_t BusyTimeoutCount() const { return busy_timeout_count_.load(std::memory_order_relaxed); }

    /// Worst-case time the driver may spend inside one four-color refresh.
    /// 3 read_busy() calls in EPD_TurnOnDisplay(), 120 s each, plus margin.
    static constexpr uint32_t kWorstCaseRefreshMs = 3u * 120u * 1000u + 30u * 1000u;

    // Notify when refresh transitions from busy to idle.
    void SetOnRefreshIdle(std::function<void()> cb);
    void SetNextKickMs(uint32_t kick_ms);
    
private:
    const custom_lcd_spi_t lcd_spi_data;
    const int Width;
    const int Height;
    const epd_panel_type_t panel_type_;
    spi_device_handle_t spi = nullptr;
    bool spi_bus_inited = false;
    uint8_t *buffer      = nullptr;   // 1bpp framebuffer
    uint8_t *prev_buffer = nullptr;   // optional
    uint8_t *tx_buf      = nullptr;   // snapshot buffer for async send (size = buffer_len)

    // LVGL (only compiled when HAVE_LVGL is defined)
#ifdef HAVE_LVGL
    static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p);
#endif

    // SPI/GPIO
    void spi_gpio_init();
    void spi_port_init();
    void spi_port_rx_init();
    void read_busy();

    void set_cs_1(){gpio_set_level((gpio_num_t)lcd_spi_data.cs,1);}
    void set_cs_0(){gpio_set_level((gpio_num_t)lcd_spi_data.cs,0);}
    void set_dc_1(){gpio_set_level((gpio_num_t)lcd_spi_data.dc,1);}
    void set_dc_0(){gpio_set_level((gpio_num_t)lcd_spi_data.dc,0);}
    void set_rst_1(){gpio_set_level((gpio_num_t)lcd_spi_data.rst,1);}
    void set_rst_0(){gpio_set_level((gpio_num_t)lcd_spi_data.rst,0);}
    void set_scl_1(){gpio_set_level((gpio_num_t)lcd_spi_data.scl,1);}
    void set_scl_0(){gpio_set_level((gpio_num_t)lcd_spi_data.scl,0);}

    void SPI_SendByte(uint8_t data);
    uint8_t SPI_RecvByte();
    uint8_t EPD_RecvData();
    void EPD_PowerOn();
    void EPD_PowerOff();
    void EPD_SendData(uint8_t data);
    void EPD_SendCommand(uint8_t command);
    void writeBytes(uint8_t *buf,int len);
    void writeBytes(const uint8_t *buf, int len);

    // SSD1683 helpers
    void EPD_SetWindows(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend);
    void EPD_SetCursor(uint16_t Xstart, uint16_t Ystart);
    void EPD_TurnOnDisplay();
    void EPD_TurnOnDisplayPart();
    void EPD_SetFullWindowAndCounter(); // ***关键：恢复全屏窗口+计数器***
    bool IsFourColorPanel() const { return panel_type_ == EPD_PANEL_4COLOR_SSD2683; }
    void EPD_DisplayFourColorTestPattern();

    // Helper functions for partial display
    void bitInterleave(unsigned char bytes1, unsigned char bytes2);
    void WRITE_WHITE_TO_HLINE();
    void WRITE_HLINE_TO_VLINE();
    void WRITE_VLINE_TO_HLINE();

    uint8_t bw_threshold    = 200;

    // -------------------------
    // Async refresh mechanism
    // -------------------------
    void start_refresh_task();
    void stop_refresh_task();
    static void refresh_task_entry(void *arg);
    void refresh_task_loop();

    SemaphoreHandle_t dirty_mutex = nullptr;
    TaskHandle_t      refresh_task = nullptr;

    Rect dirty = {0,0,0,0};
    bool pending = false;

    bool urgent_refresh = false;
    bool force_full_refresh_ = false;
    TickType_t last_sample_tick = 0;
    int sample_interval_ms = 300; // 节流：采样间隔（可调 200~800）

    bool prev_buffer_synced = false;  // 标志：prev_buffer 是否已与屏幕同步
    bool refresh_in_progress = false;
    bool refresh_busy_seen_ = false;

    // Completion signalling for callers that must know when the panel is
    // actually done, rather than inferring it by polling the busy flags.
    SemaphoreHandle_t refresh_done_sem_ = nullptr;
    // Incremented every time read_busy() gives up waiting for the BUSY pin.
    // The refresh task samples it either side of a refresh to decide whether
    // the panel really completed or the driver simply stopped waiting.
    std::atomic<uint32_t> busy_timeout_count_{0};
    bool last_refresh_ok_ = true;
    uint32_t next_kick_ms_ = 0;
    std::function<void()> on_refresh_idle_;

    void UpdateDisplayBusyLocked();
    bool CheckRefreshIdleLocked();
    void SignalRefreshDone();

    // 文本渲染辅助
    void render_text_to_buffer(const char* text, int x, int y, const lv_font_t* font);

    // Rawdraw-backed drawing helpers (P0: direct framebuffer operations)
    // Acquire mutex, draw on buffer, mark dirty — NO refresh trigger (caller decides)
public:
    void RawDrawRoundRect(int x, int y, int w, int h, int radius,
                          bool filled, bool has_border);
    void RawDrawHLine(int y, int thickness);
    void RawInvertRegion(int x, int y, int w, int h);
    uint8_t* GetFramebuffer() { return buffer; }
    // The framebuffer's real byte length. On the 4-color panel this is 2bpp
    // (30000 = ((400*2+7)/8)*300), NOT the 1bpp size a (width+7)/8 * height
    // recompute would give. Callers that copy the whole frame (e.g. the
    // quick-switch backing) must use this, or they clip to the top half.
    size_t GetFramebufferLen() const { return static_cast<size_t>(lcd_spi_data.buffer_len); }
    int GetFBWidth() const { return Width; }
    int GetFBHeight() const { return Height; }
    SemaphoreHandle_t GetMutex() { return dirty_mutex; }
    bool DisplayRaw4ColorImage(const uint8_t* data, size_t len, int width, int height) override;
};

#endif // __CUSTOM_LCD_DISPLAY_H__
