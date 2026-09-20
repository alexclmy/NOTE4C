# Hybrid low power: the design, and what has not been checked on hardware

The goal is one sentence: **maximise battery life while keeping an automatic
hourly refresh.** Everything below follows from that sentence and from one
physical fact that no amount of software can argue with.

> A device in deep sleep has its Wi-Fi radio powered down. Nothing on the
> network can wake it. There is no socket listening, and no packet that changes
> that.

Every honest design for this product is shaped by that fact, and every
dishonest one hides it.

---

## Where the code lives

| Concern | File | Tested by |
|---|---|---|
| The whole policy: modes, wake planning, the bounded budget, the battery gate | `main/common/power_policy.h/.cc` | `tests/host/test_power_policy.cc` |
| Ownership of the battery ADC: one open, one lock, one close, one cache | `main/common/battery_monitor.h/.cc` | `tests/host/test_battery_monitor.cc` |
| The autonomy half of the fetch phase: sub-phase budgets, every terminal step | `main/common/autonomy_cycle.h/.cc` | `tests/host/test_autonomy_cycle.cc` |
| Whether a local render may replace what is on the glass | `main/common/autonomy_policy.h/.cc` | `tests/host/test_autonomy_policy.cc` |
| The forecast fetch's whole-operation deadline | `main/common/http_deadline.h`, `bounded_connect.h`, `openmeteo_client.h/.cc` | `test_http_deadline.cc`, `test_bounded_connect.cc`, `test_openmeteo_client.cc` |
| The v2 config fields `power.*` | `main/common/device_config.h/.cc` | `tests/host/test_device_config.cc` |
| Patch dispatch and the `power.*` hooks | `main/common/device_config_service.cc` | `tests/host/test_device_config_service.cc` (via `tests/host/shims/`) |
| Device glue: `esp_sleep`, NVS, Wi-Fi, the wake cause, **the wake cycle** | `main/application.cc` | not host testable; see validation below |
| The `power` block on the status route | `main/ui/renderers/rawdraw/dashboard_api.cc` | the renderer is host tested; the route is not |
| Tower model and the pending intent | `note4c-control-tower/src/core/power.ts` | `tests/unit/power.test.ts` |
| Tower delivery against a device that sleeps | `note4c-control-tower/src/server/device/powerIntent.ts` | `tests/unit/powerIntent.test.ts` |
| Which tower routes may deliver an intent | `note4c-control-tower/app/api/device/power/route.ts`, `src/server/refreshScheduler.ts` | `tests/unit/powerRoutes.test.ts` |

`power_policy.h` contains no ESP-IDF header, on purpose and for the same reason
`device_config.h` does not: the host tests compile the exact translation unit
the firmware links, rather than a parallel model of it.

---

## The three modes

| Mode | Wire name | Behaviour | Persisted |
|---|---|---|---|
| Automatic power saving | `auto_saver` | Wake on a timer, one bounded update cycle, sleep again. The default. | yes |
| Interactive, temporarily | `interactive` | Stay awake and serve the API until the window expires, then fall back to `auto_saver` on its own. 5/15/30/60 minutes. | **no** |
| Always awake | `always_on` | Never sleeps. Two orders of magnitude more current. | yes |

**`interactive` is never persisted.** A window that survived a reboot would be
a window nobody opened, so `PowerState::PersistableMode()` maps it to
`auto_saver` and `Init()` refuses to restore it even if a value somehow reached
NVS. The expiry is the feature: a mode you have to remember to turn off is a
mode you forget to turn off, which is the same outcome as having no low-power
mode at all.

### What opens an interactive window

1. **A button wake from deep sleep**, or a power-on. Somebody is standing in
   front of the device; coming straight back up and sleeping again in their
   hand is the behaviour that makes a device feel broken.
2. **Any physical gesture while awake** (`Application::NotePhysicalActivity`,
   called from `DispatchNavEvent`). Without it the sleep timer fires
   mid-navigation and puts the device away while it is being used.
3. **A `power.mode` write over the API**, which only ever reaches an awake
   device.

A *timer* wake deliberately does not open one. Nobody is there, and staying
awake would defeat the wake it just performed.

---

## The bounded wake budget

A wake cycle is a sequence of things that can each hang: association, a DHCP
lease, a TCP connect, a 15 kB frame, a panel refresh waiting on a BUSY pin.
Giving each one its own timeout does not bound the total, because the timeouts
add up: the worst case becomes the sum of six timeouts, which is minutes on a
battery budgeted for seconds.

`WakeBudget` inverts that. There is one total, every phase asks it how much time
is left, and no phase can be granted time the total does not have:

```
RemainingFor(phase, now) == min(phase cap, total remaining)
```

The worst case is therefore the total, by construction rather than by hope.
`test_no_sequence_of_phases_can_outlive_the_total` walks all 24 orderings of
the four phases, spending each grant in full, and asserts the invariant before
every single grant.

Defaults: 165 s total; network 45 s, fetch 90 s, render 45 s, settle 8 s. The
per-phase caps deliberately sum to more than the total — a phase that finishes
early donates its unused time to the ones after it, while the total stays the
guarantee.

The **fetch cap is the tower rendezvous**: how long the device holds the door
open on a timer wake for the Control Tower to push a new frame before it sleeps.
The tower's refresh scheduler pulses every 30 s, so a rendezvous shorter than
one pulse makes delivery a coin flip — which is exactly what the earlier 40 s
window was. It is now **90 s** (default profile `tower_wait_s` 80 s, the fetch
cap is the ceiling), comfortably longer than one pulse, so a scheduled push
reliably lands inside the awake window and is rendered before sleep. The total
was raised 120 → 165 s so the widened rendezvous still leaves the full
render + settle reserve (53 s) intact; no phase exceeds the total, and
`test_no_sequence_of_phases_can_outlive_the_total` still holds at the new
numbers.

**Battery cost.** The knob that actually costs current is the awake time per
wake, and it roughly doubles: the tower-wait dominates the cycle, so a timer
wake that used to hold the radio open ~40 s now holds it ~80–90 s, and the whole
awake cycle grows from ~1 min to ~2 min. With Wi‑Fi associated and the CPU up
the device draws on the order of 80–120 mA, so each wake costs very roughly
~1.5–3 mAh more than before (≈ (50 s ÷ 3600) × 100 mA). Against the fridge's
15–30 min wake interval that is a handful of extra mAh per hour — a real but
modest cost, paid to turn frame delivery from a coin flip into a near‑certainty.
The exact figure is **unmeasured** and belongs on the bench; the arithmetic here
is the order of magnitude, not a measurement.

### Where the budget is actually driven

`Application::ServiceWakeCycle()`, from the one-second tick in `Run()`. The
budget is started in `InitializePowerState()` — at the *top* of the wake, not
after initialisation finishes, because the total it guarantees is the total
time the device is awake. A wake that spent ninety seconds mounting SPIFFS has
ninety seconds less to spend on the network, and that is the honest accounting.

The cycle walks network → fetch → render → settle and ends by calling
`FinishWakeCycle`, which records the outcome through `NoteCycleOutcome` and
sleeps. Every exit is one of:

| What happened | Outcome recorded |
|---|---|
| A frame arrived and the panel was refreshed | `updated` |
| Reached the network, nothing new to draw | `unchanged` |
| Never associated, retries exhausted or unaffordable | `network_failed` |
| The total ran out part way through | `budget_exhausted` |
| A refresh was asked for and did not reach the glass | `render_failed` |

`render_failed` is a failure, and the distinction from `unchanged` is the whole
reason it exists. A panel that never releases BUSY leaves the store holding a
frame nobody has seen. Recording that as `unchanged` told `PlanNextWake` the
wake had succeeded, which reset the failure counter and put the next attempt a
full wake interval away: a pushed frame then sat in flash for an hour with the
panel showing something else and nothing in the status route saying why. It now
earns `RetryDelayMs` like any other failure, and the plan's reason is
`retry_after_render_failure` rather than `retry_after_network_failure`, because a
device whose router is fine should not send anybody looking at the router.

One case that *looks* like a render failure is not one: a frame that arrives
while the user is on another page is a benign, expected refusal. The painter now
reports it as `deferred` rather than `failed` (see `RenderOutcome::kDeferred`
and `DASHBOARD_API.md`), so it does not increment `refresh.failed`, does not mark
the cycle `render_failed`, and does not earn a failure backoff. A timer wake
draws the dashboard on the dashboard page, so this case does not arise during an
unattended wake at all; it is what happens when somebody is standing at the
device reading a different page, and treating that as a fault was wrong.

The matching evidence is `refresh.failed` / `refresh.last_failed_seq` on the
dashboard status route; see `DASHBOARD_API.md`.

A second bound exists as a backstop: `ArmSyncSleepTimer()` arms an `esp_timer`
for whatever is left of the total, so the cycle still ends in sleep if the
one-second tick is starved by a long panel refresh.

**Two tasks can therefore be inside the cycle at once**, and that is why entry is
a compare-exchange (`power::CycleAdvanceGate`). The `Run()` loop ticks the cycle
once a second and the backstop calls the same function from the `esp_timer`
task — and "the loop is starved" and "the loop is busy" are not the same thing.
A `Run()` loop blocked inside a bounded forecast fetch is *inside* the cycle,
holding no lock, with the backstop free to enter behind it. The gate refuses
rather than waits: waiting would park the `esp_timer` task behind a fetch for as
long as the fetch runs, and that task is the one that has to end the cycle if the
fetch never returns. A refused tick costs a second, and the backstop re-arms.

Three things suspend the cycle rather than failing it — a refresh in flight,
the provisioning portal, a running slideshow. `PlanNextWake` refuses to sleep
through any of them, so the budget is *restarted* instead of being allowed to
expire: a cycle that ran out while the device was required to be awake would
record a failure nothing did wrong and back the wake interval off for it. What
the restart does **not** clear is what the wake has already done — the fetch it
made, the frame it composed. Clearing those meant a slideshow left running had
the device refetch the forecast every two minutes for as long as it was on.

"A running slideshow" means one that is actually advancing pictures: the gallery
page, full-screen, with an interval set and no dialog over it
(`nav::SlideshowIsRunning`). It used to mean "an interval is configured", and the
shipped default interval is five minutes — so every auto-saver device sitting on
its dashboard answered yes on every tick and never entered deep sleep at all.
The device stayed up until the battery was flat and then went quiet, which from
the tower looks like a device that failed rather than one that was never allowed
to sleep.

A button press or an API mode change stands the cycle down entirely. Somebody
is there, and the device is theirs until the window expires.

### Network recovery

`NetworkRecovery` retries association up to four times with a doubling backoff
(2 s, 4 s, 8 s, capped at 16 s), **and the budget has the last word**. An
attempt is only started if the remaining budget covers the backoff *and* a
five-second attempt after it. Otherwise the device stops and sleeps: spending
the rest of the cycle on an attempt that cannot finish leaves a stale panel and
a flatter battery than doing nothing.

### Retry after a failed cycle

A failed cycle schedules a *shorter* wake — 10 minutes first, doubling, capped
at the normal interval. An hour is a long time to hold a stale panel because a
router was rebooting; but a device whose network is genuinely gone must not
spend the day waking every ten minutes to fail. The failure count is persisted
in NVS (`power/fails`), because deep sleep clears RAM and a counter that lived
only in memory would make every wake look like the first failure.

---

## The autonomy path, and why it does not widen the budget

`ServiceWakeCycle`'s fetch phase now has a second job: when the tower has had
whatever turn the profile gives it and said nothing, the device may compose a
panel for itself. That path is inside the existing 165 s total and **adds no new
exit from the cycle** — every terminal step still goes through
`FinishWakeCycle`, and `test_autonomy_cycle.cc` sweeps the input space asserting
exactly that.

The sequencing is a pure function in `main/common/autonomy_cycle.cc`, and it
exists so the bounded-power rules can be checked without a board. One rule
matters more than the rest:

> **No step that costs time is started without the budget to finish it.**

A forecast fetch is only begun when at least `kMinFetchBudgetMs` (24 s) remains
of `WakeBudget::WorkRemainingMs()` — the client's own ceiling is 20 s and the
rest covers the association check and the store. A compose is only begun with at
least `kMinComposeBudgetMs` (3 s). Below either threshold the honest answer is
to stand down, and standing down is what reaches deep sleep. Starting a
twenty-second fetch with four seconds left is how a device ends up held awake by
its own backstop every hour.

`WorkRemainingMs()` is the total still unspent **minus the render and settle
caps**, not the fetch phase's own cap. The render is part of the 165 s: a frame
stored at 118 s is a frame the panel begins drawing after the cycle should
already have ended.

### Autonomy stands down; it does not finish

A device with autonomy off, with no profile, or with a profile asking for
nothing must behave *exactly* as it did before this feature existed — which
means the planner answers `kStandDown` and the caller keeps its ordinary phase
timing, including the ninety-second rendezvous a queued tower push needs to land
in. An earlier revision answered "finish" instead, and the result was a device
that went back to sleep on the first tick of the fetch phase and never received
another push. The `participated` flag is what keeps the status route honest
about the difference between "autonomy took no part in this wake" and "autonomy
ran and found nothing to do".

### Two properties that lower the cost rather than raise it

- **A wake can skip the radio entirely.** In Device mode with a cached forecast
  not yet due for a refetch, `NeedsNetwork` is false and the cycle composes from
  flash and sleeps without ever associating. This is the largest saving the
  feature offers and it is, as ever, **unmeasured**.
- **A fetch is attempted at most once per wake.** A failure is not retried
  inside the same budget: whatever went wrong will still be wrong four seconds
  later, and the next wake is the retry with the backoff that already exists.

A wake that never gets Wi-Fi now runs the local path before sleeping — a
countdown and a cached forecast are still true, and that is the wake where
drawing them matters most, because the tower is unreachable and nobody else is
going to. It still ends as `network_failed`: the panel is better for it, the
retry schedule must not be.

### What "bounded" means for the fetch, precisely

This is the part most easily overclaimed, so it is stated as a chain rather than
as an adjective.

`RunBoundedGet` takes an absolute deadline at entry and clamps every blocking
call to what is left of it. That is necessary and **not sufficient**: it can only
check the clock *between* calls. Whether the operation is actually bounded
depends on each of the five `HttpOps` methods returning inside the budget it was
handed — and the obvious `esp_http_client` implementation does not.
`esp_http_client_fetch_headers` and `esp_http_client_read` (ESP-IDF v6.0) each
loop over `esp_transport_read`, granting it the full per-operation timeout every
time round, so one call can outlive any budget while a peer trickles bytes.

The device implementation earns the bound by enforcing an absolute deadline
*inside* its tcp_transport, where every byte has to pass
(`CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT=y`), and by resolving the name
itself — bounded — before connecting, because the `getaddrinfo` inside
esp-tls's connect consults no clock at all. Three suites divide the work and
none is sufficient alone: `test_openmeteo_client.cc` tests the arithmetic
against a fake that honours its budget by construction and *cannot* catch an
overrun; `test_http_deadline.cc` reimplements both IDF loops and drives a
trickling server past the old and the new transport, which is where the overrun
is actually demonstrated; `test_bounded_connect.cc` does the same for the
connect step against a reproduction of lwIP's asynchronous resolver contract,
with real threads, so the uncancellable late callback is exercised rather than
reasoned about.

**Still only compiled.** `openmeteo_transport_esp.cc` and the lwIP binding have
never executed. The SNI/CN behaviour on a literal address is established by
reading the SDK, not by an observed handshake. And `RunBoundedGet` can burn CPU
on repeated zero-length reads inside its deadline — documented and accepted,
because the deadline still ends it.

---

## The change that actually saves the battery

Before this work, `Application::ArmSyncSleepTimer` skipped arming whenever
`IsHttpServerRunning()` was true, and `SetLanService` stopped the timer when the
LAN API server started. That single predicate is why **a device with the tower
connected never slept at all**: turning the API on cancelled the sleep timer and
nothing re-armed it.

Both are changed. The API server no longer blocks sleep; only the **AP
provisioning portal** does, because somebody is standing there typing a Wi-Fi
password and sleeping would strand them. The device is now expected to sleep
with the tower configured, and the tower is built to expect an unreachable
device.

A third thing had to change with them. The sleep timer was armed from
`sync.sync_interval`, and that key was the *only* thing that ever put this
device to sleep. So even with the two fixes above, a timer wake stayed awake
for the whole sync interval — thirty minutes by default, against a wake cycle
that needs about three — and `sync.sync_interval = 0` meant "never sleep", which
silently disabled power saving entirely while the config, the status route and
the UI all reported `auto_saver`. In `auto_saver` the sleep decision no longer
consults that key at all: the awake period is the wake budget and the schedule
is `power.wake_interval_min`, which has a floor and cannot express "never".

Three things still outrank the mode, and all three are in `PlanNextWake`:

- a **panel refresh in flight** — e-paper holds whatever was on the glass when
  the power went, so sleeping mid-refresh leaves a half-drawn image;
- the **AP provisioning portal** open;
- a **gallery slideshow** running, which is mutually exclusive with sleep.

### Serving and modem sleep

Deep sleep is this product's power story. Modem sleep — the station dozing
between beacons while the CPU is up — is not, and while an HTTP server is
running it is actively harmful.

The AP provisioning path always knew this: `StartAccessPoint()` calls
`esp_wifi_set_ps(WIFI_PS_NONE)` before it serves. The LAN path did not, so a
station could associate at -36 dBm, take a DHCP lease, log `httpd_start`
success with 21 routes registered — and still refuse every inbound TCP
handshake. A dozing station advertising a ten-beacon listen interval (~1 s) is
one whose access point buffers frames for it, and consumer APs routinely fail to
deliver them inside a client's SYN-retransmit window.

So the rule is now one rule, in `power::ServingWifiPowerSave` where a host test
reads it back, and both paths map it onto the driver through
`ApplyServingPowerSave`:

- **serving HTTP → `WIFI_PS_NONE`**, logged as `LAN serving: wifi power save
  off`;
- **not serving → `WIFI_PS_MIN_MODEM`**, restored when the LAN server stops,
  because a fifteen-minute interactive window with nothing listening has no
  reason to keep the radio awake.

This does **not** lengthen the awake window, inhibit sleep for a connected
client, or hold the device up for the LAN server — those remain exactly as
described above. It makes the device reachable *during* the window it already
had.

Still unverified on hardware at the time of writing: this is the source fix for
the observed unreachability, not a measurement of it. The acceptance step is a
`curl` inside the window with `http: open fd=` and a request line in the same
capture.

A second contributing factor is **recorded and deliberately not acted on**: this
build trims the Wi-Fi RX buffers and does not set `ESP_WIFI_RX_IRAM_OPT`. If a
capture still shows timeouts with `power save off` in the log, and AP-side
causes (client isolation, a stale ARP entry for the device's address) have been
ruled out from a second machine, those `sdkconfig` values are the next
candidates. No `sdkconfig` was touched for this change.

### What the serial log now says about a wake

`Application::EnterCyclePhaseLocked` logs one line per phase transition:

```
wake phase: fetch after 21430 ms in network
```

That is the rest of the HB7 account — the status route reports only the phase
the budget ran *out* in, which is nothing at all on a cycle that finished. The
HTTP side has its own three lines, none of which carries a secret: `http: open
fd=` / `http: close fd=` around the TCP session, and one
`http: GET /api/v1/config -> 200 OK (auth_hdr=1)` per answered request. The
authentication header is reported as present or absent and never by value; no
token, body, SSID, BSSID or hub URL is logged anywhere.

Read together they separate the failures that used to look identical: no `open
fd=` line at all means no client reached the listener (radio, or the window was
over); an `open fd=` with no request line means the connection was accepted and
a handler never completed; `LAN HTTP server stopped` means somebody switched it
off; and `Entering deep sleep (…)` means the window simply ended.

---

## How a request reaches a device that is asleep

The tower records the request as an *intent* and delivers it the next time the
device is actually there. Delivery happens in exactly two places, and both are
something a person or an operator asked for:

- **`POST /api/device/power`** — the user clicked. Session and CSRF checked,
  like every other mutation in the tower. `action: "reconcile"` is the "try it
  now" button for a user who has just walked over and pressed the button on the
  device.
- **The background scheduler** (`runPowerIntentTick`) — every thirty seconds,
  and a no-op that opens no socket unless an intent is actually recorded.

**`GET /api/device/status` does not deliver, and must not.** It used to, which
meant reading the status page could PATCH the device's configuration and
rewrite the tower's durable state — a mutation on a route with no CSRF check.
It now calls `describeIntent`, which runs the same decision and writes nothing,
so what a reader sees is what the next delivery will do.

Waiting is not an attempt. A pass that finds the device asleep writes nothing
at all: the scheduler comes round whether or not the device is there, so
counting those passes made `attempts` accumulate a hundred and twenty entries
over one hour of normal sleep and read, in the UI, as a device that had refused
a hundred and twenty times.

## Battery reporting, and why it is often null

The board reads VBAT on **ADC1 channel 3, which is GPIO4 on the ESP32-S3**,
through a divider (the driver multiplies the converted value by 2), and
converts with `adc_cali_create_scheme_curve_fitting`. That scheme only exists
if this particular chip was factory calibrated. When it is absent the driver
has ADC counts and no volts.

`power::EvaluateBattery` gates on three things, and all three must hold before
a number is shown:

1. a reading came back at all;
2. the calibration scheme exists (`ZectrixReadBatteryMillivolts` reports it);
3. the voltage is inside 2800–4400 mV, which is where a single cell can be.

Otherwise `mv` and `percent` render as JSON `null`. The percentage itself is
the **inherited vendor curve** (`(-v² + 9016v - 19189000) / 10000`), unchanged:
it is what the device's own status bar has always shown, and having two
different answers on one screen would be worse than one imperfect one. What is
new is the gate in front of it.

> **Not verified on hardware.** The divider ratio, the curve, and therefore the
> absolute accuracy of the percentage have never been checked against a meter.
> The plausibility window will catch a wildly wrong divider; it will not catch
> a percentage that is ten points optimistic. See HB4 below.

---

## Hardware, as read from the board files

| Signal | Pin | Where | Note |
|---|---|---|---|
| Battery ADC | GPIO4 (ADC1 ch 3) | `zectrix-s3-epaper-4.2.cc` | not named in `config.h`; identified from `ADC_CHANNEL_3` |
| `VBAT_PWR_PIN` (power hold) | GPIO17 | `config.h` | driven by `BoardPowerBsp::VbatPowerOn/Off` with `gpio_hold_en` |
| `VBAT_PWR_GPIO` (power key) | GPIO18 | `config.h` | **multiplexed with `TODO_DOWN_BUTTON_GPIO`** |
| Wake button | GPIO0 (`BOOT_BUTTON_GPIO`) | `config.h` | the ext0 wake source, active low |
| Charge detect / full | GPIO2 / GPIO1 | `config.h` | read by `ChargeStatus` |
| RTC PCF8563 INT | GPIO5 | `config.h` | **not used as a wake source by this firmware** |

### Why the ESP32-S3 timer, and not the PCF8563 alarm

The product asks for an hourly wake. Both would do it, and the internal timer
was chosen because it needs no I²C transaction on the way into sleep and none
on the way out, and because the existing sleep path was already proven to
return from deep sleep on this board. The PCF8563 would be more accurate over
long intervals and is wired to an RTC-capable pin, so the option stays open:
`power::WakeReason::kRtcAlarm` and the `ESP_SLEEP_WAKEUP_EXT1` branch in
`WakeReasonFromChip()` already exist and are reported honestly, so a later
build that arms GPIO5 does not have to remember to come back and fix the
reporting.

Both wake sources are armed for **every** sleep, which is why
`esp_sleep_get_wakeup_causes()` (the bitmask) is used rather than the
deprecated singular call: a button press can genuinely coincide with the hourly
timer. When both are set the **button wins**, because a person pressing it
wants the device usable, and treating a coincident press as a plain timer wake
would put the device back to sleep in their hand.

---

## What has NOT been verified on hardware

Nothing in this change has been flashed or run on a device. Everything below is
a claim derived from reading the board files and the ESP-IDF headers, and each
one is a real way this could fail in the field.

**HB1 — The power latch survives a timer wake.**
GPIO17 is held with `gpio_hold_en` and is an RTC-capable pin on the S3, so the
hold should persist through deep sleep. The existing firmware already deep
sleeps and returns on the BOOT button, which is evidence the latch holds — but
it is not proof, because a device that had dropped the latch and powered off
would *also* appear to come back when the button was pressed. A **timer** wake
has no such ambiguity and has never been observed on this board.
*Test:* set `power.wake_interval_min` to 15, let it sleep on battery with no
USB and no finger on any button, and confirm it comes back by itself.

**HB2 — Deep sleep current is actually low.**
The measured sleep current is unknown. If a rail is left on — the EPD at
GPIO6, the audio rail at GPIO42, the NFC rail at GPIO21, or the LED task's
GPIO3 — the saving may be far smaller than the design assumes. Nothing in this
change touches those rails on the way into sleep.
*Test:* a meter in series with the battery, in deep sleep, before and after.

**HB3 — The panel is not left half-drawn.**
`PlanNextWake` refuses to sleep while `rendering || pending`, which is the
software guard. Whether the 4-colour refresh is genuinely finished when the
manager reports idle is a property of the panel driver, not of this change.
*Test:* force a refresh, let the budget expire during it, and inspect the glass.

**HB4 — The battery percentage means anything.**
See above. The curve and divider are inherited and unmeasured.
*Test:* a meter across the cell against the reported `mv`, at three states of
charge. If `calibrated` comes back false on the real chip, the tower will show
the explanation rather than a number, which is the intended behaviour and
should be confirmed to look right.

**HB5 — GPIO18 is not disturbed.**
GPIO18 is both the hardware power key and the down button. Nothing in this
change configures it, and it is deliberately **not** a wake source: arming the
hardware power key as a wake would be a good way to discover an unexpected
interaction with the latch.
*Test:* confirm a long press still powers the device down as before.

**HB6 — The interactive window ends.**
`PowerTick()` runs on the one-second loop in `Run()`. If that loop is starved —
by a long panel refresh on the same task, for instance — the window could
overrun. The shortest window is five minutes, so a second or two of overshoot
is harmless; a stall of minutes would not be.
*Test:* request a 5-minute window, leave it, confirm the device sleeps within
roughly a second of the deadline.

**HB7 — A wake cycle really does fit the budget.**
The budget arithmetic is proven; what it is set to is a guess. If association
plus DHCP on this network routinely takes more than 45 s, the device will wake,
fail, back off, and show a stale panel while appearing to work.
*Test:* log the phase durations over a day of real wakes and compare against
the caps. The device now emits these itself — one `wake phase: <to> after <n>
ms in <from>` line per transition — so this is a capture to read rather than
instrumentation to add.

**HB8 — The fetch phase's definition of "done" matches this device.**
This device is a *push* target: the tower sends frames to it, the device does
not pull. The cycle therefore treats the fetch phase as complete when a new
stored frame appears, and as `unchanged` when the fetch cap expires with none —
which assumes the tower notices the device is up and pushes within that window.
If the tower's own poll interval is longer than the fetch cap, every wake will
record `unchanged` and the panel will only ever be refreshed by the wake that
happens to coincide with a push.
*Test:* run a tower and a device together for a day and compare the count of
`updated` outcomes against the number of frames the tower actually sent.

---

## Suggested validation order

1. Flash to a **bench device**, not the installed one.
2. HB2 (sleep current) and HB1 (timer wake) first: if either fails, nothing
   else matters.
3. HB5 (power key) before leaving it unattended on battery.
4. HB4 (battery) and HB7 (budget) over a day of normal operation.
5. HB3 and HB6 last; they are correctness rather than "the device is bricked".

Until HB1 and HB2 pass, treat the battery-life claim as unmeasured. The code
is written so that the tower says "estimate" wherever it is estimating, and
that should stay true of anything written about this feature elsewhere.
