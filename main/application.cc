#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "ml307_ssl_transport.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "font_awesome_symbols.h"
#include "iot/thing_manager.h"
#include "assets/lang_config.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();
    background_task_ = new BackgroundTask(4096 * 8);

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            app->OnClockTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Enable realtime barge-in by default
    realtime_chat_enabled_ = true;

    // 初始化TTS状态
    is_tts_playing_ = false;
    last_tts_start_time_ = std::chrono::steady_clock::now();
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (background_task_ != nullptr) {
        delete background_task_;
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion() {
    // OTA功能已禁用 - 如果需要启用，请删除上面这些行
    ESP_LOGI(TAG, "OTA check disabled");
    return;
}

void Application::ShowActivationCode() {
    auto& message = ota_.GetActivationMessage();
    auto& code = ota_.GetActivationCode();

    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::P3_0},
        digit_sound{'1', Lang::Sounds::P3_1},
        digit_sound{'2', Lang::Sounds::P3_2},
        digit_sound{'3', Lang::Sounds::P3_3},
        digit_sound{'4', Lang::Sounds::P3_4},
        digit_sound{'5', Lang::Sounds::P3_5},
        digit_sound{'6', Lang::Sounds::P3_6},
        digit_sound{'7', Lang::Sounds::P3_7},
        digit_sound{'8', Lang::Sounds::P3_8},
        digit_sound{'9', Lang::Sounds::P3_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion);
    auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(status);
        display->SetEmotion(emotion);
        display->SetChatMessage("system", message);
    if (!sound.empty()) {
        ResetDecoder();
        PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
    }
}

void Application::PlaySound(const std::string_view& sound) {
    // Wait for the previous sound to finish
    {
        std::unique_lock<std::mutex> lock(mutex_);
        audio_decode_cv_.wait(lock, [this]() {
            return audio_decode_queue_.empty();
        });
    }
    background_task_->WaitForCompletion();

    // The assets are encoded at 16000Hz, 60ms frame duration
    SetDecodeSampleRate(16000, 60);
    const char* data = sound.data();
    size_t size = sound.size();
    for (const char* p = data; p < data + size; ) {
        auto p3 = (BinaryProtocol3*)p;
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size);
        std::vector<uint8_t> opus;
        opus.resize(payload_size);
        memcpy(opus.data(), p3->payload, payload_size);
        p += payload_size;

        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.emplace_back(std::move(opus));
    }
}

void Application::PlaySoundEffect(const std::string_view& sound) {
    if (sound.empty()) return;
    ESP_LOGI(TAG, "PlaySoundEffect: size=%zu, state=%d", sound.size(), device_state_);
    
    // 确保音频输出被启用
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec->output_enabled()) {
        ESP_LOGI(TAG, "PlaySoundEffect: enabling audio output");
        codec->EnableOutput(true);
    }
    
    aborted_ = false;
    ResetDecoder();
    PlaySound(sound);
    ESP_LOGI(TAG, "PlaySoundEffect: done, queue_size=%zu", audio_decode_queue_.size());
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    // 改为：按键仅在 UI 层切换显示/提示；网络连接上电即连，不再通过按键开关
    if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            // 保持通道打开，仅切 UI
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    } else if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            Board::GetInstance().GetDisplay()->EnterChatMode();
            SetListeningMode(realtime_chat_enabled_ ? kListeningModeRealtime : kListeningModeAutoStop);
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
                SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Initialize Match Game (灯带消消乐) */
    if (!match_game_.Initialize()) {
        ESP_LOGW(TAG, "Match game initialization failed, continuing without it");
    }

    /* Setup the display */
    auto display = board.GetDisplay();

    /* Setup the audio codec (skip if no audio hardware, e.g., Component Sorter board) */
    auto codec = board.GetAudioCodec();
    if (!codec) {
        ESP_LOGI(TAG, "No audio codec available, skipping audio setup");
    } else {
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    if (realtime_chat_enabled_) {
        ESP_LOGI(TAG, "Realtime chat enabled, setting opus encoder complexity to 0");
        opus_encoder_->SetComplexity(0);
    } else if (board.GetBoardType() == "ml307") {
        ESP_LOGI(TAG, "ML307 board detected, setting opus encoder complexity to 5");
        opus_encoder_->SetComplexity(5);
    } else {
        ESP_LOGI(TAG, "WiFi board detected, setting opus encoder complexity to 3");
        opus_encoder_->SetComplexity(3);
    }

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }
    codec->Start();

    xTaskCreatePinnedToCore([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop();
        vTaskDelete(NULL);
    }, "audio_loop", 4096 * 2, this, 8, &audio_loop_task_handle_, realtime_chat_enabled_ ? 1 : 0);
    }

    /* Wait for the network to be ready */
    board.StartNetwork();

    /* 启动灯带消消乐 (需要 WiFi 连接成功后才能启动) */
    if (match_game_.IsInitialized()) {
        if (!match_game_.Start()) {
            ESP_LOGW(TAG, "Match game start failed, continuing without it");
        } else {
            ESP_LOGI(TAG, "Match game started successfully");
        }
    }

    // Check for new firmware version or get the MQTT broker address
    if (display) {
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);
    }
    CheckNewVersion();

    // Initialize the protocol
        display->SetStatus(Lang::Strings::LOADING_PROTOCOL);
#ifdef CONFIG_CONNECTION_TYPE_WEBSOCKET
    protocol_ = std::make_unique<WebsocketProtocol>();
#else
    protocol_ = std::make_unique<MqttProtocol>();
#endif
    protocol_->OnNetworkError([this](const std::string& message) {
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, message.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION);
    });
    protocol_->OnIncomingAudio([this](std::vector<uint8_t>&& data) {
            std::lock_guard<std::mutex> lock(mutex_);
            audio_decode_queue_.emplace_back(std::move(data));
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        // 若在断线前处于 Listening/Speaking，则重连后恢复到 Listening（AutoStop，便于连续多轮对话）
        if (resume_listening_after_reconnect_) {
            resume_listening_after_reconnect_ = false;
            Schedule([this]() {
                SetListeningMode(kListeningModeAutoStop);
            });
        }
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
        SetDecodeSampleRate(protocol_->server_sample_rate(), protocol_->server_frame_duration());
        auto& thing_manager = iot::ThingManager::GetInstance();
        protocol_->SendIotDescriptors(thing_manager.GetDescriptorsJson());
        std::string states;
        if (thing_manager.GetStatesJson(states, false)) {
            protocol_->SendIotStates(states);
        }
        // 初次建立音频通道后，自动进入聆听模式（上电即连即听）
        Schedule([this]() {
            if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateStarting || device_state_ == kDeviceStateConnecting) {
                SetListeningMode(realtime_chat_enabled_ ? kListeningModeRealtime : kListeningModeAutoStop);
            }
        });
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        // 若当前在 Listening/ Speaking，标记重连后需恢复 Listening
        if (device_state_ == kDeviceStateListening || device_state_ == kDeviceStateSpeaking) {
            resume_listening_after_reconnect_ = true;
        }
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "mode") == 0) {
            auto action = cJSON_GetObjectItem(root, "action");
            if (action && action->valuestring) {
                if (strcmp(action->valuestring, "conversation_on") == 0 || strcmp(action->valuestring, "converse_on") == 0 || strcmp(action->valuestring, "on") == 0) {
                    Schedule([this]() {
                        // UI：进入对话模式
                        Board::GetInstance().GetDisplay()->EnterChatMode();
                    });
                } else if (strcmp(action->valuestring, "conversation_off") == 0 || strcmp(action->valuestring, "converse_off") == 0 || strcmp(action->valuestring, "off") == 0 || strcmp(action->valuestring, "listen") == 0) {
                    Schedule([this]() {
                        // UI：切回聆听
                        auto display = Board::GetInstance().GetDisplay();
                        display->SetChatMessage("system", "");
                        SetDeviceState(kDeviceStateListening);
                    });
                }
            }
            return;
        } else if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;

                    // ========== TTS开始播放：更新状态并切换AFE ==========
                    is_tts_playing_ = true;
                    last_tts_start_time_ = std::chrono::steady_clock::now();

                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }

                    // TTS开始播放，切换到Wake Word Detection（允许唤醒词打断）
                    UpdateAfeStrategy();
                    ESP_LOGI(TAG, "TTS started, switched to Wake Word Detection for interrupt");

                    // 进入播报时，关闭远端语音打断，仅保留唤醒词/按键
                    SetAllowRemoteBargeIn(false);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    background_task_->WaitForCompletion();

                    // ========== TTS停止播放：更新状态并切换AFE ==========
                    is_tts_playing_ = false;

                    if (device_state_ == kDeviceStateSpeaking) {
                        // TTS停止，根据对话模式决定AFE策略
                        if (listening_mode_ == kListeningModeRealtime) {
                            // 对话模式：切换回Audio Processor准备下一轮交互
                            UpdateAfeStrategy();
                            ESP_LOGI(TAG, "TTS stopped, switched to Audio Processor for next input");
                        } else {
                            // 非对话模式：切换到Listening或Idle状态
                            if (listening_mode_ == kListeningModeManualStop) {
                                SetDeviceState(kDeviceStateIdle);
                            } else {
                                SetDeviceState(kDeviceStateListening);
                            }
                        }
                        // 播报结束恢复远端打断
                        SetAllowRemoteBargeIn(true);
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (text != NULL) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, message = std::string(text->valuestring)]() {
                        auto display = Board::GetInstance().GetDisplay();
                        display->SetChatMessage("assistant", message.c_str());
                        // Branch-hint heuristics for confirmation/retry/rollback
                        if (strstr(message.c_str(), "确认") || strstr(message.c_str(), "confirm")) {
                            display->ShowBranchHint("请说: 确认 / 取消", true);
                        } else if (strstr(message.c_str(), "重试") || strstr(message.c_str(), "retry")) {
                            display->ShowBranchHint("请说: 重试 / 回退", true);
                        } else if (strstr(message.c_str(), "回退") || strstr(message.c_str(), "rollback")) {
                            display->ShowBranchHint("执行回退? 说: 确认回退", true);
                        } else {
                            display->ClearBranchHint();
                        }
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (text != NULL) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, message = std::string(text->valuestring)]() {
                    auto display = Board::GetInstance().GetDisplay();
                    display->SetChatMessage("user", message.c_str());
                    // 若正在播报且禁止远端打断，则不触发打断
                    if (device_state_ == kDeviceStateSpeaking && !GetAllowRemoteBargeIn()) {
                        return;
                    }
                    if (strstr(message.c_str(), "确认") || strstr(message.c_str(), "confirm")) {
                        display->ShowBranchHint("✓ 已确认", false);
                    } else if (strstr(message.c_str(), "取消") || strstr(message.c_str(), "cancel")) {
                        display->ShowBranchHint("✗ 已取消", false);
                    } else if (strstr(message.c_str(), "重试") || strstr(message.c_str(), "retry")) {
                        display->ShowBranchHint("↻ 请求重试", false);
                    } else if (strstr(message.c_str(), "回退") || strstr(message.c_str(), "rollback")) {
                        display->ShowBranchHint("↩ 请求回退", false);
                    }
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (emotion != NULL) {
                Schedule([this, emotion_str = std::string(emotion->valuestring)]() {
                    auto display = Board::GetInstance().GetDisplay();
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "iot") == 0) {
            auto commands = cJSON_GetObjectItem(root, "commands");
            if (commands != NULL) {
                auto& thing_manager = iot::ThingManager::GetInstance();
                for (int i = 0; i < cJSON_GetArraySize(commands); ++i) {
                    auto command = cJSON_GetArrayItem(commands, i);
                    thing_manager.Invoke(command);
                }
            }
        } else if (strcmp(type->valuestring, "image_preview") == 0) {
            // ============================================
            // 处理图片预览消息 (语音文生图)
            // ============================================
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();

            auto task_id = cJSON_GetObjectItem(root, "task_id");
            auto preview_url = cJSON_GetObjectItem(root, "preview_url");
            auto bitmap_url = cJSON_GetObjectItem(root, "bitmap_url");
            auto width = cJSON_GetObjectItem(root, "width");
            auto height = cJSON_GetObjectItem(root, "height");
            auto checksum = cJSON_GetObjectItem(root, "checksum");
            auto prompt = cJSON_GetObjectItem(root, "prompt");

            if (!task_id || !preview_url || !bitmap_url || !width || !height || !checksum) {
                ESP_LOGE(TAG, "Invalid image_preview notification: missing required fields");
                return;
            }

            std::string task_id_str = task_id->valuestring;
            std::string preview_url_str = preview_url->valuestring;
            std::string bitmap_url_str = bitmap_url->valuestring;
            uint16_t width_val = (uint16_t)width->valueint;
            uint16_t height_val = (uint16_t)height->valueint;
            std::string checksum_str = checksum->valuestring;
            std::string prompt_str = prompt ? prompt->valuestring : "";

            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "🖼️ 收到图片预览: %s", task_id_str.c_str());
            ESP_LOGI(TAG, "📝 描述: %s", prompt_str.c_str());
            ESP_LOGI(TAG, "📐 尺寸: %dx%d", width_val, height_val);
            ESP_LOGI(TAG, "========================================");

            // 显示图片预览 (如果Display支持)
            Schedule([this, task_id_str, preview_url_str, bitmap_url_str, 
                      width_val, height_val, checksum_str, prompt_str]() {
                auto& board = Board::GetInstance();
                auto display = board.GetDisplay();

                // 收到图片预览消息（通过 MQTT），播放音效
                // 注意：语音打印的预览是在 ai_printer_lcd_board.cc 中直接处理的
                if (device_state_ == kDeviceStateListening) {
                    SetDeviceState(kDeviceStateSpeaking);
                }
                auto codec = board.GetAudioCodec();
                if (!codec->output_enabled()) {
                    codec->EnableOutput(true);
                }
                PlaySound(Lang::Sounds::P3_IMAGE_GENERATED);

                // 尝试在LCD上显示预览
                bool preview_shown = display->ShowImagePreview(
                    preview_url_str.c_str(),
                    prompt_str.c_str(),
                    [this, task_id_str](bool success) {
                        ESP_LOGI(TAG, "Image preview loaded: %s", success ? "OK" : "FAILED");
                    }
                );

                if (preview_shown) {
                    display->ShowPreviewConfirmHint("按BOOT确认打印");
                } else {
                    // 不支持图片预览的Display，显示文字提示
                    display->SetChatMessage("system", prompt_str.c_str());
                    display->ShowNotification("按BOOT确认打印");
                }

                // 保存待打印任务信息，等待BOOT确认
                // preview_url_str 是 RGB565 格式的 480x480 预览图
                board.HandleImagePreviewForPrint(
                    task_id_str, bitmap_url_str, width_val, height_val, checksum_str,
                    [this, task_id_str](const std::string& status, const std::string& error) {
                        // 发送打印结果到服务器
                        protocol_->SendPrintResult(task_id_str, status, error);
                    },
                    preview_url_str
                );
            });
        } else if (strcmp(type->valuestring, "preview_update") == 0) {
            // ============================================
            // 仅更新 LCD 预览图，不影响待打印任务
            // 用于后台上色完成后热更新 LCD 显示
            // ============================================
            auto task_id = cJSON_GetObjectItem(root, "task_id");
            auto preview_url = cJSON_GetObjectItem(root, "preview_url");

            if (!task_id || !preview_url) {
                ESP_LOGE(TAG, "Invalid preview_update: missing fields");
                return;
            }

            std::string task_id_str = task_id->valuestring;
            std::string preview_url_str = preview_url->valuestring;

            ESP_LOGI(TAG, "🎨 Received preview_update for task %s", task_id_str.c_str());

            Schedule([this, task_id_str, preview_url_str]() {
                auto& board = Board::GetInstance();
                board.HandlePreviewUpdate(task_id_str, preview_url_str);
            });
        } else if (strcmp(type->valuestring, "print") == 0) {
            // 处理打印通知消息
            auto& board = Board::GetInstance();
            if (!board.SupportsPrinting()) {
                ESP_LOGW(TAG, "Received print notification but board doesn't support printing");
                return;
            }

            auto task_id = cJSON_GetObjectItem(root, "task_id");
            auto url = cJSON_GetObjectItem(root, "url");
            auto width = cJSON_GetObjectItem(root, "width");
            auto height = cJSON_GetObjectItem(root, "height");
            auto checksum = cJSON_GetObjectItem(root, "checksum");
            auto preview_url = cJSON_GetObjectItem(root, "preview_url");

            if (!task_id || !url || !width || !height || !checksum) {
                ESP_LOGE(TAG, "Invalid print notification: missing required fields");
                return;
            }

            std::string task_id_str = task_id->valuestring;
            std::string url_str = url->valuestring;
            uint16_t width_val = (uint16_t)width->valueint;
            uint16_t height_val = (uint16_t)height->valueint;
            std::string checksum_str = checksum->valuestring;
            std::string preview_url_str = (preview_url && preview_url->valuestring) ? preview_url->valuestring : "";

            ESP_LOGI(TAG, "Print notification received: task=%s, size=%dx%d",
                     task_id_str.c_str(), width_val, height_val);
            if (!preview_url_str.empty()) {
                ESP_LOGI(TAG, "  preview_url: %s", preview_url_str.c_str());
            }

            // 在后台任务中执行打印，避免阻塞主循环
            Schedule([this, task_id_str, url_str, width_val, height_val, checksum_str, preview_url_str]() {
                auto& board = Board::GetInstance();
                board.HandlePrintNotification(
                    task_id_str, url_str, width_val, height_val, checksum_str,
                    [this, task_id_str](const std::string& status, const std::string& error) {
                        // 发送打印结果到服务器
                        protocol_->SendPrintResult(task_id_str, status, error);
                    },
                    preview_url_str
                );
            });
        }
    });
    protocol_->Start();

    // 上电即连：自动建立音频通道并进入聆听模式
    Schedule([this]() {
        SetDeviceState(kDeviceStateConnecting);
        if (!protocol_->OpenAudioChannel()) {
            // 失败时，内部重连机制会处理；此处无需额外动作
        }
    });

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_.Initialize(codec, realtime_chat_enabled_);
    audio_processor_.OnOutput([this](std::vector<int16_t>&& data) {
        background_task_->Schedule([this, data = std::move(data)]() mutable {
            opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                Schedule([this, opus = std::move(opus)]() {
                    protocol_->SendAudio(opus);
                });
            });
        });
    });
    audio_processor_.OnVadStateChange([this](bool speaking) {
        if (device_state_ == kDeviceStateListening) {
            Schedule([this, speaking]() {
                if (speaking) {
                    voice_detected_ = true;
                } else {
                    voice_detected_ = false;
                }
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            });
        }
    });
#endif

#if CONFIG_USE_WAKE_WORD_DETECT
    wake_word_detect_.Initialize(codec);
    wake_word_detect_.OnWakeWordDetected([this](const std::string& wake_word) {
        Schedule([this, &wake_word]() {
            // Update welcome page hint to show waking status
            {
                auto display = Board::GetInstance().GetDisplay();
                display->SetWelcomeWakeHint("正在唤醒...");
            }
            if (device_state_ == kDeviceStateSpeaking) {
                // 唤醒词触发打断：正在播报时检测到唤醒词，打断当前播报
                AbortSpeaking(kAbortReasonWakeWordDetected);
            } else if (device_state_ == kDeviceStateActivating) {
                SetDeviceState(kDeviceStateIdle);
            } else {
                // 其他状态（Idle/Listening）：处理唤醒，进入对话模式
                if (!protocol_ || !protocol_->IsAudioChannelOpened()) {
                    // 若未连接，由上电即连逻辑负责建立连接。此处仅回退：尝试一次。
                    SetDeviceState(kDeviceStateConnecting);
                    protocol_->OpenAudioChannel();
                }
                // 发送唤醒词数据（可选）
                wake_word_detect_.EncodeWakeWordData();
                std::vector<uint8_t> opus;
                while (wake_word_detect_.GetWakeWordOpus(opus)) {
                    protocol_->SendAudio(opus);
                }
                // 通知后端：唤醒词文本（兼容老服务端），并显式请求开启对话模式
                protocol_->SendWakeWordDetected(wake_word);
                // 立即发送一次空包，帮助后端刷新 VAD 窗口，尽快结束上一段
                std::vector<uint8_t> empty;
                protocol_->SendAudio(empty);
                // @@@wake-event-only - Do not force conversation mode; stream audio and let backend decide.
                // 保持进入"聆听态"以持续上行音频，由后端判定是否进入对话态
                ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
                SetListeningMode(realtime_chat_enabled_ ? kListeningModeRealtime : kListeningModeAutoStop);
            }
        });
    });
    // ⚠️ 【方案A】不在初始化时启动本地唤醒词检测
    // 原因：采用后端唤醒词检测方案，避免 Wake Word Detection 和 Audio Processor 冲突
    // wake_word_detect_.StartDetection(); // 注释掉，改由 UpdateAfeStrategy() 根据状态决定
    ESP_LOGI(TAG, "Wake Word Detection initialized (backend wake word detection mode)");
#endif

    // Wait for the new version check to finish
    xEventGroupWaitBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);
    SetDeviceState(kDeviceStateIdle);
    std::string message = std::string(Lang::Strings::VERSION) + ota_.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
    // Play the success sound to indicate the device is ready
    ResetDecoder();
    PlaySound(Lang::Sounds::P3_SUCCESS);
    // After device is ready, if we have wake words, update welcome hint
#if CONFIG_USE_WAKE_WORD_DETECT
    {
        auto words = wake_word_detect_.GetConfiguredWakeWords();
        if (!words.empty()) {
            std::string joined; size_t max_items = words.size() > 2 ? 2 : words.size();
            for (size_t i = 0; i < max_items; ++i) { if (i > 0) joined += "/"; joined += words[i]; }
            if (words.size() > max_items) joined += "/…";
            std::string hint = std::string("说出“") + joined + "”唤醒设备";
            Schedule([hint]() {
                auto d = Board::GetInstance().GetDisplay();
                d->SetWelcomeWakeHint(hint.c_str());
            });
        }
    }
#endif

    // Enter the main event loop
    MainEventLoop();
}

void Application::OnClockTimer() {
    clock_ticks_++;

    // Print memory info every 30 seconds (elevated to INFO for stability monitoring)
    if (clock_ticks_ % 30 == 0) {
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        int free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "📊 Heap: internal=%dKB (min=%dKB), PSRAM=%dKB", 
                 free_sram / 1024, min_free_sram / 1024, free_spiram / 1024);
        
        // 内存不足警告（内部 RAM < 30KB 时告警）
        if (free_sram < 30 * 1024) {
            ESP_LOGW(TAG, "⚠️ LOW MEMORY WARNING: internal=%d bytes, min=%d bytes", free_sram, min_free_sram);
        }

        // If we have synchronized server time, set the status to clock "HH:MM" if the device is idle
        if (ota_.HasServerTime()) {
            if (device_state_ == kDeviceStateIdle) {
                    Schedule([this]() {
                    // Set status to clock "HH:MM"
                    time_t now = time(NULL);
                    char time_str[64];
                    strftime(time_str, sizeof(time_str), "%H:%M  ", localtime(&now));
                    Board::GetInstance().GetDisplay()->SetStatus(time_str);
                });
            }
        }
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, SCHEDULE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & SCHEDULE_EVENT) {
            std::unique_lock<std::mutex> lock(mutex_);
            std::list<std::function<void()>> tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }
    }
}

// The Audio Loop is used to input and output audio data
void Application::AudioLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();
    static int check_counter = 0;
    while (true) {
        OnAudioInput();
        
        bool has_data = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            has_data = !audio_decode_queue_.empty();
        }
        
        // 当有音频数据但输出未启用时打印警告
        if (has_data && !codec->output_enabled()) {
            if (++check_counter <= 5) {
                ESP_LOGW(TAG, "AudioLoop: has data but output_enabled=false!");
            }
        }
        
        if (codec->output_enabled()) {
            check_counter = 0;
            OnAudioOutput();
        }
    }
}

void Application::OnAudioOutput() {
    auto now = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int max_silence_seconds = 10;

    std::unique_lock<std::mutex> lock(mutex_);
    
    // 调试：队列有数据时打印
    static int output_counter = 0;
    if (!audio_decode_queue_.empty()) {
        if (++output_counter <= 5 || output_counter % 20 == 0) {
            ESP_LOGI(TAG, "OnAudioOutput: processing, queue=%zu", audio_decode_queue_.size());
        }
    }
    
    if (audio_decode_queue_.empty()) {
        output_counter = 0;
        // Disable the output if there is no audio data for a long time
        if (device_state_ == kDeviceStateIdle) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_output_time_).count();
            if (duration > max_silence_seconds) {
                codec->EnableOutput(false);
            }
        }
        return;
    }

    if (device_state_ == kDeviceStateListening) {
        ESP_LOGW(TAG, "OnAudioOutput: clearing queue (state=Listening), size=%zu", audio_decode_queue_.size());
        audio_decode_queue_.clear();
        audio_decode_cv_.notify_all();
        return;
    }

        auto opus = std::move(audio_decode_queue_.front());
        audio_decode_queue_.pop_front();
        lock.unlock();
        audio_decode_cv_.notify_all();

        background_task_->Schedule([this, codec, opus = std::move(opus)]() mutable {
            static int bg_task_count = 0;
            bg_task_count++;
            
            if (aborted_) {
                if (bg_task_count <= 3) {
                    ESP_LOGW(TAG, "OnAudioOutput: aborted=true, skipping");
                }
                return;
            }

            std::vector<int16_t> pcm;
            if (!opus_decoder_->Decode(std::move(opus), pcm)) {
                if (bg_task_count <= 3) {
                    ESP_LOGE(TAG, "OnAudioOutput: Decode failed!");
                }
                return;
            }
            
            if (bg_task_count <= 3) {
                ESP_LOGI(TAG, "OnAudioOutput: Decoded %zu samples, decoder_rate=%d, codec_rate=%d", 
                         pcm.size(), opus_decoder_->sample_rate(), codec->output_sample_rate());
            }
            
            // Resample if the sample rate is different
            if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
                int target_size = output_resampler_.GetOutputSamples(pcm.size());
                std::vector<int16_t> resampled(target_size);
                output_resampler_.Process(pcm.data(), pcm.size(), resampled.data());
                pcm = std::move(resampled);
                if (bg_task_count <= 3) {
                    ESP_LOGI(TAG, "OnAudioOutput: Resampled to %zu samples", pcm.size());
                }
            }
            
            codec->OutputData(pcm);
            last_output_time_ = std::chrono::steady_clock::now();
    });
}

void Application::OnAudioInput() {
    // ========== 路径0：语音打印录音时，暂停 AudioLoop 的音频读取 ==========
    if (voice_printing_active_) {
        vTaskDelay(pdMS_TO_TICKS(10));  // 让出 CPU，避免空转
        return;
    }
    
    // ========== 路径1：Wake Word Detection 运行 ==========
#if CONFIG_USE_WAKE_WORD_DETECT
    {
        bool running = wake_word_detect_.IsDetectionRunning();
        if (running) {
            size_t feed = wake_word_detect_.GetFeedSize();
            if (feed == 0) feed = 512; // 安全兜底
            std::vector<int16_t> data;
            ReadAudio(data, 16000, (int)feed);
            // 二次确认，避免 StopDetection 窗口期
            if (wake_word_detect_.IsDetectionRunning()) {
                wake_word_detect_.Feed(data);
            }
            // Debug log every 100 calls to avoid spam
            static int call_count = 0;
            if (++call_count % 100 == 0) {
                const char* mode_str = (listening_mode_ == kListeningModeRealtime) ? "realtime" :
                                      (listening_mode_ == kListeningModeAutoStop) ? "auto" : "manual";
                ESP_LOGD(TAG, "Wake word detection running (state=%s, mode=%s, tts=%d)",
                         STATE_STRINGS[device_state_], mode_str, is_tts_playing_);
            }
            // 保证让出 CPU，防止长时间占用触发看门狗
            vTaskDelay(pdMS_TO_TICKS(1));
            return;  // ✅ 立即返回，保持固定节奏
        }
    }
#endif

    // ========== 路径2：Audio Processor 运行 ==========
#if CONFIG_USE_AUDIO_PROCESSOR
    if (audio_processor_.IsRunning()) {
        std::vector<int16_t> data;
        ReadAudio(data, 16000, audio_processor_.GetFeedSize());
        audio_processor_.Feed(data);

        // ✅ Audio Processor 会通过 OnOutput() 回调自动编码并上传音频
        // 在初始化时已设置：audio_processor_.OnOutput([this](data) { encode & send })
        // VAD 会判断是否有语音，只有有语音时才会触发回调
        // 无论 Listening 状态还是 Speaking 状态，都使用相同的回调机制

        return;  // ✅ 立即返回，保持固定节奏
    }
#endif

    // ========== 路径3：直接编码上传（Realtime对话中，Speaking状态） ==========
    // ⚠️ 注意：Listening状态现在由路径2（Audio Processor + VAD）处理
    // 这个路径现在只用于：Speaking (Realtime) 状态的连续对话
    if (listening_mode_ == kListeningModeRealtime && device_state_ == kDeviceStateSpeaking) {
        std::vector<int16_t> data;
        ReadAudio(data, 16000, 30 * 16000 / 1000);

        // Simple energy-based VAD for barge-in when speaking
        if (device_state_ == kDeviceStateSpeaking && listening_mode_ == kListeningModeRealtime && !aborted_) {
            // 若禁止远端打断，则跳过能量判定
            if (!GetAllowRemoteBargeIn()) {
                goto encode_stream;
            }
            long long energy = 0;
            const int stride = 8; // downsample for speed
            for (size_t i = 0; i < data.size(); i += stride) {
                int v = data[i];
                energy += (long long)(v >= 0 ? v : -v);
                if (energy > 4000000) break; // early exit
            }
            if (energy > 4000000) {
                AbortSpeaking(kAbortReasonNone);
            }
        }
encode_stream:
        background_task_->Schedule([this, data = std::move(data)]() mutable {
            opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                Schedule([this, opus = std::move(opus)]() {
                    protocol_->SendAudio(opus);
                });
            });
        });
        return;  // ✅ 立即返回
    }

    // ========== 其他情况：空闲等待 ==========
    vTaskDelay(pdMS_TO_TICKS(30));
}

void Application::ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples) {
    auto codec = Board::GetInstance().GetAudioCodec();
    int channels = codec->input_channels();
    
    if (codec->input_sample_rate() != sample_rate) {
        // 计算需要读取的原始样本数（考虑通道数）
        // samples 是请求的单声道样本数
        // 需要读取 samples * (input_rate / output_rate) * channels 个 int16_t
        int raw_samples_per_channel = samples * codec->input_sample_rate() / sample_rate;
        data.resize(raw_samples_per_channel * channels);
        
        if (!codec->InputData(data)) {
            return;
        }
        
        if (channels == 2) {
            // 分离双通道交错数据
            auto mic_channel = std::vector<int16_t>(raw_samples_per_channel);
            auto reference_channel = std::vector<int16_t>(raw_samples_per_channel);
            for (int i = 0; i < raw_samples_per_channel; ++i) {
                mic_channel[i] = data[i * 2];
                reference_channel[i] = data[i * 2 + 1];
            }
            
            // 重采样
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            
            // 重新交错
            data.resize(resampled_mic.size() * 2);
            for (size_t i = 0; i < resampled_mic.size(); ++i) {
                data[i * 2] = resampled_mic[i];
                data[i * 2 + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        // 采样率相同，直接读取
        data.resize(samples * channels);
        if (!codec->InputData(data)) {
            return;
        }
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;

    // 【关键修复】立即清空音频队列，实现无感打断
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t queue_size = audio_decode_queue_.size();
        audio_decode_queue_.clear();
        audio_decode_cv_.notify_all();
        if (queue_size > 0) {
            ESP_LOGI(TAG, "Cleared %zu audio frames from queue for instant interrupt", queue_size);
        }
    }

    protocol_->SendAbortSpeaking(reason);
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

// ========== 智能AFE策略管理 ==========

void Application::UpdateAfeStrategy() {
    // 判断应该使用哪个AFE组件
    bool use_audio_processor = ShouldUseAudioProcessor();
    bool use_wake_word = ShouldUseWakeWordDetection();

    const char* mode_str = (listening_mode_ == kListeningModeRealtime) ? "realtime" :
                          (listening_mode_ == kListeningModeAutoStop) ? "auto" : "manual";

#if CONFIG_USE_AUDIO_PROCESSOR
    if (use_audio_processor) {
        if (!audio_processor_.IsRunning()) {
            // 需要启动Audio Processor
            audio_processor_.Start();
            ESP_LOGI(TAG, "[AFE-SWITCH] Started Audio Processor (state=%s, mode=%s, tts=%d)",
                     STATE_STRINGS[device_state_], mode_str, is_tts_playing_);
        }
    } else {
        if (audio_processor_.IsRunning()) {
            // 需要停止Audio Processor
            audio_processor_.Stop();
            ESP_LOGI(TAG, "[AFE-SWITCH] Stopped Audio Processor (state=%s, mode=%s, tts=%d)",
                     STATE_STRINGS[device_state_], mode_str, is_tts_playing_);
        }
    }
#endif

#if CONFIG_USE_WAKE_WORD_DETECT
    if (use_wake_word) {
        if (!wake_word_detect_.IsDetectionRunning()) {
            // 需要启动Wake Word Detection
            wake_word_detect_.StartDetection();
            ESP_LOGI(TAG, "[AFE-SWITCH] Started Wake Word Detection (state=%s, mode=%s, tts=%d)",
                     STATE_STRINGS[device_state_], mode_str, is_tts_playing_);
        }
    } else {
        if (wake_word_detect_.IsDetectionRunning()) {
            // 需要停止Wake Word Detection
            wake_word_detect_.StopDetection();
            ESP_LOGI(TAG, "[AFE-SWITCH] Stopped Wake Word Detection (state=%s, mode=%s, tts=%d)",
                     STATE_STRINGS[device_state_], mode_str, is_tts_playing_);
        }
    }
#endif
}

bool Application::ShouldUseAudioProcessor() const {
    // 使用Audio Processor的条件：
    // 1. 在Listening状态（录音笔模式，需要VAD过滤，只在有语音时上传）
    // 2. 或者在Speaking状态 + Realtime模式 + TTS不在播放

    if (device_state_ == kDeviceStateListening) {
        // ✅ Listening状态：使用Audio Processor进行VAD过滤
        // 目的：只在检测到语音时录制和上传音频，节省带宽
        // Audio Processor提供AEC（可选）、NS、VAD功能
        return true;
    }

    if (device_state_ == kDeviceStateSpeaking &&
        listening_mode_ == kListeningModeRealtime &&
        !is_tts_playing_) {
        // ✅ Speaking + Realtime：使用Audio Processor进行AEC/NS/VAD
        return true;
    }

    return false;
}

bool Application::ShouldUseWakeWordDetection() const {
    // 使用Wake Word Detection的条件：
    // 1. 在Idle状态（待机，等待唤醒），或
    // 2. 在Speaking状态但TTS正在播放（用户可能用唤醒词打断），或
    // 3. 在Speaking状态但非Realtime模式（非连续对话）
    //
    // ⚠️ 重要：Listening状态（录音笔模式）不使用本地唤醒词检测！
    // 原因：Listening状态的主要目的是持续上传音频数据到后端
    // 如果本地运行Wake Word Detection，音频数据会被消耗，无法上传
    // 解决方案：由后端进行唤醒词检测，检测到后发送detect消息给硬件

    if (device_state_ == kDeviceStateIdle) {
        return true;  // Idle状态，启动唤醒词检测（快速响应）
    }

    if (device_state_ == kDeviceStateListening) {
        // ❌ Listening状态：不使用本地唤醒词检测
        // 音频数据需要上传到后端进行"常开聆听入库"
        // 唤醒词由后端检测
        return false;
    }

    if (device_state_ == kDeviceStateSpeaking) {
        // Speaking状态下的细分判断
        if (is_tts_playing_) {
            return true;  // TTS播放期间，允许唤醒词打断
        }
        if (listening_mode_ != kListeningModeRealtime) {
            return true;  // 非Realtime模式，使用唤醒词
        }
    }

    return false;
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }

    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;

    const char* mode_str = (listening_mode_ == kListeningModeRealtime) ? "realtime" :
                          (listening_mode_ == kListeningModeAutoStop) ? "auto" : "manual";
    ESP_LOGI(TAG, "STATE: %s (listening_mode=%s, tts_playing=%d)",
             STATE_STRINGS[device_state_], mode_str, is_tts_playing_);

    // The state is changed, wait for all background tasks to finish
    background_task_->WaitForCompletion();

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            Board::GetInstance().SetPowerSaveMode(true);

            // 使用统一的AFE策略更新
            UpdateAfeStrategy();
            break;

        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;

        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
            Board::GetInstance().SetPowerSaveMode(false);

            // Update the IoT states before sending the start listening command
            UpdateIotStates();

            // 使用统一的AFE策略更新
            // ⚠️ 在 Listening 状态，ShouldUseWakeWordDetection() 返回 false
            // 所以会停止本地 Wake Word Detection，音频数据直接编码上传到后端
            // 后端通过 ListeningRecorder 的 ASR 识别结果检测唤醒词
            UpdateAfeStrategy();

            // Send the start listening command
            protocol_->SendStartListening(listening_mode_);
            if (listening_mode_ == kListeningModeAutoStop && previous_state == kDeviceStateSpeaking) {
                // FIXME: Wait for the speaker to empty the buffer
                vTaskDelay(pdMS_TO_TICKS(120));
            }
            opus_encoder_->ResetState();
            break;

        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            Board::GetInstance().SetPowerSaveMode(false);

            // 使用统一的AFE策略更新
            // 如果是Realtime模式且TTS未播放，会启动Audio Processor
            // 否则会启动Wake Word Detection
            UpdateAfeStrategy();

            ResetDecoder();
            break;

        default:
            // 其他状态：默认启动Wake Word Detection
#if CONFIG_USE_AUDIO_PROCESSOR
            if (audio_processor_.IsRunning()) {
                audio_processor_.Stop();
            }
#endif
#if CONFIG_USE_WAKE_WORD_DETECT
            if (!wake_word_detect_.IsDetectionRunning()) {
                wake_word_detect_.StartDetection();
            }
#endif
            break;
    }
}

void Application::ResetDecoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    opus_decoder_->ResetState();
    audio_decode_queue_.clear();
    audio_decode_cv_.notify_all();
    last_output_time_ = std::chrono::steady_clock::now();

    auto codec = Board::GetInstance().GetAudioCodec();
    codec->EnableOutput(true);
}

void Application::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
}

void Application::UpdateIotStates() {
    auto& thing_manager = iot::ThingManager::GetInstance();
    std::string states;
    if (thing_manager.GetStatesJson(states, true)) {
        protocol_->SendIotStates(states);
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void Application::ResetToWelcomeAndReconnect() {
    ESP_LOGI(TAG, "ResetToWelcomeAndReconnect: tear down channels and return to welcome");
    // Stop background tasks and audio
    background_task_->WaitForCompletion();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.clear();
        audio_decode_cv_.notify_all();
    }
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    codec->EnableInput(false);
    codec->EnableOutput(false);

    // Close protocol transports
    if (protocol_) {
        protocol_->CloseAudioChannel();
    }

    // UI: welcome page
    auto display = board.GetDisplay();
    if (display) {
        display->ShowWelcome();
    }

    // Reset state machine and wake word
    SetDeviceState(kDeviceStateIdle);

    // Restart network (STA only; no AP auto-enter). This will block until connected or timeout
    board.StartNetwork();

    // Re-enable audio IO
    codec->EnableInput(true);
    codec->EnableOutput(true);
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (device_state_ == kDeviceStateIdle) {
        // Idle状态：切换到对话模式
        ToggleChatState();
        Schedule([this, wake_word]() {
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word);
            }
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        // Speaking状态（TTS播放中）：打断TTS
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        // ✅ Listening状态：直接进入Speaking对话状态
        ESP_LOGI(TAG, "Wake word detected in Listening mode, entering Speaking state");
        Schedule([this, wake_word]() {
            // 关键修复：直接切换到 Speaking 状态，而不是重新设置 Listening 模式
            // 这样会触发 UpdateAfeStrategy()，根据当前状态选择正确的 AFE
            if (listening_mode_ != kListeningModeRealtime) {
                // 如果当前不是 realtime 模式，先设置为 realtime
                listening_mode_ = kListeningModeRealtime;
            }

            // 直接进入 Speaking 状态
            SetDeviceState(kDeviceStateSpeaking);

            // 发送唤醒词检测事件
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word);
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::PauseAfeComponents() {
    ESP_LOGI(TAG, "Pausing AFE components for external recording...");
    
#if CONFIG_USE_WAKE_WORD_DETECT
    if (wake_word_detect_.IsDetectionRunning()) {
        wake_word_detect_.StopDetection();
        ESP_LOGI(TAG, "  - Stopped Wake Word Detection");
    }
#endif

#if CONFIG_USE_AUDIO_PROCESSOR
    if (audio_processor_.IsRunning()) {
        audio_processor_.Stop();
        ESP_LOGI(TAG, "  - Stopped Audio Processor");
    }
#endif
}

void Application::ResumeAfeComponents() {
    ESP_LOGI(TAG, "Resuming AFE components after external recording...");
    
    // 根据当前状态恢复 AFE 组件
    UpdateAfeStrategy();
}
