/**
 * @file earcon_player.cc
 * @brief Implementation of the earcon task.
 *
 * Copyright (c) 2026 NOTE4C poulailler dashboard contributors.
 * SPDX-License-Identifier: MIT
 */

#include "earcon_player.h"

#include <esp_log.h>

#include "audio_service.h"
#include "common/earcon_gen.h"

#define TAG "EarconPlayer"

EarconPlayer::~EarconPlayer() {
    Stop();
}

bool EarconPlayer::Start(AudioService* service) {
    if (service == nullptr) {
        ESP_LOGE(TAG, "No audio service; no earcons will be played");
        return false;
    }
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    service_ = service;
    // One buffer for the longest tone, allocated once. Doing this here rather
    // than per tone means a tone can never fail for want of memory at the
    // moment somebody presses the button.
    scratch_.assign(earcon::kMaxSamples, 0);

    queue_ = xQueueCreate(kQueueDepth, sizeof(audio_ui::Earcon));
    if (queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create the earcon queue");
        scratch_.clear();
        scratch_.shrink_to_fit();
        return false;
    }
    running_.store(true, std::memory_order_release);
    // Priority 5: above the UI, below the audio input task at 8, because
    // dropping a microphone frame to play a tone would be the wrong trade.
    if (xTaskCreate(TaskEntry, "earcon", 3 * 1024, this, 5, &task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the earcon task");
        running_.store(false, std::memory_order_release);
        vQueueDelete(queue_);
        queue_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "Earcon player ready (nothing about this board's speaker has been verified)");
    return true;
}

void EarconPlayer::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (queue_ != nullptr) {
        // Wake the task so it observes running_ == false and returns rather
        // than sitting in xQueueReceive forever.
        const audio_ui::Earcon wake = audio_ui::Earcon::kMuted;
        xQueueSend(queue_, &wake, 0);
    }
}

bool EarconPlayer::Request(audio_ui::Earcon earcon) {
    if (!running_.load(std::memory_order_acquire) || queue_ == nullptr) {
        return false;
    }
    // Zero ticks: this is called from the button task and must not wait.
    if (xQueueSend(queue_, &earcon, 0) != pdTRUE) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGW(TAG, "Earcon queue full, dropped %s", audio_ui::EarconName(earcon));
        return false;
    }
    return true;
}

void EarconPlayer::TaskEntry(void* arg) {
    static_cast<EarconPlayer*>(arg)->Run();
    vTaskDelete(nullptr);
}

void EarconPlayer::Run() {
    audio_ui::Earcon earcon = audio_ui::Earcon::kAcknowledge;
    while (running_.load(std::memory_order_acquire)) {
        if (xQueueReceive(queue_, &earcon, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        const size_t written =
            earcon::Render(earcon, scratch_.data(), scratch_.size());
        if (written == 0) {
            // Render refuses a short buffer rather than writing a partial
            // tone, because a truncated tone is a click on a class-D amp.
            ESP_LOGW(TAG, "Earcon %s did not render", audio_ui::EarconName(earcon));
            continue;
        }
        if (!service_->PlayPcm(scratch_.data(), written)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    task_ = nullptr;
    ESP_LOGW(TAG, "Earcon task stopped");
}
