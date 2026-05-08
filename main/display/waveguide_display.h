#ifndef WAVEGUIDE_DISPLAY_H
#define WAVEGUIDE_DISPLAY_H

#include "display.h"
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class WaveguideDisplay : public Display {
public:
    WaveguideDisplay();
    ~WaveguideDisplay();

    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string &notification, int duration_ms = 3000) override { ShowNotification(notification.c_str(), duration_ms); }
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void EnterChatMode() override;
    void OnNetworkReady() override;
    void StartNetworkErrorBlink() override;
    void StopNetworkErrorBlink() override;
    void ShowWelcome() override;
    void ShowBranchHint(const char* text, bool highlight_red = true) override;
    void ClearBranchHint() override;

protected:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;
    void Update() override;

private:
    void InitLvgl();
    static void LvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);
    static void CommitTimerThunk(void* arg);
    void CommitDirty();
    void ScheduleCommit(uint64_t delay_us);
    void BuildSimpleFrameWithLabel(const char* text, lv_obj_t** out_label);
    // 强制清屏开关，确保下一次 flush 完整覆盖
    bool force_full_flush_ = false;

    SemaphoreHandle_t mutex_ = nullptr;
    lv_color_t* draw_buf1_ = nullptr;
    lv_obj_t* screen_obj_ = nullptr;
    lv_obj_t* frame_obj_ = nullptr;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* content_area_ = nullptr;
    lv_obj_t* action_bar_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_obj_t* chat_label_ = nullptr;
    esp_timer_handle_t commit_timer_ = nullptr;
    bool dirty_ = false;

    // AR panel sync throttle (time-based, no background SPI)
    uint64_t last_sync_us_ = 0;
};

#endif



