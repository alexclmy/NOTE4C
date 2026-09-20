/**
 * @file dashboard_renderer.h
 * @brief Full-screen renderer for the composed poulailler dashboard frame.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * The device composes nothing here. The Mac produces a finished 400x300 BWRY
 * frame in exactly the panel's own 2bpp layout, and this renderer copies it
 * into the framebuffer verbatim. No scaling, no dithering, no text overlay:
 * anything drawn on top would be drawn on top of the operator's design.
 *
 * When no frame has ever been stored, it draws a welcome screen instead of a
 * blank one, so a freshly flashed device explains itself: a QR of the device's
 * own `http://<ip>/` — never a token — three short English lines about what
 * this is and how it is doing, the house mascot, and the four pigments this
 * panel actually has. Every one of those lines is measured against the box it
 * is drawn in rather than placed at a fixed x; `RenderPlaceholder`'s own
 * comment says what went wrong on the glass before that was true.
 */

#ifndef RAWDRAW_DASHBOARD_RENDERER_H
#define RAWDRAW_DASHBOARD_RENDERER_H

#include <stdint.h>

#include <string>

#include "page_renderer.h"

namespace rawdraw {

class DashboardRenderer : public PageRenderer {
public:
    ~DashboardRenderer() override;

    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    /// Full-screen page: the status bar is suppressed for it.
    bool IsFullscreen() const { return true; }

    /**
     * @brief Copy @p frame (kFrameBytes) into the renderer's own buffer.
     *
     * Called from the dashboard render task before triggering a refresh. The
     * copy exists so the framebuffer paint cannot race the store.
     */
    bool SetFrame(const uint8_t* frame);

    /// True once a frame has been handed to this renderer.
    bool has_frame() const { return has_frame_; }

    /**
     * @brief What the placeholder screen is allowed to claim.
     *
     * Call it whenever the underlying facts change. Before this existed the
     * function had no callers at all, so the placeholder always said
     * "Waiting for Wi-Fi" even on a connected, paired device.
     *
     * The wording itself comes from nav::StatusLine() (main/common/nav_model.h)
     * so the device says the same thing here as it does in its logs, and so the
     * strings are host-tested.
     */
    void SetPlaceholderInfo(const std::string& ip, bool provisioned,
                            bool lan_service_running = false);

private:
    void RenderPlaceholder(uint8_t* fb, int width, int height);

    uint8_t* frame_ = nullptr;   ///< kFrameBytes, PSRAM
    bool has_frame_ = false;
    std::string ip_;
    bool provisioned_ = false;
    bool lan_service_running_ = false;
};

}  // namespace rawdraw

#endif  // RAWDRAW_DASHBOARD_RENDERER_H
