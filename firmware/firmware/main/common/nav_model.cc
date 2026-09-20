/**
 * @file nav_model.cc
 * @brief Implementation of the portable navigation model.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "nav_model.h"

#include <stdio.h>

namespace nav {

namespace {

constexpr Page kRing[NavModel::kRingSize] = {
    Page::Dashboard,
    Page::Gallery,
    Page::Settings,
};

bool IsClick(Event event) {
    return event == Event::kUpClick || event == Event::kDownClick ||
           event == Event::kBootClick;
}

Action Make(ActionType type) {
    Action a;
    a.type = type;
    return a;
}

Action MakePage(ActionType type, Page page) {
    Action a;
    a.type = type;
    a.page = page;
    return a;
}

Action MakeMove(ActionType type, int delta) {
    Action a;
    a.type = type;
    a.delta = delta;
    return a;
}

}  // namespace

Page RingStep(Page page, int delta) {
    size_t index = 0;
    for (size_t i = 0; i < NavModel::kRingSize; ++i) {
        if (kRing[i] == page) {
            index = i;
            break;
        }
    }
    // Transfer is not on the ring. It is modal and cannot receive ring
    // movement, so callers never reach here with it; if one ever does, the
    // lookup above leaves index at Dashboard rather than reading out of range.
    const size_t size = NavModel::kRingSize;
    const size_t step = (delta < 0) ? size - 1 : 1;
    return kRing[(index + step) % size];
}

Action NavModel::ResolveBack() const {
    // Innermost context first. This order is the contract; docs/MANUAL.md
    // states it and tests/host/test_nav_model.cc asserts each layer.
    if (ctx_.dialog_open) return Make(ActionType::kCloseDialog);
    if (ctx_.quick_switch_open) return Make(ActionType::kCloseQuickSwitch);
    if (ctx_.fullscreen_photo) return Make(ActionType::kCloseFullscreen);
    // Back out of transfer to where it is entered from, which is Settings.
    if (ctx_.transfer_active || ctx_.page == Page::Transfer) {
        return MakePage(ActionType::kExitTransfer, Page::Settings);
    }
    if (ctx_.page != Page::Dashboard) {
        return MakePage(ActionType::kSwitchPage, Page::Dashboard);
    }
    return Make(ActionType::kFeedbackNoop);
}

Action NavModel::ResolveClick(Event event) const {
    const int delta = (event == Event::kUpClick) ? -1 : 1;

    if (ctx_.dialog_open) {
        if (event == Event::kBootClick) return Make(ActionType::kDialogConfirm);
        return MakeMove(ActionType::kDialogMove, delta);
    }
    if (ctx_.quick_switch_open) {
        if (event == Event::kBootClick) return Make(ActionType::kQuickSwitchConfirm);
        return MakeMove(ActionType::kQuickSwitchMove, delta);
    }

    switch (ctx_.page) {
        case Page::Dashboard:
            // BOOT click used to force a stored-frame redraw here
            // (application.cc:480-504). The redraw is not lost: it is the
            // "Repaint screen" entry of the quick switch this now opens, which
            // is also the only way three buttons can reach Gallery.
            if (event == Event::kBootClick) return Make(ActionType::kOpenQuickSwitch);
            return MakePage(ActionType::kSwitchPage, RingStep(Page::Dashboard, delta));

        case Page::Gallery:
        case Page::Settings:
            // Documented exception to "UP/DOWN is lateral": with three buttons
            // a list needs them (PRODUCT-PLAN.md section 3.1).
            if (event == Event::kBootClick) return Make(ActionType::kSelect);
            return MakeMove(ActionType::kListMove, delta);

        case Page::Transfer:
            // The transfer page is a static instruction sheet. Its only useful
            // click is redrawing it after the panel ghosted.
            if (event == Event::kBootClick) return Make(ActionType::kRepaintPage);
            return Make(ActionType::kFeedbackNoop);
    }
    return Make(ActionType::kFeedbackNoop);
}

Action NavModel::Resolve(Event event) const {
    switch (event) {
        case Event::kComboLong:
            return Make(ActionType::kWifiConfigAp);
        case Event::kBootLong:
            // Reserved exclusively. No page, overlay or modal may claim it.
            return Make(ActionType::kPttArm);
        case Event::kUpLong:
            return ResolveBack();
        case Event::kDownLong:
            if (ctx_.page == Page::Dashboard && !ctx_.quick_switch_open &&
                !ctx_.dialog_open && !ctx_.fullscreen_photo && !ctx_.transfer_active) {
                return Make(ActionType::kFeedbackNoop);
            }
            return Make(ActionType::kHome);
        case Event::kUpClick:
        case Event::kDownClick:
        case Event::kBootClick:
            return ResolveClick(event);
    }
    return Make(ActionType::kFeedbackNoop);
}

Action NavModel::Decide(Event event) {
    if (ctx_.refresh_busy && IsClick(event)) {
        // Depth 1, newest wins. Coalescing rather than queueing is deliberate:
        // a queue drains into a burst of page changes the user stopped wanting
        // a minute ago. Same reasoning as dashboard::RefreshCoordinator.
        pending_ = event;
        has_pending_ = true;
        return Make(ActionType::kPendingLatched);
    }

    // Anything resolved immediately supersedes whatever was latched, and that
    // is true for clicks as well as for Back and Home. Clearing this only on
    // long presses left a window where a click handled outside a refresh was
    // applied now and the older latched click was replayed at the next
    // refresh-idle, navigating twice and in the wrong order. Depth 1, newest
    // wins, applied consistently: if the newest event has already run, there
    // is nothing older worth replaying.
    has_pending_ = false;
    return Resolve(event);
}

Action NavModel::OnRefreshIdle() {
    ctx_.refresh_busy = false;
    if (!has_pending_) return Make(ActionType::kNone);
    has_pending_ = false;
    return Resolve(pending_);
}

const char* PageName(Page page) {
    switch (page) {
        case Page::Dashboard: return "Dashboard";
        case Page::Gallery:   return "Gallery";
        case Page::Settings:  return "Settings";
        case Page::Transfer:  return "Transfer";
    }
    return "Unknown";
}

const char* EventName(Event event) {
    switch (event) {
        case Event::kUpClick:   return "UpClick";
        case Event::kDownClick: return "DownClick";
        case Event::kBootClick: return "BootClick";
        case Event::kUpLong:    return "UpLong";
        case Event::kDownLong:  return "DownLong";
        case Event::kBootLong:  return "BootLong";
        case Event::kComboLong: return "ComboLong";
    }
    return "Unknown";
}

const char* ActionString(const Action& action, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return out;
    const char* base = "none";
    bool with_page = false;
    bool with_delta = false;
    switch (action.type) {
        case ActionType::kNone:               base = "none"; break;
        case ActionType::kSwitchPage:         base = "switch_page"; with_page = true; break;
        case ActionType::kListMove:           base = "list_move"; with_delta = true; break;
        case ActionType::kSelect:             base = "select"; break;
        case ActionType::kOpenQuickSwitch:    base = "open_quick_switch"; break;
        case ActionType::kCloseQuickSwitch:   base = "close_quick_switch"; break;
        case ActionType::kQuickSwitchMove:    base = "quick_switch_move"; with_delta = true; break;
        case ActionType::kQuickSwitchConfirm: base = "quick_switch_confirm"; break;
        case ActionType::kDialogMove:         base = "dialog_move"; with_delta = true; break;
        case ActionType::kDialogConfirm:      base = "dialog_confirm"; break;
        case ActionType::kCloseDialog:        base = "close_dialog"; break;
        case ActionType::kCloseFullscreen:    base = "close_fullscreen"; break;
        case ActionType::kExitTransfer:       base = "exit_transfer"; with_page = true; break;
        case ActionType::kHome:               base = "home"; break;
        case ActionType::kRepaintPage:        base = "repaint_page"; break;
        case ActionType::kWifiConfigAp:       base = "wifi_config_ap"; break;
        case ActionType::kPttArm:             base = "ptt_arm"; break;
        case ActionType::kFeedbackNoop:       base = "feedback_noop"; break;
        case ActionType::kPendingLatched:     base = "pending_latched"; break;
    }
    if (with_page) {
        snprintf(out, cap, "%s:%s", base, PageName(action.page));
    } else if (with_delta) {
        snprintf(out, cap, "%s:%+d", base, action.delta);
    } else {
        snprintf(out, cap, "%s", base);
    }
    return out;
}

StatusLines StatusLine(const StatusInputs& in) {
    // Ordered by what blocks what. The first unmet precondition is the one
    // worth telling the user about; listing all of them at once turns the
    // screen into a diagnostic dump.
    if (in.transfer_active) {
        return {"Photo transfer is running.", kExitHint};
    }
    if (!in.wifi_connected) {
        return {"Waiting for Wi-Fi.",
                "Configure Wi-Fi from the settings menu."};
    }
    if (!in.paired) {
        // Deliberately not offering a network pairing route: the operator has
        // to be holding the device (docs/PROVISIONING.md).
        return {"Not paired yet.",
                "Open Settings and choose \"Pair dashboard\" on this device."};
    }
    if (!in.has_stored_frame) {
        return {"Paired. Waiting for the first frame.",
                "The Mac pushes frames. This device never fetches them."};
    }
    if (!in.lan_service_running) {
        return {"Dashboard image stored on the device.",
                "LAN service is off, so no new frame can arrive."};
    }
    return {"Dashboard image stored on the device.",
            "LAN service is on and ready to receive."};
}

bool SlideshowIsRunning(const SlideshowInputs& in) {
    if (in.interval_minutes <= 0) return false;
    if (!in.on_gallery_page) return false;
    if (!in.fullscreen) return false;
    if (in.dialog_open) return false;
    return true;
}

}  // namespace nav
