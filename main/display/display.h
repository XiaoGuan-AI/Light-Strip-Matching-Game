#ifndef DISPLAY_H
#define DISPLAY_H

#include <lvgl.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <functional>

struct DisplayFonts {
    const lv_font_t* text_font = nullptr;
    const lv_font_t* icon_font = nullptr;
    const lv_font_t* emoji_font = nullptr;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetEmotion(const char* emotion);
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void SetIcon(const char* icon);
    virtual void SetTheme(const std::string& theme_name);
    virtual std::string GetTheme() { return current_theme_name_; }
    // Optional: switch to chat UI when wake word is detected
    virtual void EnterChatMode() {}
    // Optional: called when Wi‑Fi is connected
    virtual void OnNetworkReady() {}
    // Optional: blink red error on top/bottom bars during network outage
    virtual void StartNetworkErrorBlink() {}
    virtual void StopNetworkErrorBlink() {}
    // Switch to welcome/standby page if supported
    virtual void ShowWelcome() {}
    // (removed) Test-only page hook
    virtual void ShowTestPage() {}

    // Optional: branch hint overlay for confirm/retry/rollback guidance
    virtual void ShowBranchHint(const char* text, bool highlight_red = true) {}
    virtual void ClearBranchHint() {}
    // Optional: update only the time label (cheap partial refresh on e-ink)
    virtual void UpdateTimeOnly() {}
    // Optional: set wake hint text on welcome page (if supported)
    virtual void SetWelcomeWakeHint(const char* text) {}

    // ============================================
    // 图片预览功能 (用于语音文生图)
    // ============================================

    /**
     * @brief 显示图片预览
     * @param image_url 图片URL (JPEG/PNG)
     * @param prompt 用户的描述文本
     * @param on_loaded 图片加载完成回调 (可选)
     * @return true 如果支持预览功能
     */
    virtual bool ShowImagePreview(
        const char* image_url,
        const char* prompt,
        std::function<void(bool success)> on_loaded = nullptr) {
        return false;  // 默认不支持
    }

    /**
     * @brief 隐藏图片预览，恢复正常界面
     */
    virtual void HideImagePreview() {}

    /**
     * @brief 检查是否正在显示预览
     */
    virtual bool IsShowingPreview() const { return false; }

    /**
     * @brief 显示预览确认提示
     * @param message 提示消息 (如 "按BOOT确认打印")
     */
    virtual void ShowPreviewConfirmHint(const char* message) {}

    inline int width() const { return width_; }
    inline int height() const { return height_; }

protected:
    int width_ = 0;
    int height_ = 0;
    
    esp_pm_lock_handle_t pm_lock_ = nullptr;
    lv_display_t *display_ = nullptr;

    lv_obj_t *emotion_label_ = nullptr;
    lv_obj_t *network_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *notification_label_ = nullptr;
    lv_obj_t *mute_label_ = nullptr;
    lv_obj_t *battery_label_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;
    lv_obj_t* low_battery_label_ = nullptr;
    
    const char* battery_icon_ = nullptr;
    const char* network_icon_ = nullptr;
    bool muted_ = false;
    std::string current_theme_name_;

    esp_timer_handle_t notification_timer_ = nullptr;
    esp_timer_handle_t update_timer_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;

    virtual void Update();
};


class DisplayLockGuard {
public:
    DisplayLockGuard(Display *display) : display_(display) {
        if (!display_->Lock(3000)) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        display_->Unlock();
    }

private:
    Display *display_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override {
        return true;
    }
    virtual void Unlock() override {}
};

#endif
