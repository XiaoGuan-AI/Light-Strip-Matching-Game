#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <list>
#include <vector>
#include <condition_variable>

#include <opus_encoder.h>
#include <opus_decoder.h>
#include <opus_resampler.h>

#include "protocol.h"
#include "ota.h"
#include "background_task.h"
#include "iot/match_game.h"

#if CONFIG_USE_WAKE_WORD_DETECT
#include "wake_word_detect.h"
#endif
#if CONFIG_USE_AUDIO_PROCESSOR
#include "audio_processor.h"
#endif

#define SCHEDULE_EVENT (1 << 0)
#define AUDIO_INPUT_READY_EVENT (1 << 1)
#define AUDIO_OUTPUT_READY_EVENT (1 << 2)
#define CHECK_NEW_VERSION_DONE_EVENT (1 << 3)

enum DeviceState {
    kDeviceStateUnknown,
    kDeviceStateStarting,
    kDeviceStateWifiConfiguring,
    kDeviceStateIdle,
    kDeviceStateConnecting,
    kDeviceStateListening,
    kDeviceStateSpeaking,
    kDeviceStateUpgrading,
    kDeviceStateActivating,
    kDeviceStateFatalError
};

#define OPUS_FRAME_DURATION_MS 60

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return voice_detected_; }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void UpdateIotStates();
    void Reboot();
    // Reset UI/network/audio stacks to a clean welcome state and reconnect Wi‑Fi
    void ResetToWelcomeAndReconnect();
    void WakeWordInvoke(const std::string& wake_word);
    void PlaySound(const std::string_view& sound);
    void PlaySoundEffect(const std::string_view& sound);  // 播放音效（自动初始化解码器）
    bool CanEnterSleepMode();

    // Expose configured wake words (may be empty until WakeWordDetect initializes)
    std::vector<std::string> GetConfiguredWakeWords() const {
#if CONFIG_USE_WAKE_WORD_DETECT
        return wake_word_detect_.GetConfiguredWakeWords();
#else
        return {};
#endif
    }

private:
    Application();
    ~Application();

#if CONFIG_USE_WAKE_WORD_DETECT
    WakeWordDetect wake_word_detect_;
#endif
#if CONFIG_USE_AUDIO_PROCESSOR
    AudioProcessor audio_processor_;
#endif
    Ota ota_;
    MatchGame match_game_;
    std::mutex mutex_;
    std::list<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
#if CONFIG_USE_REALTIME_CHAT
    bool realtime_chat_enabled_ = true;
#else
    bool realtime_chat_enabled_ = false;
#endif
    bool aborted_ = false;
    bool voice_detected_ = false;
    // Barge-in policy: when false, suppress remote barge-in during TTS (only wake-word/manual allowed)
    bool allow_remote_barge_in_ = false;
    int clock_ticks_ = 0;
    bool resume_listening_after_reconnect_ = false;
    TaskHandle_t check_new_version_task_handle_ = nullptr;

    // TTS播放状态跟踪（用于智能AFE切换）
    volatile bool is_tts_playing_ = false;  // ✅ 默认false：设备启动时没有TTS播放
    std::chrono::steady_clock::time_point last_tts_start_time_;

    // Audio encode / decode
    TaskHandle_t audio_loop_task_handle_ = nullptr;
    BackgroundTask* background_task_ = nullptr;
    std::chrono::steady_clock::time_point last_output_time_;
    std::list<std::vector<uint8_t>> audio_decode_queue_;
    std::condition_variable audio_decode_cv_;

    std::unique_ptr<OpusEncoderWrapper> opus_encoder_;
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;

    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
    OpusResampler output_resampler_;

    void MainEventLoop();
    void OnAudioInput();
    void OnAudioOutput();
    void ResetDecoder();
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckNewVersion();
    void ShowActivationCode();
    void OnClockTimer();
    void SetListeningMode(ListeningMode mode);
    void AudioLoop();

    // 智能AFE策略管理
    void UpdateAfeStrategy();
    bool ShouldUseAudioProcessor() const;
    bool ShouldUseWakeWordDetection() const;

public:
    bool GetAllowRemoteBargeIn() const { return allow_remote_barge_in_; }
    void SetAllowRemoteBargeIn(bool allow) { allow_remote_barge_in_ = allow; }
    
    // 语音打印录音控制 - 当 true 时 AudioLoop 暂停读取麦克风
    bool IsVoicePrintingActive() const { return voice_printing_active_; }
    void SetVoicePrintingActive(bool active) { voice_printing_active_ = active; }
    
    // 暂停/恢复 AFE 组件（用于外部录音时避免资源冲突）
    void PauseAfeComponents();
    void ResumeAfeComponents();
    
    // 公开音频读取接口供 Board 录音使用
    void ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples);
    
private:
    bool voice_printing_active_ = false;
};

#endif // _APPLICATION_H_
