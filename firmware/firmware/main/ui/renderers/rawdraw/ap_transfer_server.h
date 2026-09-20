/**
 * @file ap_transfer_server.h
 * @brief WiFi AP + HTTP Server for image transfer
 *
 * Provides:
 * - WiFi AP (SSID: InkScreen-AP, Password: 12345678)
 * - HTTP Server at 192.168.4.1
 * - HTML page for image upload
 * - Floyd-Steinberg dithering
 * - Save to SPIFFS
 */

#ifndef AP_TRANSFER_SERVER_H
#define AP_TRANSFER_SERVER_H

#include <string>
#include <functional>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace rawdraw {

/**
 * @brief AP Transfer Server for WiFi image upload
 */
class ApTransferServer {
public:
    ApTransferServer();
    ~ApTransferServer();

    // Start AP + HTTP server
    void Start();

    // Start HTTP server on the current Wi-Fi station interface
    bool StartLan(const std::string& ip_address);
    
    // Stop AP + HTTP server
    void Stop();

    // Check if server is running
    bool IsRunning() const { return server_ != nullptr; }
    bool IsApMode() const { return mode_ == TransferMode::kAp; }
    bool IsLanMode() const { return mode_ == TransferMode::kLan; }

    // State callback
    enum ServerState {
        kStopped,
        kApStarted,
        kClientConnected,
        kReceivingImage,
        kProcessingImage,
        kImageSaved,
        kError,
    };
    
    void SetStateCallback(std::function<void(ServerState, const std::string&)> callback);

    // Image received callback (called when image saved to SPIFFS)
    void SetImageReceivedCallback(std::function<void(const char* photo_id)> callback);

    void SetSettingsChangedCallback(std::function<void(int slideshow_interval_minutes)> callback);
    void SetPhotosChangedCallback(std::function<void()> callback);
    void SetShowPhotoCallback(std::function<bool(const std::string& photo_id)> callback);

private:
    enum class TransferMode {
        kNone,
        kAp,
        kLan,
    };

    httpd_handle_t server_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    volatile bool running_ = false;
    volatile bool starting_ = false;
    TaskHandle_t start_task_ = nullptr;
    std::string ap_ip_ = "192.168.4.1";
    TransferMode mode_ = TransferMode::kNone;

    std::function<void(ServerState, const std::string&)> state_callback_;
    std::function<void(const char* photo_id)> image_received_callback_;
    std::function<void(int slideshow_interval_minutes)> settings_changed_callback_;
    std::function<void()> photos_changed_callback_;
    std::function<bool(const std::string& photo_id)> show_photo_callback_;

    bool StartAccessPoint();
    const std::string& GetApIp() const { return ap_ip_; }
    bool StartHttpServer();
    static void StartTask(void* arg);

    // HTTP handlers
    static esp_err_t IndexHandler(httpd_req_t* req);
    static esp_err_t UploadHandler(httpd_req_t* req);
    static esp_err_t StatusHandler(httpd_req_t* req);
    static esp_err_t SettingsHandler(httpd_req_t* req);
    static esp_err_t PhotosHandler(httpd_req_t* req);
    static esp_err_t PhotoHandler(httpd_req_t* req);
    static esp_err_t PhotoMetaHandler(httpd_req_t* req);
    static esp_err_t PhotoMoveHandler(httpd_req_t* req);
    static esp_err_t PhotoShowHandler(httpd_req_t* req);
    
    // Notify state change
    void NotifyState(ServerState state, const std::string& message);
    
};

}  // namespace rawdraw

#endif  // AP_TRANSFER_SERVER_H
