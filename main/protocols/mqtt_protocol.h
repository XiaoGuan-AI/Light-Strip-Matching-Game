#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H


#include "protocol.h"
#include <mqtt.h>
#include <udp.h>
#include <cJSON.h>
#include <mbedtls/aes.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <functional>
#include <string>
#include <map>
#include <mutex>

#define MQTT_PING_INTERVAL_SECONDS 90
#define MQTT_RECONNECT_INTERVAL_MS 15000
#define MQTT_RECONNECT_MAX_BACKOFF_MS 60000

#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class MqttProtocol : public Protocol {
public:
    MqttProtocol();
    ~MqttProtocol();

    void Start() override;
    void SendAudio(const std::vector<uint8_t>& data) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;

    std::string endpoint_;
    std::string client_id_;
    std::string username_;
    std::string password_;
    std::string publish_topic_;
    std::string subscribe_topic_;

    std::mutex channel_mutex_;
    Mqtt* mqtt_ = nullptr;
    Udp* udp_ = nullptr;
    mbedtls_aes_context aes_ctx_;
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_;
    uint32_t local_sequence_;
    uint32_t remote_sequence_;
    uint64_t udp_loss_count_ = 0;     // 丢包计数（根据序号缺口累加）
    uint64_t udp_recv_count_ = 0;     // 收到包计数

    // ★★★ 非阻塞重连机制 ★★★
    esp_timer_handle_t reconnect_timer_ = nullptr;
    bool reconnect_pending_ = false;
    uint32_t reconnect_backoff_ms_ = MQTT_RECONNECT_INTERVAL_MS;
    static void ReconnectTimerCallback(void* arg);
    void ScheduleReconnect();

    bool StartMqttClient(bool report_error=false);
    void ParseServerHello(const cJSON* root);
    std::string DecodeHexString(const std::string& hex_string);

    bool SendText(const std::string& text) override;
};


#endif // MQTT_PROTOCOL_H
