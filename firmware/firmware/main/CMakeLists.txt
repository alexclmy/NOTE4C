set(SOURCES
    "application.cc"
    "main.cc"
    "settings.cc"
    "system_info.cc"
    "audio/audio_codec.cc"
    "audio/audio_service.cc"
    "audio/codecs/es8311_audio_codec.cc"
    "audio/processors/audio_debugger.cc"
    "audio/processors/no_audio_processor.cc"
    "display/display.cc"
    "display/lcd_display.cc"
    "display/ui_page_registry.cc"
    "display/pages/factory_test_page_adapter.cc"
    # F5: LVGL display files kept - lcd_display.cc depends on LvglDisplay base class
    "display/lvgl_display/lvgl_display.cc"
    "display/lvgl_display/lvgl_theme.cc"
    "display/lvgl_display/lvgl_font.cc"
    "common/audio_fsm.cc"
    "common/button_gestures.cc"
    "common/earcon_gen.cc"
    "common/nav_model.cc"
    # Push-to-talk. utterance_buffer, voice_transport and voice_hub_config are
    # portable C++ and are also compiled by tests/host/run.sh; the rest is the
    # ESP-IDF glue around them. All of it compiles in both VOICE_PTT_ENABLED
    # configurations on purpose, so the OFF build is a real compile check of
    # the ON build's wiring rather than of an empty struct. Only the
    # instantiation in application.cc is behind the flag.
    "common/utterance_buffer.cc"
    "common/voice_transport.cc"
    "common/voice_hub_config.cc"
    "common/voice_capture.cc"
    "common/voice_uploader.cc"
    "audio/earcon_player.cc"
    "common/sleep_manager.cc"
    "boards/common/board.cc"
    "boards/common/button.cc"
    "boards/common/i2c_bus_lock.cc"
    "boards/common/i2c_device.cc"
    "boards/zectrix/zectrix_nfc.cc"
    "boards/zectrix-s3-epaper-4.2/board_power_bsp.cc"
    "boards/zectrix-s3-epaper-4.2/charge_status.cc"
    "boards/zectrix-s3-epaper-4.2/custom_lcd_display.cc"
    "boards/zectrix-s3-epaper-4.2/i2c_power_hook.cc"
    "boards/zectrix-s3-epaper-4.2/rtc_pcf8563.cc"
    "boards/zectrix-s3-epaper-4.2/zectrix-s3-epaper-4.2.cc"
    "boards/zectrix-s3-epaper-4.2/FT/acoustic_selftest.cc"
    "boards/zectrix-s3-epaper-4.2/FT/factory_test_service.cc"
    "rawdraw/rawdraw.cc"
    "rawdraw/theme.cc"
    "rawdraw/layout_utils.cc"
    "rawdraw/framebuffer.cc"
    "rawdraw/layers.cc"
    "rawdraw/refresh.cc"
    "rawdraw/clock.cc"
    # The welcome screen's graphics. All three are dependency-free — no ESP-IDF,
    # no LVGL, no font engine — and tests/host/run.sh compiles them, which is
    # what pins "the QR is black and white only" and "the QR carries the device's
    # own address and nothing else" as tests rather than as intentions.
    #
    # qrcodegen.c is Nayuki's, MIT, vendored byte-identical from
    # managed_components/espressif2022__esp_emote_gfx so an upstream fix is a
    # drop-in replace. It is the one C source in this list; the host runner
    # compiles it with the C compiler for the same reason. See
    # THIRD_PARTY_NOTICES.md.
    "rawdraw/qrcodegen.c"
    "rawdraw/qr_render.cc"
    "rawdraw/octopus_mark.cc"
    "rawdraw/welcome_screen.cc"
    "rawdraw/components/bubble.cc"
    "rawdraw/components/button.cc"
    "rawdraw/components/scrollview.cc"
    "rawdraw/components/panel.cc"
    "rawdraw/components/progress_bar.cc"
    "rawdraw/components/toggle.cc"
    "rawdraw/components/slider.cc"
    "rawdraw/components/card.cc"
    "rawdraw/components/list_item.cc"
    "rawdraw/components/status_bar.cc"
    "rawdraw/components/footer_bar.cc"
    "rawdraw/components/modal.cc"
    "rawdraw/components/voice_wakeup.cc"
    "common/photo_storage.cc"
    "common/photo_downloader.cc"
    "common/storage_manager.cc"
    "common/nvs_state.cc"
    # Poulailler dashboard. dashboard_slot.cc and dashboard_service.cc are
    # portable C++ and are also compiled by tests/host/run.sh on the build
    # machine; dashboard_manager.cc is the ESP-IDF glue around them.
    "common/dashboard_slot.cc"
    "common/dashboard_service.cc"
    "common/dashboard_manager.cc"
    # Semi-autonomous rendering, gated by CONFIG_AUTONOMY_ENABLED (default n).
    # All of it compiles and links in both configurations on purpose, exactly as
    # the push-to-talk path does: the OFF build is a real compile check of the
    # ON build's wiring, and the gate is over reachability rather than presence.
    # See AUTONOMY_COMPILED in main/dashboard_build_config.h.
    #
    # Every one of these is portable C++ with no ESP-IDF header and is compiled
    # by tests/host/run.sh; autonomy_compose.cc is additionally the producer of
    # the golden frames the control tower's TypeScript mirror is compared to.
    #
    # record_slot.cc is the profile's A/B store. It holds the same power-cut
    # discipline as dashboard_slot.cc, in its own translation unit, for a
    # variable-length record; the frame store is left exactly as hardware
    # validated it. See record_slot.h for why the duplication is the choice.
    #
    # json_scan.cc is the strict reader the profile and the forecast parser both
    # walk their documents with. It was folded into autonomy_profile.cc while
    # the profile was its only consumer; the forecast parser is the second, and
    # two copies of a reader whose refusal tokens cross the wire into the
    # tower's table would be two sets of refusals to keep in step.
    "common/record_slot.cc"
    "common/json_scan.cc"
    "common/autonomy_profile.cc"
    "common/autonomy_compose.cc"
    "common/autonomy_font_data.cc"
    "common/autonomy_service.cc"
    # The arbitration rule, the wake-cycle sequencing and the status block.
    # Pure functions of structs, all three, so the whole truth table is driven
    # by tests/host/run.sh rather than by arranging a device.
    "common/autonomy_policy.cc"
    "common/autonomy_cycle.cc"
    "common/autonomy_status.cc"
    # The one outbound origin this firmware has: api.open-meteo.com, HTTPS, no
    # API key, no redirect, 32 KB ceiling, one request per wake. The policy and
    # the parser are portable and host tested; openmeteo_transport_esp.cc is the
    # only file of the four that includes ESP-IDF, and it is the one the host
    # suite deliberately never links — so a host test that tried to make a real
    # request would fail to link rather than make one.
    "common/openmeteo_parse.cc"
    "common/openmeteo_client.cc"
    "common/openmeteo_transport_esp.cc"
    # The durable forecast cache: what makes a wake with no network still able
    # to draw something true, and what makes a wake with a fresh cache able to
    # skip the radio entirely.
    "common/weather_cache.cc"
    # Device API v2. device_config.cc is portable C++ and is compiled by
    # tests/host/run.sh; device_config_service.cc is the NVS and hook glue and
    # config_api.cc is the httpd layer over both.
    "common/device_config.cc"
    "common/device_config_service.cc"
    # The hybrid low-power contract. Portable C++ with no ESP-IDF header, so
    # tests/host/run.sh compiles this exact file; the esp_sleep, NVS and Wi-Fi
    # glue around it lives in application.cc.
    "common/power_policy.cc"
    # The battery ADC's owner: one open, one lock, one close, and a cache that
    # keeps a polling client off the converter. Portable C++ with no ESP-IDF
    # header — the driver calls are injected as hooks by the board — so
    # tests/host/run.sh compiles this exact file. See its header for the
    # concurrency defect it replaced.
    "common/battery_monitor.cc"
    "ui/renderers/rawdraw/config_api.cc"
    "ui/renderers/rawdraw/dashboard_api.cc"
    "ui/renderers/rawdraw/dashboard_renderer.cc"
    "common/bluetooth_manager.cc"
    "common/ble_gatt_service.cc"
    "common/ble_image_receiver.cc"
    "ui/widgets/status_bar.cc"
    "ui/renderers/rawdraw/settings_renderer.cc"
    "ui/renderers/rawdraw/wifi_renderer.cc"
    "ui/renderers/rawdraw/photo_gallery.cc"
    "ui/renderers/rawdraw/photo_detail_renderer.cc"
    "ui/renderers/rawdraw/ap_transfer_renderer.cc"
    "ui/renderers/rawdraw/ap_transfer_server.cc"
    "ui/rawdraw_ui_manager.cc"
    "ui/epd_refresh.cc"
    "streaming/text_chunker.cc"
    "streaming/tts_streamer.cc"
    "streaming/stream_pipeline.cc"
)

# Pages this device cannot reach, and their third-party data sources.
# See main/dashboard_build_config.h for why these are removed from the
# build rather than merely hidden from navigation.
option(DASHBOARD_MINIMAL_UI "Compile only the reachable dashboard pages" ON)
# Microphone capture for push-to-talk. Off until the audio hardware gates pass.
# See main/dashboard_build_config.h for why, and docs/VOICE-FEASIBILITY.md.
option(VOICE_PTT_ENABLED "Compile the push-to-talk microphone path" OFF)
set(LEGACY_PAGE_SOURCES
    "ui/renderers/rawdraw/almanac_renderer.cc"
    "ui/renderers/rawdraw/calendar_renderer.cc"
    "ui/renderers/rawdraw/weather_renderer.cc"
    "ui/renderers/rawdraw/weather_detail_renderer.cc"
    "ui/renderers/rawdraw/news_renderer.cc"
    "ui/renderers/rawdraw/lifebar_renderer.cc"
    "ui/renderers/rawdraw/yearprogress_renderer.cc"
    "ui/renderers/rawdraw/ebook_renderer.cc"
    "ui/renderers/rawdraw/chat_renderer.cc"
    "ui/renderers/rawdraw/log_renderer.cc"
    "ui/renderers/rawdraw/font_debug_renderer.cc"
    "ui/renderers/rawdraw/font_metrics_renderer.cc"
    "rawdraw/components/calendar.cc"
    "rawdraw/components/weather_card.cc"
    "common/weather_api.cc"
    "common/holiday_fetcher.cc"
)
if(NOT DASHBOARD_MINIMAL_UI)
    list(APPEND SOURCES ${LEGACY_PAGE_SOURCES})
endif()

set(INCLUDE_DIRS
    "."
    "audio"
    "display"
    # F5: LVGL include directory kept for header compatibility, source files disabled
    "display/lvgl_display"
    "ui"
    "ui/pages"
    "ui/widgets"
    "ui/renderers"
    "ui/renderers/common"
    "ui/renderers/rawdraw"
    "streaming"
    "protocols"
    "components/78__esp-ml307/include"
    "boards/common"
    "rawdraw"
    "rawdraw/components"
    "common"
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES
        spiffs
        mbedtls
        spi_flash
        efuse
        driver
        esp_wifi
        bt
        nvs_flash
        78__esp-opus
        78__esp-wifi-connect
        78__xiaozhi-fonts
        esp_app_format
        app_update
        esp_partition
        fatfs
        esp_driver_i2s
        esp_http_client
        esp_https_ota
        json
        esp_timer
        esp_http_server
        mqtt
        esp_psram
        esp_lcd
        esp_netif
        esp_event
        log
)

target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE
    BOARD_TYPE=\"zectrix-s3-epaper-4.2\"
    BOARD_NAME=\"zectrix-s3-epaper-4.2\"
    BUILTIN_TEXT_FONT=SourceHanSansSC_Regular_slim
    BUILTIN_ICON_FONT=font_zectrix_16_1
    PROJECT_VER=\"6.5.9\"  # F5: Make version available to renderer code
    DASHBOARD_MINIMAL_UI=$<BOOL:${DASHBOARD_MINIMAL_UI}>
    VOICE_PTT_ENABLED=$<BOOL:${VOICE_PTT_ENABLED}>
)
