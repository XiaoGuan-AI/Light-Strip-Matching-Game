#include "mqtt_protocol.h"
#include "board.h"
#include "display.h"
#include "application.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <ml307_mqtt.h>
#include <ml307_udp.h>
#include <cstring>
#include <algorithm>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "MQTT"

MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();
    
    // 创建非阻塞重连定时器
    esp_timer_create_args_t timer_args = {
        .callback = &MqttProtocol::ReconnectTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mqtt_reconnect",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
        reconnect_timer_ = nullptr;
    }
    if (udp_ != nullptr) {
        delete udp_;
    }
    if (mqtt_ != nullptr) {
        delete mqtt_;
    }
    vEventGroupDelete(event_group_handle_);
}

void MqttProtocol::ReconnectTimerCallback(void* arg) {
    MqttProtocol* self = static_cast<MqttProtocol*>(arg);
    // 在 Application 主循环中执行重连，避免在 esp_timer 线程中做重操作
    Application::GetInstance().Schedule([self]() {
        self->reconnect_pending_ = false;
        ESP_LOGI(TAG, "🔄 MQTT reconnect timer fired, attempting reconnection...");
        if (!self->StartMqttClient(true)) {
            // 重连失败，使用指数退避再次调度
            self->reconnect_backoff_ms_ = std::min(
                self->reconnect_backoff_ms_ * 2, 
                (uint32_t)MQTT_RECONNECT_MAX_BACKOFF_MS
            );
            ESP_LOGW(TAG, "MQTT reconnect failed, next retry in %lu ms", self->reconnect_backoff_ms_);
            self->ScheduleReconnect();
        } else {
            // 重连成功，重置退避时间
            self->reconnect_backoff_ms_ = MQTT_RECONNECT_INTERVAL_MS;
            ESP_LOGI(TAG, "✅ MQTT reconnected successfully");
        }
    });
}

void MqttProtocol::ScheduleReconnect() {
    if (reconnect_pending_) {
        ESP_LOGD(TAG, "Reconnect already pending, skip");
        return;
    }
    reconnect_pending_ = true;
    ESP_LOGI(TAG, "⏱️ Scheduling MQTT reconnect in %lu ms", reconnect_backoff_ms_);
    esp_timer_stop(reconnect_timer_);  // 防止重复启动
    esp_timer_start_once(reconnect_timer_, (uint64_t)reconnect_backoff_ms_ * 1000);
}

void MqttProtocol::Start() {
    StartMqttClient(false);
}

bool MqttProtocol::StartMqttClient(bool report_error) {
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started");
        delete mqtt_;
    }

    Settings settings("mqtt", false);
    // 优先读取 endpoint，如果为空则尝试 broker_url (OTA服务器返回的字段名)
    endpoint_ = settings.GetString("endpoint");
    ESP_LOGI(TAG, "Read endpoint from NVS: '%s'", endpoint_.c_str());
    if (endpoint_.empty()) {
        endpoint_ = settings.GetString("broker_url");
        ESP_LOGI(TAG, "Read broker_url from NVS: '%s'", endpoint_.c_str());
    }
    client_id_ = settings.GetString("client_id");
    username_ = settings.GetString("username");
    password_ = settings.GetString("password");
    publish_topic_ = settings.GetString("publish_topic");
    subscribe_topic_ = settings.GetString("subscribe_topic");
    
    // 如果没有配置 client_id，使用设备 MAC 地址 (与后端保持一致)
    if (client_id_.empty()) {
        client_id_ = SystemInfo::GetMacAddress();
        ESP_LOGI(TAG, "Using device MAC as client_id: %s", client_id_.c_str());
    }
    
    // 如果没有配置 topic，使用默认格式 (与 bread-test-mqtt 一致)
    if (subscribe_topic_.empty() && !client_id_.empty()) {
        subscribe_topic_ = "printer/" + client_id_ + "/command";
        ESP_LOGI(TAG, "Using default subscribe_topic: %s", subscribe_topic_.c_str());
    }
    if (publish_topic_.empty() && !client_id_.empty()) {
        publish_topic_ = "printer/" + client_id_ + "/result";
        ESP_LOGI(TAG, "Using default publish_topic: %s", publish_topic_.c_str());
    }
    
    ESP_LOGI(TAG, "MQTT Config: endpoint=%s, client_id=%s, subscribe_topic=%s, publish_topic=%s",
             endpoint_.c_str(), client_id_.c_str(), subscribe_topic_.c_str(), publish_topic_.c_str());

    if (endpoint_.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint/broker_url is not specified (check OTA server response)");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    mqtt_ = Board::GetInstance().CreateMqtt();
    mqtt_->SetKeepAlive(90);

    mqtt_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "Disconnected from endpoint");
        Board::GetInstance().GetDisplay()->StartNetworkErrorBlink();
        // ★★★ 使用非阻塞定时器重连，避免阻塞主循环 ★★★
        // 旧方案在 Schedule() 内 vTaskDelay(15000) 会阻塞主事件循环 15 秒，
        // 导致所有排队任务无法执行，严重时引发看门狗超时或级联故障
        ScheduleReconnect();
    });

    mqtt_->OnConnected([this]() {
        ESP_LOGI(TAG, "✅ MQTT Connected!");
        // 重连成功后重置退避时间
        reconnect_backoff_ms_ = MQTT_RECONNECT_INTERVAL_MS;
        reconnect_pending_ = false;
        // 订阅 topic 以接收服务器消息
        if (!subscribe_topic_.empty()) {
            if (mqtt_->Subscribe(subscribe_topic_, 1)) {
                ESP_LOGI(TAG, "📡 Subscribed to: %s", subscribe_topic_.c_str());
            } else {
                ESP_LOGE(TAG, "❌ Failed to subscribe to: %s", subscribe_topic_.c_str());
            }
        } else {
            ESP_LOGW(TAG, "⚠️ No subscribe topic configured, device cannot receive print commands");
        }
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        ESP_LOGI(TAG, "[RX][mqtt][text] topic=%s payload=%s", topic.c_str(), payload.c_str());
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (type == nullptr) {
            ESP_LOGE(TAG, "Message type is not specified");
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            ESP_LOGI(TAG, "Received goodbye message, session_id: %s", session_id ? session_id->valuestring : "null");
            if (session_id == nullptr || session_id_ == session_id->valuestring) {
                Application::GetInstance().Schedule([this]() {
                    CloseAudioChannel();
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    // 解析 endpoint URL: mqtt://host:port 或 mqtts://host:port
    std::string host = endpoint_;
    int port = 1883;  // 默认 MQTT 端口
    
    // 移除协议前缀
    size_t pos = endpoint_.find("://");
    if (pos != std::string::npos) {
        std::string protocol = endpoint_.substr(0, pos);
        host = endpoint_.substr(pos + 3);
        
        // mqtts 使用 TLS 端口
        if (protocol == "mqtts") {
            port = 8883;
        }
    }
    
    // 解析端口
    size_t port_pos = host.find(":");
    if (port_pos != std::string::npos) {
        port = std::stoi(host.substr(port_pos + 1));
        host = host.substr(0, port_pos);
    }
    
    ESP_LOGI(TAG, "Connecting to MQTT broker: %s:%d", host.c_str(), port);
    if (!mqtt_->Connect(host, port, client_id_, username_, password_)) {
        ESP_LOGE(TAG, "Failed to connect to MQTT broker");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    ESP_LOGI(TAG, "Connected to endpoint");
    Board::GetInstance().GetDisplay()->StopNetworkErrorBlink();
    return true;
}

bool MqttProtocol::SendText(const std::string& text) {
    if (publish_topic_.empty()) {
        return false;
    }
    ESP_LOGI(TAG, "[TX][mqtt][text] topic=%s payload=%s", publish_topic_.c_str(), text.c_str());
    if (!mqtt_->Publish(publish_topic_, text)) {
        ESP_LOGE(TAG, "Failed to publish message: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

void MqttProtocol::SendAudio(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ == nullptr) {
        return;
    }
    static uint32_t tx_bin_cnt = 0;
    if ((tx_bin_cnt++ % 10) == 0) {
        ESP_LOGI(TAG, "[TX][udp][bin] len=%u cnt=%u", (unsigned)data.size(), (unsigned)tx_bin_cnt);
    }

    std::string nonce(aes_nonce_);
    *(uint16_t*)&nonce[2] = htons(data.size());
    *(uint32_t*)&nonce[12] = htonl(++local_sequence_);

    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + data.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());

    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if (mbedtls_aes_crypt_ctr(&aes_ctx_, data.size(), &nc_off, (uint8_t*)nonce.c_str(), stream_block,
        (uint8_t*)data.data(), (uint8_t*)&encrypted[nonce.size()]) != 0) {
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        return;
    }
    udp_->Send(encrypted);
}

void MqttProtocol::CloseAudioChannel() {
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (udp_ != nullptr) {
            delete udp_;
            udp_ = nullptr;
        }
    }

    std::string message = "{";
    message += "\"session_id\":\"" + session_id_ + "\",";
    message += "\"type\":\"goodbye\"";
    message += "}";
    SendText(message);

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    // 发送 hello 消息申请 UDP 通道
    std::string message = "{";
    message += "\"type\":\"hello\",";
    message += "\"version\": 3,";
    message += "\"transport\":\"udp\",";
    message += "\"audio_params\":{";
    message += "\"format\":\"opus\", \"sample_rate\":16000, \"channels\":1, \"frame_duration\":" + std::to_string(OPUS_FRAME_DURATION_MS);
    message += "}}";
    if (!SendText(message)) {
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ != nullptr) {
        delete udp_;
    }
    udp_ = Board::GetInstance().CreateUdp();
    udp_->OnMessage([this](const std::string& data) {
        if (data.size() < sizeof(aes_nonce_)) {
            ESP_LOGE(TAG, "Invalid audio packet size: %zu", data.size());
            return;
        }
        if (data[0] != 0x01) {
            ESP_LOGE(TAG, "Invalid audio packet type: %x", data[0]);
            return;
        }
        uint32_t sequence = ntohl(*(uint32_t*)&data[12]);
        if (sequence < remote_sequence_) {
            ESP_LOGW(TAG, "Received audio packet with old sequence: %lu, expected: %lu", sequence, remote_sequence_);
            return;
        }
        if (remote_sequence_ != 0 && sequence != remote_sequence_ + 1) {
            uint32_t lost = (sequence > remote_sequence_) ? (sequence - (remote_sequence_ + 1)) : 0;
            if (lost > 0) {
                udp_loss_count_ += lost;
                if ((udp_loss_count_ % 100) == 0) {
                    ESP_LOGW(TAG, "[UDP][loss] lost=%lu total_loss=%llu last_seq=%lu curr_seq=%lu",
                        (unsigned long)lost,
                        (unsigned long long)udp_loss_count_,
                        (unsigned long)remote_sequence_,
                        (unsigned long)sequence);
                }
            }
        }

        std::vector<uint8_t> decrypted;
        size_t decrypted_size = data.size() - aes_nonce_.size();
        size_t nc_off = 0;
        uint8_t stream_block[16] = {0};
        decrypted.resize(decrypted_size);
        auto nonce = (uint8_t*)data.data();
        auto encrypted = (uint8_t*)data.data() + aes_nonce_.size();
        int ret = mbedtls_aes_crypt_ctr(&aes_ctx_, decrypted_size, &nc_off, nonce, stream_block, encrypted, (uint8_t*)decrypted.data());
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to decrypt audio data, ret: %d", ret);
            return;
        }
        static uint32_t rx_bin_cnt = 0;
        udp_recv_count_++;
        if ((rx_bin_cnt++ % 10) == 0) {
            ESP_LOGI(TAG, "[RX][udp][bin] len=%u seq=%lu cnt=%u loss_total=%llu", (unsigned)decrypted_size, (unsigned long)sequence, (unsigned)rx_bin_cnt, (unsigned long long)udp_loss_count_);
        }
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::move(decrypted));
        }
        remote_sequence_ = sequence;
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    udp_->Connect(udp_server_, udp_port_);

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

void MqttProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (session_id != nullptr) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    // Get sample rate from hello message
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (audio_params != NULL) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (sample_rate != NULL) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (frame_duration != NULL) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (udp == nullptr) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    udp_server_ = cJSON_GetObjectItem(udp, "server")->valuestring;
    udp_port_ = cJSON_GetObjectItem(udp, "port")->valueint;
    auto key = cJSON_GetObjectItem(udp, "key")->valuestring;
    auto nonce = cJSON_GetObjectItem(udp, "nonce")->valuestring;

    // auto encryption = cJSON_GetObjectItem(udp, "encryption")->valuestring;
    // ESP_LOGI(TAG, "UDP server: %s, port: %d, encryption: %s", udp_server_.c_str(), udp_port_, encryption);
    aes_nonce_ = DecodeHexString(nonce);
    mbedtls_aes_init(&aes_ctx_);
    mbedtls_aes_setkey_enc(&aes_ctx_, (const unsigned char*)DecodeHexString(key).c_str(), 128);
    local_sequence_ = 0;
    remote_sequence_ = 0;
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

static const char hex_chars[] = "0123456789ABCDEF";
// 辅助函数，将单个十六进制字符转换为对应的数值
static inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

std::string MqttProtocol::DecodeHexString(const std::string& hex_string) {
    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

bool MqttProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
