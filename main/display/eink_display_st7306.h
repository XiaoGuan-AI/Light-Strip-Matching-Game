#ifndef EINK_DISPLAY_ST7306_H
#define EINK_DISPLAY_ST7306_H

#include "display.h"
#include "display/st7306_gfx.h"
#include "st7306_driver.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string>
#include <vector>
#include <stdint.h>
#include <lvgl.h>
#include <esp_timer.h>

class EinkDisplayST7306 : public Display {
public:
    explicit EinkDisplayST7306(st7306_device_t* dev);
    virtual ~EinkDisplayST7306();

    virtual void SetStatus(const char* status) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void EnterChatMode() override { SwitchPage(kPageChat); }
    virtual void OnNetworkReady() override;
    virtual void StartNetworkErrorBlink() override;
    virtual void StopNetworkErrorBlink() override;
    virtual void ShowWelcome() override { SwitchPage(kPageWelcome); }
    virtual void SetWelcomeWakeHint(const char* text) override {
        if (Lock(200)) {
            if (welcome_wake_hint_label_) {
                lv_label_set_text(welcome_wake_hint_label_, text ? text : "");
                // 移除同步刷新，避免在唤醒词回调中阻塞10秒导致看门狗超时
                // lv_refr_now(NULL);
            }
            Unlock();
        }
    }
    // Test-only hook removed; keep empty override to avoid link-time surprises
    virtual void ShowTestPage() override {}
    // Branch hint overlay
    virtual void ShowBranchHint(const char* text, bool highlight_red = true) override;
    virtual void ClearBranchHint() override;
    // Minimal hooks used by board timers
    void SetTimeSyncCompleted();
    void UpdateTimeOnly();
    // Update top-bar indicators
    void SetBatteryPercent(int percent);
    void SetWifiRssiIcon(int8_t rssi, bool connected);
    // Start scramble animation towards target assistant text
    void StartScramble(const char* text);

protected:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
    virtual void Update() override; // no-op for E-ink

private:
    st7306_device_t* dev_;
    ST7306_GFX gfx_;
    SemaphoreHandle_t mutex_;
    uint64_t last_refresh_us_ = 0; // throttle full-screen refreshes
    // LVGL display objects for E-ink path
    lv_obj_t* screen_obj_ = nullptr;
    lv_obj_t* content_label_ = nullptr; // floating notification overlay
    lv_obj_t* branch_hint_label_ = nullptr; // overlay for confirm/retry/rollback
    lv_color_t* draw_buf1_ = nullptr;

    // simple chat state
    std::string summary_;
    std::vector<std::string> bullets_;

    // page layout
    enum PageType { kPageWelcome, kPageChat, kPageMeeting, kPageCoding, kPageWorking };
    PageType current_page_ = kPageWelcome;

    // common containers
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* content_area_ = nullptr;
    lv_obj_t* action_bar_ = nullptr;

    // top bar labels
    lv_obj_t* mode_label_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* wifi_text_label_ = nullptr;
    lv_obj_t* battery_text_label_ = nullptr;

    // welcome page labels
    lv_obj_t* welcome_title_label_ = nullptr;
    lv_obj_t* welcome_name_label_ = nullptr;
    lv_obj_t* welcome_sub_label_ = nullptr;
    lv_obj_t* welcome_wake_hint_label_ = nullptr;

    // chat page labels
    lv_obj_t* chat_summary_view_ = nullptr;
    lv_obj_t* chat_summary_label_ = nullptr;
    lv_obj_t* chat_summary_sep_ = nullptr;
    lv_obj_t* chat_summary_text_label_ = nullptr;
    lv_obj_t* chat_history_title_label_ = nullptr;
    lv_obj_t* chat_history_sep_ = nullptr;
    lv_obj_t* chat_bullet_labels_[3] = {nullptr, nullptr, nullptr};

    // meeting page labels
    lv_obj_t* meeting_topic_label_ = nullptr;
    lv_obj_t* meeting_duration_label_ = nullptr;
    lv_obj_t* meeting_status_label_ = nullptr;
    lv_obj_t* meeting_transcript_title_ = nullptr;
    lv_obj_t* meeting_transcript_label_ = nullptr;

    // coding page labels
    lv_obj_t* coding_state_label_ = nullptr;
    lv_obj_t* coding_prompt_label_ = nullptr;
    lv_obj_t* coding_transcript_title_ = nullptr;
    lv_obj_t* coding_transcript_label_ = nullptr;

    // working page labels
    lv_obj_t* working_header_label_ = nullptr;
    lv_obj_t* working_task_title_ = nullptr;
    lv_obj_t* working_task_labels_[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* working_reminder_title_ = nullptr;
    lv_obj_t* working_reminder_labels_[3] = {nullptr, nullptr, nullptr};

    void RenderFrame();
    void RenderFrameWithThrottle(uint64_t min_interval_us);
    bool IsRefreshDue(uint64_t min_interval_us) const;
    void MarkRefreshed();
    void DrawTopBar(const char* mode_label);
    void DrawActionBar(const char* hint);

    void BuildWelcomePage();
    void BuildChatPage();
    void BuildMeetingPage();
    void BuildCodingPage();
    void BuildWorkingPage();
    // (removed) BuildTestPage(); // @@@test-only page builder
    void SwitchPage(PageType page);
    void UpdateChatTexts();
    void UpdateSummaryLabelTextWrapped();

    // E-ink commit coalescing to reduce flicker
    bool dirty_area_valid_ = false;
    lv_area_t dirty_area_{};
    esp_timer_handle_t commit_timer_ = nullptr;
    static void CommitTimerThunk(void* arg);
    void ScheduleCommit(uint64_t delay_us);
    void CommitDirtyArea();

    // Network error blink
    esp_timer_handle_t net_blink_timer_ = nullptr;
    bool net_blink_on_ = false;
    static void NetBlinkTimerThunk(void* arg);
    void UpdateNetBlink();

    // LVGL flush callback
    static void LvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);

    // Scramble animation state
    esp_timer_handle_t scramble_timer_ = nullptr;
    bool scramble_active_ = false;
    int scramble_total_frames_ = 10;
    int scramble_current_frame_ = 0;
    int scramble_period_us_ = 80000;
    std::string scramble_target_;
    static void ScrambleTimerThunk(void* arg);
    void OnScrambleTick();

    // Ensure the very first LVGL frame pushes a full-screen commit to avoid
    // residual content before partial updates begin.
    bool did_first_full_display_ = false;

    // Ghosting control: partial commit counting and periodic full wipe
    uint32_t partial_commits_since_full_ = 0;
    uint64_t last_wipe_us_ = 0;

    // Summary auto-scroll (vertical) when overflow
    esp_timer_handle_t summary_scroll_timer_ = nullptr;
    bool summary_scroll_active_ = false;
    int summary_scroll_y_ = 0;           // current scroll offset (px)
    int summary_view_height_ = 0;        // viewport height (px)
    int summary_scroll_step_px_ = 12;    // step per tick (px)
    static void SummaryScrollTimerThunk(void* arg);
    void OnSummaryScrollTick();
    void UpdateSummaryScrollState();

    // Time display control: apply +16h offset before real time is synced
    bool time_sync_completed_ = false;
    static constexpr int kStartupTimeOffsetSeconds = 16 * 3600; // 16 hours
};

#endif // EINK_DISPLAY_ST7306_H

