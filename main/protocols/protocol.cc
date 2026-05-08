#include "protocol.h"

#include <esp_log.h>

#define TAG "Protocol"

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudio(std::function<void(std::vector<uint8_t>&& data)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

void Protocol::SendAbortSpeaking(AbortReason reason) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\"";
    if (reason == kAbortReasonWakeWordDetected) {
        message += ",\"reason\":\"wake_word_detected\"";
    }
    message += "}";
    ESP_LOGI(TAG, "[TX][abort] reason=%s sid=%s",
             (reason == kAbortReasonWakeWordDetected ? "wake_word_detected" : "none"),
             session_id_.c_str());
    SendText(message);
}

void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    std::string json = "{\"session_id\":\"" + session_id_ +
                      "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + wake_word + "\"}";
    ESP_LOGI(TAG, "[TX][listen.detect] text=%s sid=%s", wake_word.c_str(), session_id_.c_str());
    SendText(json);
}

void Protocol::SendStartListening(ListeningMode mode) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"listen\",\"state\":\"start\"";
    if (mode == kListeningModeRealtime) {
        message += ",\"mode\":\"realtime\"";
    } else if (mode == kListeningModeAutoStop) {
        message += ",\"mode\":\"auto\"";
    } else {
        message += ",\"mode\":\"manual\"";
    }
    message += "}";
    const char* mode_str = (mode == kListeningModeRealtime) ? "realtime" : (mode == kListeningModeAutoStop) ? "auto" : "manual";
    ESP_LOGI(TAG, "[TX][listen.start] mode=%s sid=%s", mode_str, session_id_.c_str());
    SendText(message);
}

void Protocol::SendStopListening() {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    ESP_LOGI(TAG, "[TX][listen.stop] sid=%s", session_id_.c_str());
    SendText(message);
}

void Protocol::SendIotDescriptors(const std::string& descriptors) {
    cJSON* root = cJSON_Parse(descriptors.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse IoT descriptors: %s", descriptors.c_str());
        return;
    }

    if (!cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "IoT descriptors should be an array");
        cJSON_Delete(root);
        return;
    }

    int arraySize = cJSON_GetArraySize(root);
    for (int i = 0; i < arraySize; ++i) {
        cJSON* descriptor = cJSON_GetArrayItem(root, i);
        if (descriptor == nullptr) {
            ESP_LOGE(TAG, "Failed to get IoT descriptor at index %d", i);
            continue;
        }

        cJSON* messageRoot = cJSON_CreateObject();
        cJSON_AddStringToObject(messageRoot, "session_id", session_id_.c_str());
        cJSON_AddStringToObject(messageRoot, "type", "iot");
        cJSON_AddBoolToObject(messageRoot, "update", true);

        cJSON* descriptorArray = cJSON_CreateArray();
        cJSON_AddItemToArray(descriptorArray, cJSON_Duplicate(descriptor, 1));
        cJSON_AddItemToObject(messageRoot, "descriptors", descriptorArray);

        char* message = cJSON_PrintUnformatted(messageRoot);
        if (message == nullptr) {
            ESP_LOGE(TAG, "Failed to print JSON message for IoT descriptor at index %d", i);
            cJSON_Delete(messageRoot);
            continue;
        }

        SendText(std::string(message));
        cJSON_free(message);
        cJSON_Delete(messageRoot);
    }

    cJSON_Delete(root);
}

void Protocol::SendIotStates(const std::string& states) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"iot\",\"update\":true,\"states\":" + states + "}";
    SendText(message);
}

bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 180;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGW(TAG, "Channel timeout %lld seconds (no action)", duration.count());
    }
    return false; // 仅记录，不作为断线依据
}



void Protocol::SendModeCommand(const std::string& action) {
    // 同步显式模式切换到后端
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"mode\",\"action\":\"" + action + "\"}";
    ESP_LOGI(TAG, "[TX][mode] action=%s sid=%s", action.c_str(), session_id_.c_str());
    SendText(message);
}

void Protocol::SendPrintResult(const std::string& task_id, const std::string& status, const std::string& error) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"print_result\"";
    message += ",\"task_id\":\"" + task_id + "\"";
    message += ",\"status\":\"" + status + "\"";
    if (!error.empty()) {
        message += ",\"error\":\"" + error + "\"";
    }
    message += "}";
    ESP_LOGI(TAG, "[TX][print_result] task=%s status=%s", task_id.c_str(), status.c_str());
    SendText(message);
}

void Protocol::SendVoicePrintCommand(const std::string& text, bool is_final) {
    // 发送语音识别结果到后端，用于语音打印流程
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"voice_print\"";
    message += ",\"text\":\"" + text + "\"";
    message += ",\"is_final\":" + std::string(is_final ? "true" : "false");
    message += "}";
    ESP_LOGI(TAG, "[TX][voice_print] text=%s final=%d", text.c_str(), is_final);
    SendText(message);
}

void Protocol::SendVoiceSessionControl(const std::string& control_type, const std::string& wake_word) {
    // 发送语音会话控制消息
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"voice_session\"";
    message += ",\"control\":\"" + control_type + "\"";
    if (!wake_word.empty()) {
        message += ",\"wake_word\":\"" + wake_word + "\"";
    }
    message += "}";
    ESP_LOGI(TAG, "[TX][voice_session] control=%s wake=%s", control_type.c_str(), wake_word.c_str());
    SendText(message);
}