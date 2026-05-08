#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
extern "C" {
#include <esp_timer.h>
}

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    void Start() override;
    void SendAudio(const std::vector<uint8_t>& data) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;
    void SendWakeWordDetected(const std::string& wake_word) override;

private:
    EventGroupHandle_t event_group_handle_;
    WebSocket* websocket_ = nullptr;
    esp_timer_handle_t heartbeat_timer_ = nullptr;
    bool reconnect_pending_ = false;
    esp_timer_handle_t reconnect_timer_ = nullptr;
    int reconnect_backoff_sec_ = 5;

    // Audio TX interval metrics (target 60ms)
    std::chrono::time_point<std::chrono::steady_clock> last_tx_audio_time_{};
    uint64_t tx_audio_count_ = 0;
    uint64_t tx_audio_jitter_count_ = 0;      // |delta-60| > 10ms
    uint64_t tx_audio_fast_count_ = 0;        // delta < 50ms
    uint64_t tx_audio_slow_count_ = 0;        // delta > 70ms
    int64_t  tx_audio_min_ms_ = INT64_MAX;
    int64_t  tx_audio_max_ms_ = 0;
    int64_t  tx_audio_sum_ms_ = 0;

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    // Heartbeat
    static void HeartbeatTimerThunk(void* arg);
    void OnHeartbeatTick();
    void StartHeartbeat();
    void StopHeartbeat();
    // Reconnect
    static void ReconnectTimerThunk(void* arg);
    void StartReconnectOnce(uint64_t delay_us);
};

#endif
