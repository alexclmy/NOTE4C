#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "audio_service.h"
#include "audio/earcon_player.h"
#include "common/audio_fsm.h"
#include "common/device_config.h"
#include "common/nav_model.h"
#include "common/autonomy_cycle.h"
#include "common/autonomy_service.h"
#include "common/autonomy_status.h"
#include "common/dashboard_manager.h"
#include "common/weather_cache.h"
#include "common/power_policy.h"
#include "common/voice_capture.h"
#include "common/voice_uploader.h"
#include "device_state.h"

namespace ui {
class RawDrawUiManager;
}

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Initialize();
    void Run();

    DeviceState GetDeviceState() const { return state_.load(std::memory_order_acquire); }
    bool SetDeviceState(DeviceState state);

    void Schedule(std::function<void()>&& callback);
    void PlaySound(const std::string_view& sound);
    void PlaySound(const std::string_view& sound, int duration_ms);
    void MuteSound();
    void StopSound();
    bool CanEnterSleepMode() const;

    AudioService& GetAudioService() { return audio_service_; }
    ui::RawDrawUiManager* GetRawDrawUiManager() { return rawdraw_ui_manager_.get(); }
    void UpdateStatusBarForUi();

    // ---- hybrid low power --------------------------------------------------
    //
    // The policy lives in common/power_policy.h and is host tested. What is
    // here is the glue that reads a clock, writes NVS and calls esp_sleep.

    /// Everything the status route reports under "power". Safe from any task.
    power::PowerStatus PowerSnapshot() const;
    /**
     * @brief Apply a power mode that came from the config API or the menu.
     *
     * @param mode             one of the three power::ModeName strings.
     * @param interactive_min  window length to use when @p mode is interactive.
     *
     * Only ever reaches an awake device, which is the whole reason the tower
     * has to hold an intent for a sleeping one.
     */
    void ApplyPowerMode(const std::string& mode, int32_t interactive_min);
    /// Set the auto-saver wake interval, in minutes, and persist it.
    void SetWakeIntervalMinutes(int32_t minutes);

    // ---- autonomy ----------------------------------------------------------
    //
    // The profile store and its wire rules live in common/autonomy_service.h
    // and are host tested; what is here is ownership. Null when the store could
    // not be brought up — no SPIFFS, no PSRAM for the 18 KB profile — in which
    // case the routes answer 404 `autonomy_unsupported` and the device is an
    // ordinary push target, which is exactly what it was before this feature.
    autonomy::AutonomyService* autonomy_service() { return autonomy_service_; }
    /// Everything the status route reports under "autonomy". Safe from any task.
    autonomy::AutonomyStatus AutonomySnapshot() const;

    /**
     * @brief What one local composition did.
     *
     * Richer than the wake cycle needs, because the render route reports the
     * arbitration outcome rather than just an HTTP code — and the two share one
     * compose so that what the route describes is what the cycle would draw.
     */
    struct LocalComposeReport {
        /// Why it could not even start. Null when the compose ran.
        const char* refusal = nullptr;
        bool composed = false;
        uint8_t modules_drawn = 0;
        bool empty_panel = false;
        /// True when a cached forecast was actually drawn from.
        bool had_forecast = false;
        /// True when this panel could not show everything the profile asks for.
        bool degraded = false;
        uint32_t expected_seq = 0;
        autonomy::ComposeOutcome outcome = autonomy::ComposeOutcome::kNotTried;
        dashboard::PushStatus push;
    };

    /**
     * @brief Compose one panel now, for the authenticated render route.
     *
     * Pull, not schedule: this is the path an operator drives from a bench. The
     * wake cycle reaches the same compositor by its own route and neither can
     * run while the other is — see compose_mutex_.
     */
    LocalComposeReport ComposeLocalFrameNow();
    /**
     * @brief Set how long the *next* interactive window will be.
     *
     * Separate from ApplyPowerMode because it is a different request: it must
     * not close a window that is currently open, which is what routing it
     * through a mode change used to do.
     */
    void SetInteractiveMinutes(int32_t minutes);
    /// Record how the last update cycle went. Drives the retry schedule.
    void NoteCycleOutcome(power::CycleOutcome outcome);

    void OnUpClick();
    void OnDownClick();
    void OnUpLongPress();
    void OnDownLongPress();
    void OnWifiConfigComboLongPress();
    void OnBootClick();
    void OnBootLongPress();
    /**
     * @brief The BOOT key went down. Arms push-to-talk.
     *
     * Arming here rather than at the driver's 1000 ms long press is what makes
     * hold-to-talk possible at all: the microphone opens at the state
     * machine's own 300 ms threshold instead of 1300 ms after the finger
     * landed. Every BOOT press reaches this, including the ones that turn out
     * to be clicks; a press that does not cross the threshold is a tap and the
     * machine returns to Idle without opening anything.
     */
    void OnBootPressed();
    /**
     * @brief The BOOT key was released.
     *
     * @return true when the press was consumed by push-to-talk, in which case
     *         the board must swallow the click the driver is about to deliver.
     *         Without that, a 500 ms hold on the Dashboard would record an
     *         utterance and also quick-switch the page.
     */
    bool OnBootReleased();

    /// Wi-Fi came up or went down. Push-to-talk needs to know: with no network
    /// there is nowhere to send an utterance, and it says so rather than
    /// recording one anyway.
    void RefreshVoiceTransportReadiness();

    /**
     * @brief Install the hub address and token, or clear them with two empty
     *        strings.
     *
     * Called by POST /api/v1/voice/hub, which is authenticated with the
     * dashboard token. Persists to NVS and updates the transport gate.
     *
     * Configuring a hub enables nothing on its own. The software mute is a
     * separate setting, it defaults to on, and it is what actually lets the
     * microphone open.
     */
    void ConfigureVoiceHub(const std::string& url, const std::string& token);

    /**
     * @brief Replace the hub base URL, keeping whatever token is stored.
     *
     * The config API can write the address but never the token, so this is the
     * shape that write needs. An empty URL clears the pair outright.
     */
    void SetVoiceHubUrl(const std::string& url);

    /**
     * @brief Start or stop the LAN HTTP service.
     *
     * @return the state the device is actually in afterwards. Asking for the
     *         server without Wi-Fi or without an address does not start one,
     *         and the caller has to be able to say so rather than report the
     *         value it asked for.
     */
    bool SetLanService(bool enabled);

private:
    Application();
    ~Application();

    std::atomic<DeviceState> state_{kDeviceStateUnknown};
    std::atomic<bool> wifi_connected_{false};
    AudioService audio_service_;
    std::unique_ptr<ui::RawDrawUiManager> rawdraw_ui_manager_;
    esp_timer_handle_t sleep_timer_ = nullptr;

    /**
     * @brief The live power state, and the counters the retry schedule needs.
     *
     * Guarded by power_mutex_ because four tasks reach it: the httpd task
     * rendering the status route, the httpd task applying a config patch, the
     * button task opening a window, and the Run() loop ticking the expiry.
     * power_policy.h has no internal locking by design, being portable code
     * the host tests drive from one thread.
     */
    mutable std::mutex power_mutex_;
    /**
     * @brief Serialises ServiceWakeCycle between the Run() loop and the
     *        esp_timer task the backstop fires on.
     *
     * Distinct from power_mutex_ and not a substitute for it: that one guards
     * short field accesses and is taken and released repeatedly inside a single
     * advance, while this one spans the whole advance — including the seconds a
     * bounded fetch blocks for — and is therefore never waited on. See
     * power::CycleAdvanceGate for why refusing beats waiting here.
     */
    power::CycleAdvanceGate cycle_advance_gate_;
    power::PowerState power_state_;
    power::CycleOutcome last_cycle_outcome_ = power::CycleOutcome::kUpdated;
    /// False until a cycle has actually finished. The status route reports
    /// null rather than naming an outcome no cycle produced.
    bool has_cycle_outcome_ = false;
    uint32_t consecutive_failures_ = 0;

    /**
     * @brief The bounded wake cycle, and the state that drives it.
     *
     * These are what make the two-minute guarantee real rather than a comment.
     * `wake_budget_` is started at the top of every auto-saver wake and
     * consulted by ServiceWakeCycle() on the one-second tick; `net_recovery_`
     * bounds the one part of the cycle worth retrying. Before they were wired
     * here the only thing that ever put an auto-saver device to sleep was the
     * legacy `sync.interval` timer, which meant half an hour awake per wake
     * and nothing at all when that key was zero.
     */
    power::WakeBudget wake_budget_;
    power::NetworkRecovery net_recovery_;

    /**
     * @brief The autonomy profile store, and the memory it borrows.
     *
     * All three live for the life of the device and all three are in PSRAM,
     * because together they are about 35 KB and none of it belongs in internal
     * SRAM: the profile's own arena is 17 KB, the slot scratch is another 16,
     * and the device has 8 MB of PSRAM doing nothing.
     *
     * `autonomy_service_` is null when the store could not be brought up. That
     * is a supported state, not a failure to paper over: the routes answer 404
     * and the device behaves exactly as it did before this feature existed.
     */
    autonomy::AutonomyService* autonomy_service_ = nullptr;
    autonomy::Profile* autonomy_profile_ = nullptr;
    uint8_t* autonomy_scratch_ = nullptr;
    record::SlotIo* autonomy_slot_io_ = nullptr;

    /**
     * @brief The forecast cache, and the buffers the compositor draws into.
     *
     * All in PSRAM, and the canvas is the reason why: a byte per pixel of a
     * 400x300 panel is 120 KB, which is more internal SRAM than this device
     * has to spare. Allocated once for the life of the device rather than per
     * wake, because a heap that has to find 120 KB contiguous after an hour of
     * fragmentation is a wake that fails for no reason anyone could diagnose.
     *
     * Null when PSRAM could not provide them, in which case the device composes
     * nothing and stays an ordinary push target. That is a supported state.
     */
    weather::WeatherCache* weather_cache_ = nullptr;
    uint8_t* weather_scratch_ = nullptr;
    record::SlotIo* weather_slot_io_ = nullptr;
    uint8_t* compose_canvas_ = nullptr;
    uint8_t* compose_frame_ = nullptr;
    /**
     * @brief One composition at a time: the 120 KB canvas has a single owner.
     *
     * Two callers reach the compositor — the wake cycle, and the authenticated
     * render route on the HTTP task — and they run on different cores. Both
     * try-lock and neither waits: a wake-cycle tick must not be parked behind
     * an operator's bench compose, and an operator must be told "busy" rather
     * than left holding a socket open for two seconds of somebody else's CPU.
     *
     * Distinct from cycle_advance_gate_, which guards the whole wake advance,
     * and from power_mutex_, which guards short field reads.
     */
    std::mutex compose_mutex_;

    /**
     * @brief What this wake's content cycle has done so far.
     *
     * Reset by BeginWakeCycleLocked when a *new* wake starts, because "did we
     * already fetch" is a question about this wake and nothing else. Carrying
     * it across wakes would mean a device that failed one fetch never trying
     * again — and, just as importantly, *not* carried away by the budget
     * restart that follows a deferral, which is a different event and used to
     * clear all of this. A slideshow left running would otherwise have made the
     * device fetch the forecast again every two minutes for as long as it ran.
     */
    bool cycle_fetch_attempted_ = false;
    bool cycle_fetch_ok_ = false;
    autonomy::ComposeOutcome cycle_compose_result_ =
        autonomy::ComposeOutcome::kNotTried;
    bool cycle_degraded_ = false;
    autonomy::FetchStatus cycle_fetch_status_;
    /**
     * @brief The sequence the store held when this wake's compose began.
     *
     * The expected half of SubmitLocal's compare-and-swap, and separately the
     * way a locally stored frame is told apart from a pushed one: a local
     * submit moves `stored_seq` too, and treating that as "the tower sent
     * something" would have the device report its own frame as the operator's.
     */
    uint32_t cycle_compose_expected_seq_ = 0;
    uint32_t cycle_local_seq_ = 0;
    /// What the planner last decided, kept so FinishWakeCycle can record one
    /// truthful account of the cycle from one place. See CommitAutonomyRecord.
    autonomy::CycleDecision cycle_decision_;
    bool cycle_autonomy_participated_ = false;
    /// What the last *content* cycle did, which is a different question from
    /// what the last power cycle did. See autonomy_status.h.
    autonomy::CycleStatus autonomy_cycle_;
    bool cycle_active_ = false;
    /// True while something physical is keeping the device up. Kept so the
    /// deferral is logged once rather than once a second.
    bool cycle_deferred_ = false;
    power::WakePhase cycle_phase_ = power::WakePhase::kNetwork;
    int64_t cycle_phase_started_ms_ = 0;
    /// Earliest monotonic ms at which the next association attempt may start.
    int64_t net_retry_at_ms_ = 0;
    /// Dashboard counters as they stood when the cycle began, so "did anything
    /// change" is a comparison rather than a guess.
    bool cycle_baseline_taken_ = false;
    uint32_t cycle_start_render_count_ = 0;
    uint32_t cycle_start_stored_seq_ = 0;
    uint32_t cycle_start_failed_renders_ = 0;
    /**
     * @brief A refresh started during this wake and did not reach the glass.
     *
     * Kept apart from cycle_frame_drawn_ because "nothing was drawn" and "the
     * draw was refused" are different cycles. The first is the cheap, correct
     * outcome the whole skip path exists to produce; the second leaves the
     * store holding a frame the panel has never shown, and used to be recorded
     * as the first — which told PlanNextWake the wake had succeeded and put the
     * next attempt a full interval away.
     */
    bool cycle_render_failed_ = false;
    /// A frame arrived from the *tower* during this wake.
    bool cycle_saw_new_frame_ = false;
    /// The panel actually completed a refresh during this wake. The only
    /// evidence this device has that anything reached the glass, and a
    /// different question from the one above — they used to share a field.
    bool cycle_frame_drawn_ = false;
    /**
     * @brief An outcome this cycle has already earned, whatever happens next.
     *
     * Set by the offline path: a wake that never reached the network is a
     * failed wake, and it still has to converge through the render and settle
     * phases when it drew something from its cache on the way out. Without
     * this, the settle phase would report that update as a success and the
     * retry backoff would never apply.
     */
    bool cycle_forced_outcome_known_ = false;
    power::CycleOutcome cycle_forced_outcome_ = power::CycleOutcome::kUnchanged;
    /// Monotonic milliseconds at which the armed timer wake will fire, or 0
    /// when no sleep is scheduled. Reported as a countdown, never as a promise.
    int64_t next_wake_at_ms_ = 0;
    bool sleep_intent_ = false;
    /**
     * @brief Timer that advances the push-to-talk timeouts.
     *
     * 25 ms. It used to be the once-a-second loop in Run(), which could not
     * honour a 300 ms arm threshold to better than a second. The callback
     * ticks the state machine and does nothing else; the machine never touches
     * the display, so a panel refresh taking minutes cannot delay it.
     */
    esp_timer_handle_t ptt_tick_timer_ = nullptr;
    /**
     * @brief One-shot timer for a remotely requested restart or sleep.
     *
     * The delay is what lets the HTTP response reach the socket before the
     * device stops being a device. The handler that answered the request is
     * still holding that socket when the timer is armed.
     */
    esp_timer_handle_t action_timer_ = nullptr;
    devcfg::Action pending_action_ = devcfg::Action::kRestart;
    /// Push-to-talk ladder. Reachable in every build; with VOICE_PTT_ENABLED
    /// off it can never leave Muted, so no microphone is ever opened.
    audio_ui::AudioFsm ptt_fsm_;
    /**
     * @brief Guards ptt_fsm_. Six tasks reach it.
     *
     * The button task presses and releases it, the 25 ms esp_timer task ticks
     * it, the Wi-Fi event task changes its transport gate, the capture task
     * delivers an utterance, the upload task delivers an answer, and the UI
     * task toggles the mute. The machine has no internal locking, by design:
     * it is portable C++ that host tests drive from one thread.
     *
     * Every hook the machine calls while this is held is non-blocking, and
     * none of them calls back into the machine, so this cannot deadlock.
     */
    std::mutex ptt_mutex_;
    EarconPlayer earcon_player_;
    voice::VoiceCapture voice_capture_;
    voice::VoiceUploader voice_uploader_;

    /**
     * @brief Hand the current settings and the apply hooks to the config
     *        service, which owns them from then on.
     *
     * Called once, after the Settings rows are built, because the values it
     * adopts are the same ones those rows were built from. Two readers of the
     * same NVS keys would be two chances to disagree about a default.
     */
    void InitializeConfigService(int slideshow_interval, bool muted);
    /// Refresh every value the About surface reports. Called from
    /// UpdateStatusBarForUi(), which is where every change already lands.
    void UpdateAboutInfo(const std::string& lan_ip);
    /// Arm the deferred restart or sleep the config API requested.
    void ScheduleDeviceAction(devcfg::Action action, uint32_t delay_ms);

    /// Restart the budget without clearing what this wake has already done.
    void RestartWakeBudgetLocked(int64_t now_ms);

    void ArmSyncSleepTimer();
    void EnterScheduledSleep();
    void EnterManualSleep();

    /// Read NVS and the deep-sleep wake cause, and seed power_state_.
    void InitializePowerState();
    /// Bring the profile store up, or leave autonomy inert. Never fails hard.
    void InitializeAutonomy();
    /**
     * @brief Run one tick of the autonomy half of the fetch phase.
     *
     * @return what the planner decided, after executing it. **Never ends the
     *         cycle.** A step that finds autonomy switched off, unprofiled or
     *         with nothing to draw stands down and leaves the caller's ordinary
     *         phase timing — the ninety-second push rendezvous, the network
     *         failure semantics — exactly as it was before this feature
     *         existed.
     *
     * @param tower_wait_remaining_ms what is left of the profile's tower wait.
     */
    autonomy::CycleDecision ServiceAutonomyStep(int64_t now_ms, bool network_up,
                                                uint32_t tower_wait_remaining_ms);
    /// Collect everything PlanCycleStep needs from the live device.
    autonomy::CycleInputs CollectCycleInputs(bool network_up,
                                             uint32_t tower_wait_remaining_ms,
                                             uint32_t work_remaining_ms) const;
    /// One bounded forecast fetch, straight into the cache. Never throws, never
    /// retries, and always leaves cycle_fetch_attempted_ true.
    /// @param budget_ms the whole-operation deadline the planner allocated.
    void RunWeatherFetch(uint32_t budget_ms);
    /// Compose from the profile, the cache and the clock, and store the frame
    /// if it differs from what is displayed. Records the outcome in
    /// cycle_compose_result_, which is what the planner reads back.
    void RunLocalCompose();
    /// The composition itself. Caller holds compose_mutex_; both entries do.
    LocalComposeReport ComposeLocked(bool degraded);
    /**
     * @brief Write one truthful account of this wake into autonomy_cycle_.
     *
     * Called from FinishWakeCycle and from nowhere else, so every way a cycle
     * can end — the autonomy path, the render phase, the settle phase, the
     * budget backstop — records the same thing. An earlier revision recorded it
     * only on the autonomy path, which meant the common case, where a composed
     * frame went on to be drawn by the render phase, recorded nothing at all.
     *
     * @param outcome what the *power* cycle concluded, which constrains what
     *        the content cycle may claim: a frame that was never drawn is not
     *        an update, whatever the compositor thought.
     */
    void CommitAutonomyRecord(power::CycleOutcome outcome);
    /// UTC seconds, or 0 when SNTP has never succeeded on this boot.
    static int64_t NowEpochOrZero();
    /// Gather the inputs PlanNextWake needs from the live device.
    power::WakeInputs CollectWakeInputs() const;
    /**
     * @brief Stop the radio and enter deep sleep, armed to come back.
     *
     * Both wake sources are enabled every time: the timer, so the panel is
     * refreshed on schedule, and the BOOT button, so a person can always get
     * the device back immediately. Nothing on the network can.
     */
    void EnterHybridSleep(uint32_t wake_in_ms, const char* reason);
    /**
     * @brief A person touched the device: open or refresh an interactive window.
     *
     * Called from DispatchNavEvent, so every physical gesture counts exactly
     * once whatever page it lands on. Does not persist anything and does not
     * move the config revision: a window is live state with a deadline, and
     * bumping the compare-and-swap counter on every button press would hand
     * the tower a revision conflict whenever somebody walked past the device.
     */
    void NotePhysicalActivity();
    /// Fold the interactive window's expiry in, then advance the wake cycle.
    /// Runs on the one-second loop in Run().
    void PowerTick();

    // ---- the bounded wake cycle -------------------------------------------

    /// Start (or restart) the budget and the retry state. Holds power_mutex_.
    void BeginWakeCycleLocked(int64_t now_ms);
    /// Move to @p phase and reset its clock. Holds power_mutex_.
    void EnterCyclePhaseLocked(power::WakePhase phase, int64_t now_ms);
    /**
     * @brief Advance the cycle by one tick: check the budget, run the phase.
     *
     * Every path out of here either continues the cycle or calls
     * FinishWakeCycle, which always ends in sleep. That is the guarantee the
     * hybrid design rests on.
     */
    void ServiceWakeCycle(int64_t now_ms);
    /// The bounded association retry. Gives up and sleeps when the budget says.
    void ServiceNetworkRecovery(int64_t now_ms);
    /// Record the outcome once, then sleep. Idempotent: the tick and the
    /// backstop timer can both reach it.
    void FinishWakeCycle(power::CycleOutcome outcome);
    /// esp_timer callback: the tick did not end the cycle in time, so end it.
    void OnWakeBudgetBackstop();
    /// Persist the base mode and the two intervals.
    void PersistPowerSettings();
    void EnterWifiConfigMode();

    /// Feed one physical gesture to the navigation model (main/common/nav_model.h).
    void DispatchNavEvent(nav::Event event);
    /// Redraw the frame already stored on the device. Never a fetch.
    void RepaintStoredDashboardFrame();
    /// The reserved BOOT-long gesture. A fallback only: the press-down path
    /// has normally armed the machine several hundred milliseconds earlier.
    void OnPushToTalkGesture();
    void InitializeAudioFsm();
    /// Start the 25 ms timer that advances the push-to-talk timeouts.
    void StartPttTickTimer();
    /// The capture task finished draining one utterance.
    void OnUtteranceCaptured(voice::CaptureResult&& capture);
    /// The uploader finished, however it finished.
    void OnUploadFinished(const voice::UploadResult& result);
    /// Set the software mute and persist it to NVS. The Settings row and the
    /// boot-time restore are the only callers.
    void SetVoiceMuted(bool muted);
    /// Back and Home must also leave the Wi-Fi setup access point, which is
    /// shown on the same page as photo transfer. No-op otherwise.
    void LeaveWifiConfigApIfActive();
};

#endif  // _APPLICATION_H_
