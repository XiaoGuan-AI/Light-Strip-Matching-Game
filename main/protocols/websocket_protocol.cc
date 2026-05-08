#include "websocket_protocol.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "application.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "WS"

// 【双子星架构】对话模式标志：一旦进入对话态，保持Chat页面直到明确退出
static bool g_in_conversation_mode = false;

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
    // 初始化对话模式标志
    g_in_conversation_mode = false;
}

WebsocketProtocol::~WebsocketProtocol() {
    StopHeartbeat();
    if (heartbeat_timer_ != nullptr) {
        esp_timer_delete(heartbeat_timer_);
        heartbeat_timer_ = nullptr;
    }
    if (websocket_ != nullptr) {
        delete websocket_;
    }
    vEventGroupDelete(event_group_handle_);
}

void WebsocketProtocol::Start() {
}

void WebsocketProtocol::HeartbeatTimerThunk(void* arg) {
    reinterpret_cast<WebsocketProtocol*>(arg)->OnHeartbeatTick();
}

void WebsocketProtocol::StartHeartbeat() {
    if (heartbeat_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = &WebsocketProtocol::HeartbeatTimerThunk,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ws_heartbeat",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &heartbeat_timer_);
    }
    // 15s 心跳（更快感知断链）
    esp_timer_stop(heartbeat_timer_);
    esp_timer_start_periodic(heartbeat_timer_, 15000000ULL);
}

void WebsocketProtocol::StopHeartbeat() {
    if (heartbeat_timer_) {
        esp_timer_stop(heartbeat_timer_);
    }
}

void WebsocketProtocol::ReconnectTimerThunk(void* arg) {
    WebsocketProtocol* self = reinterpret_cast<WebsocketProtocol*>(arg);
    Application::GetInstance().Schedule([self]() {
        self->reconnect_pending_ = false;
        ESP_LOGI(TAG, "reconnect timer fired -> OpenAudioChannel()");
        self->OpenAudioChannel();
    });
}

void WebsocketProtocol::StartReconnectOnce(uint64_t delay_us) {
    if (reconnect_pending_) return;
    reconnect_pending_ = true;
    if (delay_us == 0) {
        ReconnectTimerThunk(this);
        return;
    }
    if (reconnect_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = &WebsocketProtocol::ReconnectTimerThunk,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ws_reconnect_once",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &reconnect_timer_);
    }
    esp_timer_stop(reconnect_timer_);
    esp_timer_start_once(reconnect_timer_, delay_us);
}

void WebsocketProtocol::OnHeartbeatTick() {
    // 将网络 I/O 切到应用主循环，避免在 esp_timer 线程内做重操作
    Application::GetInstance().Schedule([this]() {
        // 后端不要求/不保证 pong，禁用 JSON ping 以及基于超时的主动重连
        // 连接保活由 TCP 层与上层业务消息自然维持
        (void)this;
    });
}

void WebsocketProtocol::SendWakeWordDetected(const std::string& wake_word) {
    // 【双子星架构】发送唤醒词时立即进入对话模式
    g_in_conversation_mode = true;
    ESP_LOGI(TAG, "[TX][listen.detect] Enter conversation mode (local)");
    
    // 调用父类方法发送消息
    Protocol::SendWakeWordDetected(wake_word);
}

void WebsocketProtocol::SendAudio(const std::vector<uint8_t>& data) {
    if (websocket_ == nullptr) {
        return;
    }
    static uint32_t tx_bin_cnt = 0;
    if ((tx_bin_cnt++ % 10) == 0) {
        ESP_LOGI(TAG, "[TX][bin] len=%u cnt=%u", (unsigned)data.size(), (unsigned)tx_bin_cnt);
    }
    // 60ms 节流与抖动统计
    auto now = std::chrono::steady_clock::now();
    if (tx_audio_count_ > 0) {
        auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tx_audio_time_).count();
        tx_audio_min_ms_ = std::min<int64_t>(tx_audio_min_ms_, delta_ms);
        tx_audio_max_ms_ = std::max<int64_t>(tx_audio_max_ms_, delta_ms);
        tx_audio_sum_ms_ += delta_ms;
        if (delta_ms < 50) {
            tx_audio_fast_count_++;
        } else if (delta_ms > 70) {
            tx_audio_slow_count_++;
        }
        if (llabs(delta_ms - 60) > 10) {
            tx_audio_jitter_count_++;
            if ((tx_audio_jitter_count_ % 100) == 0) {
                ESP_LOGW(TAG, "[AUDIO][jitter] delta=%lldms min=%lld max=%lld avg=%.2f fast=%llu slow=%llu jitter=%llu total=%llu",
                    (long long)delta_ms,
                    (long long)tx_audio_min_ms_,
                    (long long)tx_audio_max_ms_,
                    tx_audio_count_ ? (double)tx_audio_sum_ms_ / (double)tx_audio_count_ : 0.0,
                    (unsigned long long)tx_audio_fast_count_,
                    (unsigned long long)tx_audio_slow_count_,
                    (unsigned long long)tx_audio_jitter_count_,
                    (unsigned long long)tx_audio_count_);
            }
        }
        // 若过快（显著小于 60ms），主动 sleep 纠偏，避免抖动
        if (delta_ms < 55) {
            int wait_ms = 60 - (int)delta_ms;
            if (wait_ms > 0 && wait_ms <= 30) {
                vTaskDelay(pdMS_TO_TICKS(wait_ms));
                now = std::chrono::steady_clock::now();
            }
        }
    }
    last_tx_audio_time_ = now;
    tx_audio_count_++;

    websocket_->Send(data.data(), data.size(), true);
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr) {
        return false;
    }
    ESP_LOGI(TAG, "[TX][text] %s", text.c_str());
    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel() {
    if (websocket_ != nullptr) {
        delete websocket_;
        websocket_ = nullptr;
    }
}

bool WebsocketProtocol::OpenAudioChannel() {
    if (websocket_ != nullptr) {
        delete websocket_;
    }

    error_occurred_ = false;
    std::string url = CONFIG_WEBSOCKET_URL;
    std::string token = "Bearer " + std::string(CONFIG_WEBSOCKET_ACCESS_TOKEN);
    websocket_ = Board::GetInstance().CreateWebSocket();
    websocket_->SetHeader("Authorization", token.c_str());
    websocket_->SetHeader("Protocol-Version", "1");
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            static uint32_t rx_bin_cnt = 0;
            if ((rx_bin_cnt++ % 10) == 0) {
                ESP_LOGI(TAG, "[RX][bin] len=%u cnt=%u", (unsigned)len, (unsigned)rx_bin_cnt);
            }
            if (on_incoming_audio_ != nullptr) {
                on_incoming_audio_(std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len));
            }
        } else {
            // Parse JSON data
            ESP_LOGI(TAG, "[RX][text] %.*s", (int)len, data);
            auto root = cJSON_Parse(data);
            auto type = cJSON_GetObjectItem(root, "type");
            if (type != NULL) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ESP_LOGI(TAG, "[RX][hello] server capabilities received");
                    ParseServerHello(root);
                } else if (strcmp(type->valuestring, "pong") == 0) {
                    // 心跳回应，刷新超时计时
                    last_incoming_time_ = std::chrono::steady_clock::now();
                } else if (strcmp(type->valuestring, "listen") == 0) {
                    // 设备与后端 listen 映射：start/detect 开麦；stop 置 idle
                    auto state = cJSON_GetObjectItem(root, "state");
                    if (state && state->valuestring) {
                        if (strcmp(state->valuestring, "stop") == 0) {
                            ESP_LOGI(TAG, "[RX][listen.stop]");
                            Application::GetInstance().Schedule([](){
                                auto &app = Application::GetInstance();
                                if (app.GetDeviceState() != kDeviceStateIdle) {
                                    app.SetDeviceState(kDeviceStateIdle);
                                }
                                
                                // 【双子星架构】退出对话模式：stop表示对话结束，切回Welcome
                                if (g_in_conversation_mode) {
                                    g_in_conversation_mode = false;
                                    auto display = Board::GetInstance().GetDisplay();
                                    display->ShowWelcome();
                                    ESP_LOGI(TAG, "[RX][listen.stop] Exit conversation mode, show Welcome");
                                }
                            });
                        } else if (strcmp(state->valuestring, "start") == 0) {
                            ESP_LOGI(TAG, "[RX][listen.start]");
                            // 读取 mode: realtime|auto|manual → 进入聆听态（具体模式由应用层状态机处理）
                            Application::GetInstance().Schedule([](){
                                auto &app = Application::GetInstance();
                                auto display = Board::GetInstance().GetDisplay();
                                auto current_state = app.GetDeviceState();
                                
                                // 【双子星架构】智能页面切换逻辑：
                                // - 对话模式中（g_in_conversation_mode=true）→ 保持Chat页面
                                // - 初始状态（Idle/Connecting）→ 显示Welcome页面
                                // - 非对话模式 → 显示Welcome页面（兜底）
                                
                                if (g_in_conversation_mode) {
                                    // 对话中：保持Chat页面，不切换
                                    ESP_LOGI(TAG, "[RX][listen.start] Keep Chat page (in conversation mode)");
                                } else if (current_state == kDeviceStateIdle || current_state == kDeviceStateConnecting) {
                                    // 初始状态：切换到Welcome
                                    display->ShowWelcome();
                                    ESP_LOGI(TAG, "[RX][listen.start] Show Welcome (initial state)");
                                } else {
                                    // 其他情况：切换到Welcome（安全兜底）
                                    display->ShowWelcome();
                                    ESP_LOGI(TAG, "[RX][listen.start] Show Welcome (fallback)");
                                }
                                
                                app.SetDeviceState(kDeviceStateListening);
                            });
                            // @@@listen-echo - 向后端回发客户端的 start，确保ASR进入就绪
                            SendStartListening(kListeningModeRealtime);
                        } else if (strcmp(state->valuestring, "detect") == 0) {
                            // 【后端唤醒】后端检测到唤醒词，发送detect消息给硬件
                            ESP_LOGI(TAG, "[RX][listen.detect] Backend detected wake word");
                            
                            // 提取text字段（需要在lambda外部提取，避免root被释放）
                            auto text = cJSON_GetObjectItem(root, "text");
                            std::string wake_word = (text && text->valuestring) ? text->valuestring : "你好小智";
                            
                            Application::GetInstance().Schedule([this, wake_word](){
                                auto display = Board::GetInstance().GetDisplay();
                                display->EnterChatMode();
                                
                                // 1. 设置全局对话模式标志
                                g_in_conversation_mode = true;
                                
                                // 2. 回传detect消息给后端，触发后端对话模式
                                //    后端textHandle.py会处理这个消息并发送conversation_on
                                this->SendWakeWordDetected(wake_word);
                                
                                ESP_LOGI(TAG, "[RX][listen.detect] Echoed detect to backend: %s", wake_word.c_str());
                            });
                        }
                    }
                } else if (strcmp(type->valuestring, "tts") == 0) {
                    // 收到 tts.start 时，确保 UI 切入聊天页；随后仍然转发给上层，让应用保持原有处理（播放TTS等）
                    auto state = cJSON_GetObjectItem(root, "state");
                    if (state && state->valuestring && strcmp(state->valuestring, "start") == 0) {
                        Application::GetInstance().Schedule([](){
                            auto display = Board::GetInstance().GetDisplay();
                            display->EnterChatMode();
                        });
                    }
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                } else if (strcmp(type->valuestring, "goodbye") == 0) {
                    ESP_LOGI(TAG, "[RX][goodbye] -> close channel and idle");
                    // 回到 idle 并清理 UI，然后关闭音频通道
                    Application::GetInstance().Schedule([this](){
                        auto display = Board::GetInstance().GetDisplay();
                        display->SetChatMessage("system", "");
                        Application::GetInstance().SetDeviceState(kDeviceStateIdle);
                        this->CloseAudioChannel();
                        if (on_audio_channel_closed_ != nullptr) {
                            on_audio_channel_closed_();
                        }
                    });
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                // 兼容：某些服务端回“欢迎配置”（非 type=hello），但可能包含 audio_params/session_id
                ESP_LOGW(TAG, "[RX] message without type; try parse as hello (compat)");
                ParseServerHello(root);
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        StopHeartbeat();
        // 顶/底栏红色闪烁提示网络错误 + 提示文案，切到主循环
        Application::GetInstance().Schedule([](){
            auto d = Board::GetInstance().GetDisplay();
            d->StartNetworkErrorBlink();
            d->ShowBranchHint("网络断开，正在重连...", true);
        });
        if (on_audio_channel_closed_ != nullptr) {
            Application::GetInstance().Schedule([this]() {
                on_audio_channel_closed_();
            });
        }
        // 立即安排一次短重试（3s），之后进入指数退避
        ESP_LOGI(TAG, "schedule reconnect in 3s (fast)");
        StartReconnectOnce(3000000ULL);
    });

    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server");
        SetError(Lang::Strings::SERVER_NOT_FOUND);
        // 连接失败，增加指数退避（上限 60s）并计划下一次重连
        if (reconnect_backoff_sec_ < 60) reconnect_backoff_sec_ = std::min(60, reconnect_backoff_sec_ * 2);
        ESP_LOGW(TAG, "next reconnect in %ds", reconnect_backoff_sec_);
        StartReconnectOnce((uint64_t)reconnect_backoff_sec_ * 1000000ULL);
        return false;
    }

    // Send hello message to describe the client
    // keys: message type, version, audio_params (format, sample_rate, channels)
    std::string message = "{";
    message += "\"type\":\"hello\",";
    message += "\"version\": 1,";
    message += "\"transport\":\"websocket\",";
    message += "\"audio_params\":{";
    message += "\"format\":\"opus\", \"sample_rate\":16000, \"channels\":1, \"frame_duration\":" + std::to_string(OPUS_FRAME_DURATION_MS);
    message += "}}";
    if (!SendText(message)) {
        return false;
    }

    // Wait for server hello
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    // 停止网络错误闪烁并清除提示（切到主循环）
    Application::GetInstance().Schedule([](){
        auto d = Board::GetInstance().GetDisplay();
        d->StopNetworkErrorBlink();
        d->ClearBranchHint();
    });
    // 连接成功，重置退避并启动心跳
    reconnect_backoff_sec_ = 5;
    StartHeartbeat();

    return true;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    // 兼容非严格 hello：若不存在 transport 或值异常，不直接失败
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport && transport->valuestring && strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGW(TAG, "Unexpected transport: %s (compat)", transport->valuestring);
    }

    // 更新会话 ID（若后端返回）
    auto sid = cJSON_GetObjectItem(root, "session_id");
    if (sid && sid->valuestring) {
        session_id_ = sid->valuestring;
    } else {
        session_id_.clear();
    }
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

    ESP_LOGI(TAG, "[HELLO] session_id=%s sample_rate=%d frame_duration=%d",
        session_id_.c_str(), server_sample_rate_, server_frame_duration_);
    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
