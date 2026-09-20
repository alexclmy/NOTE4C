/**
 * @file http_deadline.h
 * @brief The whole-operation deadline, enforced where the bytes actually arrive.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 *
 * WHY THIS IS NOT JUST `esp_http_client_set_timeout_ms`
 * -----------------------------------------------------
 * It is tempting to believe that computing "how many milliseconds are left" and
 * handing that to esp_http_client before each blocking call bounds the call. It
 * does not, and the reason is visible in the IDF source this firmware builds
 * against (ESP-IDF v6.0, components/esp_http_client/esp_http_client.c):
 *
 *   - `esp_http_client_fetch_headers` (v6.0, l.1579-1590) loops
 *     `while (client->state < HTTP_STATE_RES_COMPLETE_HEADER)` and calls
 *     `esp_transport_read(..., client->timeout_ms)` on every iteration.
 *   - `esp_http_client_read` (v6.0, l.1357-1372) loops
 *     `while (need_read > 0 && is_data_remain)` and does the same.
 *
 * `client->timeout_ms` is the budget for ONE `esp_transport_read`, and the loop
 * restarts it every time a byte arrives. A server that trickles one byte just
 * inside the timeout satisfies an unbounded number of iterations, so a single
 * call to either function can outlive any budget applied before it. Checking
 * the clock *between* calls — which is all a caller can do — never regains
 * control, because control never comes back.
 *
 * Nor does IDF's async mode fix it. `is_async` makes the socket non-blocking
 * and lets `esp_http_client_perform` return `ESP_ERR_HTTP_EAGAIN`, but
 * `esp_transport_read` for TLS is `ssl_read` (tcp_transport/transport_ssl.c),
 * which begins with `esp_transport_poll_read(t, timeout_ms)` — a `select()`
 * that blocks for the full timeout no matter how the socket is flagged. The
 * internal loops above still iterate once per arriving record.
 *
 * So the deadline has to live at the innermost blocking primitive: the
 * transport read itself. That is what this class is. It holds one absolute
 * millisecond deadline for the whole exchange, and the transport wrapper
 * consults it on every single read, write and poll:
 *
 *   - Before the deadline it *clamps* the caller's requested timeout, so no
 *     individual operation can overshoot the end of the exchange.
 *   - After the deadline it returns `kExpired`, and the wrapper turns that into
 *     a hard transport error rather than a timeout. That distinction is load
 *     bearing: IDF's loops treat a timeout as "maybe more is coming" and a hard
 *     error as "stop", so only the hard error actually breaks the loop.
 *
 * It is deliberately free of every ESP-IDF type, because the host suite drives
 * it through a fake that reimplements the two IDF loops above byte for byte.
 * See tests/host/test_http_deadline.cc.
 */
#ifndef NOTE4C_COMMON_HTTP_DEADLINE_H_
#define NOTE4C_COMMON_HTTP_DEADLINE_H_

#include <stdint.h>

namespace weather {

/// Returned by TransportDeadline::ClampMs when the exchange is over. Negative so
/// it can never be mistaken for a timeout, and distinct from every valid one.
constexpr int32_t kDeadlineExpired = -1;

/**
 * @brief One absolute deadline for one whole HTTP exchange.
 *
 * Armed once, before the connection is opened, and consulted by every transport
 * operation until the exchange ends. Not thread safe by design: it belongs to a
 * single exchange on a single task, and sharing one across tasks would mean two
 * exchanges sharing a deadline, which is not a thing this firmware does.
 */
class TransportDeadline {
public:
    /// Start the clock. @p budget_ms is the whole-operation ceiling; anything
    /// not positive arms an already-expired deadline, so nothing blocks at all.
    void Arm(int64_t now_ms, int32_t budget_ms) {
        deadline_ms_ = now_ms + (budget_ms > 0 ? budget_ms : 0);
        armed_ = true;
    }

    void Disarm() { armed_ = false; }

    bool armed() const { return armed_; }

    /// The absolute deadline. Only meaningful while armed.
    int64_t deadline_ms() const { return deadline_ms_; }

    /// Milliseconds left, floored at zero.
    int64_t RemainingMs(int64_t now_ms) const {
        if (!armed_) return 0;
        const int64_t left = deadline_ms_ - now_ms;
        return left > 0 ? left : 0;
    }

    /**
     * @brief How long this one transport operation may block.
     *
     * @return `kDeadlineExpired` when the exchange is over — the caller must
     *         report a hard transport error, not a timeout — otherwise the
     *         requested timeout clamped to what is left.
     *
     * An unarmed deadline imposes nothing and returns @p requested_ms
     * unchanged, so a transport built without one behaves exactly as the stock
     * IDF transport does. That is the fallback, not the intended path.
     */
    int32_t ClampMs(int64_t now_ms, int32_t requested_ms) const {
        if (!armed_) return requested_ms;
        const int64_t left = deadline_ms_ - now_ms;
        if (left <= 0) return kDeadlineExpired;
        if (requested_ms < 0) return static_cast<int32_t>(left);
        return requested_ms < left ? requested_ms : static_cast<int32_t>(left);
    }

private:
    int64_t deadline_ms_ = 0;
    bool armed_ = false;
};

}  // namespace weather

#endif  // NOTE4C_COMMON_HTTP_DEADLINE_H_
