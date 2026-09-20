#include "esp_tcp.h"

#include <esp_log.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>

static const char *TAG = "EspTcp";
static constexpr int kConnectTimeoutMs = 1500;
static constexpr int kSendTimeoutMs = 1000;
static constexpr TickType_t kReceivePollDelayTicks = pdMS_TO_TICKS(20);

EspTcp::EspTcp() {
    event_group_ = xEventGroupCreate();
}

EspTcp::~EspTcp() {
    if (connected_.load(std::memory_order_relaxed)) {
        // Active connection: close socket and wait for ReceiveTask to exit.
        DoDisconnect(true);
    } else if (receive_task_handle_ != nullptr) {
        // Already passively disconnected, but the ReceiveTask lambda may not
        // have called xEventGroupSetBits yet.  Wait briefly so we do not
        // delete event_group_ while the task is still running.
        xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT,
                            pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
    }

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspTcp::Connect(const std::string& host, int port) {
    // 确保先断开已有连接
    if (connected_.load(std::memory_order_relaxed)) {
        Disconnect();
    }

    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%d", port);

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* server = nullptr;
    int resolve_ret = getaddrinfo(host.c_str(), port_text, &hints, &server);
    if (resolve_ret != 0 || server == nullptr) {
        last_error_ = resolve_ret != 0 ? resolve_ret : EHOSTUNREACH;
        ESP_LOGE(TAG, "Failed to resolve %s:%d, code=%d", host.c_str(), port, last_error_);
        return false;
    }

    tcp_fd_ = socket(server->ai_family, server->ai_socktype, server->ai_protocol);
    if (tcp_fd_ < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to create socket");
        freeaddrinfo(server);
        return false;
    }

    timeval send_timeout = {};
    send_timeout.tv_sec = kSendTimeoutMs / 1000;
    send_timeout.tv_usec = (kSendTimeoutMs % 1000) * 1000;
    setsockopt(tcp_fd_, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

    const int original_flags = fcntl(tcp_fd_, F_GETFL, 0);
    if (original_flags >= 0) {
        fcntl(tcp_fd_, F_SETFL, original_flags | O_NONBLOCK);
    }

    int ret = connect(tcp_fd_, server->ai_addr, server->ai_addrlen);
    if (ret < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, last_error_);
        freeaddrinfo(server);
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }

    if (ret < 0) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(tcp_fd_, &write_fds);

        timeval timeout = {};
        timeout.tv_sec = kConnectTimeoutMs / 1000;
        timeout.tv_usec = (kConnectTimeoutMs % 1000) * 1000;

        ret = select(tcp_fd_ + 1, nullptr, &write_fds, nullptr, &timeout);
        if (ret <= 0) {
            last_error_ = (ret == 0) ? ETIMEDOUT : errno;
            ESP_LOGE(TAG, "Connect timeout to %s:%d, code=0x%x", host.c_str(), port, last_error_);
            freeaddrinfo(server);
            close(tcp_fd_);
            tcp_fd_ = -1;
            return false;
        }

        int socket_error = 0;
        socklen_t socket_error_len = sizeof(socket_error);
        if (getsockopt(tcp_fd_, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) < 0) {
            last_error_ = errno;
            ESP_LOGE(TAG, "Failed to read socket error for %s:%d, code=0x%x", host.c_str(), port, last_error_);
            freeaddrinfo(server);
            close(tcp_fd_);
            tcp_fd_ = -1;
            return false;
        }

        if (socket_error != 0) {
            last_error_ = socket_error;
            ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, last_error_);
            freeaddrinfo(server);
            close(tcp_fd_);
            tcp_fd_ = -1;
            return false;
        }
    }

    // Restore original socket flags (blocking mode)
    // NOTE: Do NOT use SO_RCVTIMEO -- it triggers a pbuf double-free bug
    // in ESP-IDF v6.0 LWIP (assert failed: pbuf_free: p->ref > 0)
    if (original_flags >= 0) {
        fcntl(tcp_fd_, F_SETFL, original_flags);
    }

    freeaddrinfo(server);

    connected_.store(true, std::memory_order_relaxed);

    xEventGroupClearBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
    BaseType_t create_ok = xTaskCreatePinnedToCore([](void* arg) {
        EspTcp* tcp = (EspTcp*)arg;
        tcp->ReceiveTask();
        tcp->receive_task_handle_ = nullptr;
        xEventGroupSetBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "tcp_receive", 8192, this, 1, &receive_task_handle_, 0 /* pinned to Core 0 */);
    if (create_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create tcp_receive task");
        DoDisconnect(false);
        return false;
    }
    return true;
}

void EspTcp::Disconnect() {
    DoDisconnect(true);
}

void EspTcp::DoDisconnect(bool wait_for_task) {
    const bool was_connected = connected_.exchange(false, std::memory_order_acq_rel);

    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (tcp_fd_ != -1) {
            fd = tcp_fd_;
            tcp_fd_ = -1;
        }
    }
    if (fd != -1) {
        close(fd);
    }

    // Wait only when called from a different task; the receive task itself
    // must never wait for its own exit event.
    if (wait_for_task && receive_task_handle_ != nullptr &&
        xTaskGetCurrentTaskHandle() != receive_task_handle_) {
        auto bits = xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT,
                                        pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(bits & ESP_TCP_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGE(TAG, "Failed to wait for receive task exit");
        }
    }

    // Fire callback exactly once per connected session.
    if (was_connected && disconnect_callback_) {
        disconnect_callback_();
    }
}

int EspTcp::Send(const std::string& data) {
    if (!connected_.load(std::memory_order_relaxed)) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        fd = tcp_fd_;
    }
    if (fd < 0) {
        ESP_LOGE(TAG, "TCP fd invalid");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();
    int eagain_count = 0;
    constexpr int kMaxEagainRetries = 10;  // ~10 seconds total wait (10 * 1s)

    while (total_sent < data_size) {
        int ret = send(fd, data_ptr + total_sent, data_size - total_sent, 0);

        if (ret < 0) {
            // 保存 errno 值，因为在 ESP-IDF 上 errno 可能是宏/函数调用
            // 且可能被其他操作（如 ESP_LOG）覆盖
            int saved_errno = errno;
            
            if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
                // Socket buffer full or SO_SNDTIMEO elapsed, wait for writable
                if (eagain_count++ >= kMaxEagainRetries) {
                    ESP_LOGE(TAG, "Send timeout after %d EAGAINs (%zu/%zu bytes sent)",
                             kMaxEagainRetries, total_sent, data_size);
                    return -1;
                }
                
                fd_set write_fds;
                FD_ZERO(&write_fds);
                FD_SET(fd, &write_fds);
                timeval timeout = {};
                timeout.tv_sec = 1;  // 1 second per retry
                
                int sel_ret = select(fd + 1, nullptr, &write_fds, nullptr, &timeout);
                if (sel_ret < 0) {
                    ESP_LOGE(TAG, "Send select error: sel_ret=%d, errno=%d", sel_ret, errno);
                    return -1;
                }
                if (sel_ret == 0) {
                    // select timed out, but socket might still be sendable
                    // Try send again - if still EAGAIN, loop will retry select
                    ESP_LOGD(TAG, "Send select timeout (attempt %d/%d, %zu bytes pending)",
                             eagain_count, kMaxEagainRetries, data_size - total_sent);
                }
                // Retry send regardless of select result
                continue;
            }
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d (saved=%d)", ret, errno, saved_errno);
            return ret;
        }

        if (ret == 0) {
            // Connection closed by peer (shouldn't happen on blocking socket)
            ESP_LOGE(TAG, "Send returned 0 (connection closed)");
            return 0;
        }

        eagain_count = 0;  // Reset on successful send
        total_sent += ret;
    }

    return total_sent;
}

void EspTcp::ReceiveTask() {
    std::string data;
    data.resize(1500);
    while (connected_.load(std::memory_order_relaxed)) {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            fd = tcp_fd_;
        }
        if (fd < 0) {
            break;
        }

        // Avoid esp_vfs_select heap allocations here to reduce allocator pressure
        // and potential select-path instability on long-running sessions.
        int ret = recv(fd, data.data(), data.size(), MSG_DONTWAIT);
        if (ret < 0) {
            // 保存 errno 值，因为在 ESP-IDF 上 errno 可能是宏/函数调用
            int saved_errno = errno;
            if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
                vTaskDelay(kReceivePollDelayTicks);
                continue;
            }
            ESP_LOGE(TAG, "TCP receive failed: errno=%d (saved=%d)", errno, saved_errno);
            DoDisconnect(false);
            break;
        }
        if (ret == 0) {
            // Connection closed by peer
            DoDisconnect(false);
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
            data.resize(1500);
        } else {
            continue;
        }
    }
}

int EspTcp::GetLastError() {
    return last_error_;
}
