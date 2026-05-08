#include "display/eink_display_st7306.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <string.h>
#include <algorithm>
#include "application.h"
#include "board.h"
#include <time.h>

static const char* TAG __attribute__((unused)) = "EinkDisplayST7306";
static bool s_lvgl_inited = false;
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_16_4);

EinkDisplayST7306::EinkDisplayST7306(st7306_device_t* dev)
    : dev_(dev), gfx_(dev), mutex_(xSemaphoreCreateMutex()) {
    ESP_LOGI(TAG, "Display ctor: begin");
    width_ = ST7306_WIDTH; height_ = ST7306_HEIGHT;
    // Initialize LVGL display for e-ink using custom flush
    if (!s_lvgl_inited) { lv_init(); s_lvgl_inited = true; }
    ESP_LOGI(TAG, "LVGL inited, creating display %dx%d", width_, height_);
    // Create display with 1-bit monochrome color depth simulated via LVGL's native color type
    lv_display_t* disp = lv_display_create(width_, height_);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    // Allocate a small draw buffer to satisfy LVGL; we'll ignore its contents and draw via driver in flush
    draw_buf1_ = static_cast<lv_color_t*>(heap_caps_malloc(width_ * 16 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (draw_buf1_) {
        // Use a full-width row buffer to reduce tearing
        lv_display_set_buffers(disp, draw_buf1_, nullptr, width_ * sizeof(lv_color_t) * 16, LV_DISPLAY_RENDER_MODE_PARTIAL);
    }
    lv_display_set_user_data(disp, this);
    lv_display_set_flush_cb(disp, [](lv_display_t* d, const lv_area_t* area, uint8_t* px_map){
        EinkDisplayST7306::LvglFlushCb(d, area, px_map);
    });
    display_ = disp;
    ESP_LOGI(TAG, "LVGL display created, building initial UI");

    // Build initial UI pages
    SwitchPage(kPageWelcome);
    ESP_LOGI(TAG, "Initial page built: welcome");

    // Create commit timer for coalescing partial refreshes
    if (commit_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = &EinkDisplayST7306::CommitTimerThunk,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "eink_commit",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &commit_timer_);
    }

    if (net_blink_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = &EinkDisplayST7306::NetBlinkTimerThunk,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "net_blink",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &net_blink_timer_);
    }

    // Summary scroll timer (created once)
    if (summary_scroll_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = &EinkDisplayST7306::SummaryScrollTimerThunk,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "summary_scroll",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &summary_scroll_timer_);
    }
}

EinkDisplayST7306::~EinkDisplayST7306() {
    if (display_ != nullptr) {
        lv_display_delete(display_);
        display_ = nullptr;
    }
    if (draw_buf1_) { heap_caps_free(draw_buf1_); draw_buf1_ = nullptr; }
    if (mutex_) vSemaphoreDelete(mutex_);
}

bool EinkDisplayST7306::Lock(int timeout_ms) {
    if (!mutex_) return true;
    TickType_t to = timeout_ms==0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(mutex_, to) == pdTRUE;
}
void EinkDisplayST7306::Unlock() { if (mutex_) xSemaphoreGive(mutex_); }

void EinkDisplayST7306::Update() { /* no periodic update for e-ink */ }

void EinkDisplayST7306::SetStatus(const char* status) {
    (void)status;
    if (!Lock(200)) return;
    if (current_page_ == kPageChat && mode_label_) {
        auto state = Application::GetInstance().GetDeviceState();
        const char* mode_text = (state == kDeviceStateListening) ? "聆听中" :
                                (state == kDeviceStateSpeaking)  ? "说话中" :
                                (state == kDeviceStateConnecting) ? "连接中" :
                                (state == kDeviceStateIdle)       ? "待机" : "对话";
        lv_label_set_text(mode_label_, mode_text);
        RenderFrameWithThrottle( (state == kDeviceStateListening || state == kDeviceStateSpeaking) ? 150000ULL : 5000000ULL );
    }
    Unlock();
}

void EinkDisplayST7306::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void EinkDisplayST7306::ShowNotification(const char* notification, int duration_ms) {
    (void)duration_ms;
    if (!Lock(300)) return;
    // Throttle full refresh to once per 5s to avoid flicker
    uint64_t now = esp_timer_get_time();
    if ((now - last_refresh_us_) < 5000000ULL) { Unlock(); return; }
    if (!screen_obj_) { SwitchPage(current_page_); }
    if (!content_label_) {
        content_label_ = lv_label_create(lv_screen_active());
        lv_obj_set_style_text_color(content_label_, lv_color_black(), 0);
        lv_obj_set_style_text_font(content_label_, &font_puhui_16_4, 0);
        lv_obj_set_width(content_label_, width_ - 16);
        lv_obj_set_style_bg_color(content_label_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(content_label_, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(content_label_, 6, 0);
        lv_obj_align(content_label_, LV_ALIGN_CENTER, 0, -6);
    }
    // Ensure font is applied each time
    lv_obj_set_style_text_font(content_label_, &font_puhui_16_4, 0);
    lv_label_set_text(content_label_, notification ? notification : "");
    lv_refr_now(NULL);
    last_refresh_us_ = esp_timer_get_time();
    Unlock();
}

void EinkDisplayST7306::SetEmotion(const char* emotion) {
    (void)emotion; // no-op
}

void EinkDisplayST7306::SetChatMessage(const char* role, const char* content) {
    if (!content) return;
    if (strcmp(role, "assistant") == 0) {
        // Trigger scramble animation towards target summary (do not show full text first)
        StartScramble(content);
    } else if (strcmp(role, "user") == 0) {
        bullets_.push_back(content);
        if (bullets_.size() > 3) bullets_.erase(bullets_.begin());
    } else {
        // system/other: keep existing bullets (do not clear), so items roll forward FIFO
    }
    if (!Lock(300)) return;
    // In realtime we want partial updates; keep min interval small during listening
    auto state = Application::GetInstance().GetDeviceState();
    // 同步状态栏文案（Listening/Conversing）
    if (top_bar_ && mode_label_) {
        lv_label_set_text(mode_label_, (state == kDeviceStateListening) ? "聆听中" : (state == kDeviceStateSpeaking) ? "说话中" : "对话");
    }
    // If assistant sentence_start arrives, Application already calls SetChatMessage immediately.
    // Here we choose smaller throttle (150ms) to be more realtime.
    // Do not force switch to chat page; only update texts if already in chat page
    if (current_page_ == kPageChat) {
        UpdateChatTexts();
    }
    RenderFrameWithThrottle( (state == kDeviceStateListening || state == kDeviceStateSpeaking) ? 150000ULL : 5000000ULL );
    Unlock();
}
void EinkDisplayST7306::StartScramble(const char* text) {
    // @@@scramble - low-cost scramble effect: 10 frames with random glyphs, then final text
    scramble_target_.assign(text ? text : "");
    if (scramble_target_.empty()) return;
    // Clear current summary immediately to avoid flashing final text before animation
    summary_.clear();
    if (current_page_ == kPageChat && chat_summary_text_label_) {
        lv_label_set_text(chat_summary_text_label_, "");
        lv_refr_now(NULL);
    }
    // Slow down to approximate TTS cadence based on visible characters
    // Estimate visible characters (UTF-8)
    size_t nbytes = scramble_target_.size();
    size_t chars = 0; for (size_t i = 0; i < nbytes; ) { unsigned char c = (unsigned char)scramble_target_[i]; size_t step = 1; if ((c & 0x80)==0) step=1; else if ((c & 0xE0)==0xC0 && i+1<nbytes) step=2; else if ((c & 0xF0)==0xE0 && i+2<nbytes) step=3; else if ((c & 0xF8)==0xF0 && i+3<nbytes) step=4; i += step; chars++; }
    uint32_t total_ms = (uint32_t)(chars * 140); // ~140ms/char
    if (total_ms < 600) total_ms = 600;
    if (total_ms > 3000) total_ms = 3000;
    scramble_total_frames_ = (int)std::min<size_t>(8, std::max<size_t>(4, (chars / 10) + 4));
    scramble_period_us_ = (int)((total_ms * 1000u) / (uint32_t)scramble_total_frames_);
    scramble_current_frame_ = 0;
    scramble_active_ = true;
    if (!scramble_timer_) {
        esp_timer_create_args_t args = { .callback = &EinkDisplayST7306::ScrambleTimerThunk, .arg = this, .dispatch_method = ESP_TIMER_TASK, .name = "eink_scramble", .skip_unhandled_events = true };
        esp_timer_create(&args, &scramble_timer_);
    }
    // cadence controlled by scramble_period_us_
    esp_timer_stop(scramble_timer_);
    esp_timer_start_periodic(scramble_timer_, scramble_period_us_);
    // Render the first scramble frame immediately to avoid a blank period
    OnScrambleTick();
}

void EinkDisplayST7306::ScrambleTimerThunk(void* arg) {
    // 将耗时 UI 更新切到应用主循环执行，避免在 esp_timer 线程里阻塞导致 WDT
    auto* self = reinterpret_cast<EinkDisplayST7306*>(arg);
    Application::GetInstance().Schedule([self]() { self->OnScrambleTick(); });
}

static inline char random_scramble_char() {
    static const char* k = "!<>-_[]{}=+*^?#&%$@";
    static uint32_t s = 0xA5A5A5A5u;
    // Simple LCG PRNG to avoid platform-specific RNG dependencies
    s = s * 1664525u + 1013904223u;
    size_t n = strlen(k);
    return k[s % n];
}

// UTF-8 helpers
static inline size_t utf8_next(const char* s, size_t i, size_t n) {
    if (i >= n) return n;
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return i + 1;
    if ((c >> 5) == 0x6 && i + 1 < n) return i + 2;
    if ((c >> 4) == 0xE && i + 2 < n) return i + 3;
    if ((c >> 3) == 0x1E && i + 3 < n) return i + 4;
    return i + 1;
}

static std::string TruncateUtf8ByChars(const std::string& s, size_t max_chars) {
    const char* p = s.c_str(); size_t n = s.size();
    size_t i = 0; size_t count = 0;
    while (i < n && count < max_chars) { i = utf8_next(p, i, n); count++; }
    if (i >= n) return s;
    std::string out = s.substr(0, i);
    out += "…";
    return out;
}

// Insert hard line breaks every max_chars characters to avoid overflow when LVGL cannot wrap (e.g. long CJK/URLs)
static std::string WrapUtf8ByChars(const std::string& s, size_t max_chars) {
    if (max_chars == 0) return s;
    const char* p = s.c_str(); size_t n = s.size();
    size_t i = 0; size_t count = 0; std::string out; out.reserve(s.size() + s.size() / max_chars + 8);
    while (i < n) {
        // Respect existing newlines: reset counter
        if (p[i] == '\n') { out.push_back('\n'); i++; count = 0; continue; }
        size_t next = utf8_next(p, i, n);
        out.append(&p[i], next - i);
        i = next;
        count++;
        if (count >= max_chars) {
            out.push_back('\n');
            count = 0;
        }
    }
    return out;
}

// Check if character is a word boundary (space, punctuation, etc.)
static inline bool is_word_boundary(char c) {
    return c == ' ' || c == '\n' || c == '\t' || c == ',' || c == '.' ||
           c == '!' || c == '?' || c == ';' || c == ':' || c == '-' ||
           c == '(' || c == ')' || c == '[' || c == ']' || c == '\'' || c == '"';
}

// Smart text wrapping: respects word boundaries for Latin text, character count for CJK
static std::string SmartWrapText(const std::string& text, size_t max_chars, bool is_latin) {
    if (max_chars == 0 || text.empty()) return text;

    const char* p = text.c_str();
    size_t n = text.size();
    std::string out;
    out.reserve(text.size() + text.size() / max_chars + 16);

    size_t line_char_count = 0;
    size_t line_start_pos = 0;
    size_t last_word_boundary_pos = 0;
    size_t last_word_boundary_char_count = 0;
    size_t i = 0;

    while (i < n) {
        // Respect existing newlines
        if (p[i] == '\n') {
            out.push_back('\n');
            i++;
            line_char_count = 0;
            line_start_pos = i;
            last_word_boundary_pos = 0;
            last_word_boundary_char_count = 0;
            continue;
        }

        // Get next character
        size_t next = utf8_next(p, i, n);
        bool is_boundary = (next - i == 1) && is_word_boundary(p[i]);

        // For Latin text, track word boundaries
        if (is_latin && is_boundary) {
            last_word_boundary_pos = out.size();
            last_word_boundary_char_count = line_char_count;
        }

        // Append current character
        out.append(&p[i], next - i);
        i = next;
        line_char_count++;

        // Check if we need to wrap
        if (line_char_count >= max_chars) {
            if (is_latin && last_word_boundary_pos > 0 &&
                last_word_boundary_char_count > (max_chars / 2)) {
                // For Latin: wrap at last word boundary if it's not too early in the line
                // Insert newline at the word boundary position
                out.insert(last_word_boundary_pos + 1, 1, '\n');
                // Update position tracking
                line_char_count = line_char_count - last_word_boundary_char_count - 1;
                last_word_boundary_pos = 0;
                last_word_boundary_char_count = 0;
            } else {
                // For CJK or no good boundary found: hard wrap at current position
                out.push_back('\n');
                line_char_count = 0;
                line_start_pos = i;
                last_word_boundary_pos = 0;
                last_word_boundary_char_count = 0;
            }
        }
    }

    return out;
}

// Detect if text is primarily English/Latin (ASCII) or CJK
static inline bool is_primarily_latin(const std::string& text) {
    if (text.empty()) return true;
    size_t ascii_count = 0;
    size_t total_chars = 0;
    for (size_t i = 0; i < text.size(); ) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) {
            // ASCII character (English, numbers, punctuation)
            if (c > 0x20) ascii_count++; // Count printable ASCII
            i++;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            i += 2; // 2-byte UTF-8
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            i += 3; // 3-byte UTF-8 (CJK characters)
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
            i += 4; // 4-byte UTF-8
        } else {
            i++;
        }
        total_chars++;
    }
    // If more than 60% of printable characters are ASCII, consider it primarily Latin
    return total_chars > 0 && (ascii_count * 100 / total_chars) > 60;
}

// Dynamically determine optimal character count based on text language
static inline size_t get_optimal_chars_per_line(const std::string& text) {
    if (is_primarily_latin(text)) {
        // English/Latin: can fit more characters (~8-10px per char)
        return 32;
    } else {
        // Chinese/CJK: larger characters (~14-16px per char)
        return 16;
    }
}

// Update summary label text with hard-wrapped content based on current view width
static inline size_t estimate_max_chars_for_width(int px_width) {
    // Rough estimate: Chinese glyph ~14-16px at 16px font; leave margin
    int denom = 14;
    if (px_width <= 0) return 20;
    int mc = px_width / denom;
    if (mc < 10) mc = 10;
    if (mc > 32) mc = 32;
    return (size_t)mc;
}

void EinkDisplayST7306::UpdateSummaryLabelTextWrapped() {
    if (!chat_summary_text_label_) return;
    // Dynamically adjust character count based on language detection
    bool is_latin = is_primarily_latin(summary_);
    const size_t max_chars = get_optimal_chars_per_line(summary_);
    // Use smart wrapping to respect word boundaries for English
    std::string wrapped = SmartWrapText(summary_, max_chars, is_latin);
    lv_label_set_text(chat_summary_text_label_, wrapped.c_str());
}

void EinkDisplayST7306::OnScrambleTick() {
    if (!scramble_active_) return;
    if (!Lock(100)) return;
    // Build a mixed string: progressively reveal target, others as random
    std::string out;
    out.reserve(scramble_target_.size());
    size_t reveal = (scramble_current_frame_ * scramble_target_.size()) / scramble_total_frames_;
    for (size_t i = 0; i < scramble_target_.size(); ++i) {
        if (i < reveal) out.push_back(scramble_target_[i]);
        else out.push_back(random_scramble_char());
    }
    summary_ = out;
    if (current_page_ == kPageChat && chat_summary_text_label_) {
        // Apply wrapping with dynamic character count based on language
        bool is_latin = is_primarily_latin(summary_);
        const size_t max_chars = get_optimal_chars_per_line(summary_);
        std::string wrapped = SmartWrapText(summary_, max_chars, is_latin);
        lv_label_set_text(chat_summary_text_label_, wrapped.c_str());
        // width may change after style/padding tweaks; recompute width safely
        if (chat_summary_view_) {
            int view_w = lv_obj_get_width(chat_summary_view_);
            int pad_r = lv_obj_get_style_pad_right(chat_summary_view_, 0);
            int text_w = view_w - pad_r - 2;
            if (text_w & 1) text_w -= 1;
            if (text_w > 0) lv_obj_set_width(chat_summary_text_label_, text_w);
        }
        UpdateSummaryScrollState();
    }
    lv_refr_now(NULL);
    Unlock();
    scramble_current_frame_++;
    if (scramble_current_frame_ >= scramble_total_frames_) {
        // finalize to target
        if (Lock(100)) {
            summary_ = scramble_target_;
            if (current_page_ == kPageChat && chat_summary_text_label_) {
                // Apply wrapping with dynamic character count based on language
                bool is_latin = is_primarily_latin(summary_);
                const size_t max_chars = get_optimal_chars_per_line(summary_);
                std::string wrapped = SmartWrapText(summary_, max_chars, is_latin);
                lv_label_set_text(chat_summary_text_label_, wrapped.c_str());
                if (chat_summary_view_) {
                    int view_w = lv_obj_get_width(chat_summary_view_);
                    int pad_r = lv_obj_get_style_pad_right(chat_summary_view_, 0);
                    int text_w = view_w - pad_r - 2;
                    if (text_w & 1) text_w -= 1;
                    if (text_w > 0) lv_obj_set_width(chat_summary_text_label_, text_w);
                }
                UpdateSummaryScrollState();
            }
            lv_refr_now(NULL);
            Unlock();
        }
        scramble_active_ = false;
        if (scramble_timer_) esp_timer_stop(scramble_timer_);
    }
}

void EinkDisplayST7306::SetTimeSyncCompleted() {
    if (!Lock(200)) return;
    time_sync_completed_ = true;
    // Immediately refresh the time label to real time
    if (time_label_) {
        time_t now = time(NULL);
        char time_str[16];
        strftime(time_str, sizeof(time_str), "%H:%M", localtime(&now));
        lv_label_set_text(time_label_, time_str);
    }
    lv_refr_now(NULL);
    Unlock();
}

void EinkDisplayST7306::UpdateTimeOnly() {
    // For minimal implementation we redraw the frame (simple, no partial)
    if (!Lock(200)) return;
    // Update time text if label exists; avoid full rebuild
    if (time_label_) {
        time_t now = time(NULL);
        if (!time_sync_completed_) {
            now += kStartupTimeOffsetSeconds; // apply +16h until sync
        }
        char time_str[16];
        strftime(time_str, sizeof(time_str), "%H:%M", localtime(&now));
        lv_label_set_text(time_label_, time_str);
    }
    lv_refr_now(NULL);
    Unlock();
}

void EinkDisplayST7306::SetBatteryPercent(int percent) {
    if (!Lock(200)) return;
    if (battery_text_label_) {
        if (percent <= 0) {
            lv_label_set_text(battery_text_label_, "插电");
        } else {
            if (percent > 100) percent = 100;
            char buf[8]; snprintf(buf, sizeof(buf), "%d%%", percent);
            lv_label_set_text(battery_text_label_, buf);
        }
    }
    lv_refr_now(NULL);
    Unlock();
}

void EinkDisplayST7306::SetWifiRssiIcon(int8_t rssi, bool connected) {
    if (!Lock(200)) return;
    if (wifi_text_label_) {
        if (!connected) {
            lv_label_set_text(wifi_text_label_, "WiFi- ");
        } else {
            const char* lvl = (rssi >= -60) ? "WiFi3" : (rssi >= -70) ? "WiFi2" : "WiFi1";
            lv_label_set_text(wifi_text_label_, lvl);
        }
    }
    lv_refr_now(NULL);
    Unlock();
}

void EinkDisplayST7306::RenderFrame() {
    // Throttle full refresh to once per 5s to avoid flicker
    uint64_t now = esp_timer_get_time();
    if ((now - last_refresh_us_) < 5000000ULL) { return; }
    // Use LVGL to layout text, then trigger a full driver display from flush
    if (!screen_obj_) { SwitchPage(kPageChat); }
    if (current_page_ == kPageChat) {
        UpdateChatTexts();
    }
    // Ask LVGL to render and then we call driver display in flush callback
    lv_refr_now(NULL);
    last_refresh_us_ = esp_timer_get_time();
}

void EinkDisplayST7306::RenderFrameWithThrottle(uint64_t min_interval_us) {
    uint64_t now = esp_timer_get_time();
    if ((now - last_refresh_us_) < min_interval_us) { return; }
    if (!screen_obj_) { SwitchPage(kPageChat); }
    if (current_page_ == kPageChat) {
        UpdateChatTexts();
    }
    lv_refr_now(NULL);
    last_refresh_us_ = esp_timer_get_time();
}

void EinkDisplayST7306::DrawTopBar(const char* mode_label) {
    (void)mode_label;
}
void EinkDisplayST7306::DrawActionBar(const char* hint) {
    (void)hint;
}

void EinkDisplayST7306::ShowBranchHint(const char* text, bool highlight_red) {
    if (!Lock(200)) return;
    if (!screen_obj_) { SwitchPage(current_page_); }
    // If pointer exists but object was deleted by a page switch, reset it
    if (branch_hint_label_ && !lv_obj_is_valid(branch_hint_label_)) {
        branch_hint_label_ = nullptr;
    }
    if (!branch_hint_label_) {
        branch_hint_label_ = lv_label_create(lv_screen_active());
        lv_obj_set_style_text_font(branch_hint_label_, &font_puhui_16_4, 0);
        lv_obj_set_width(branch_hint_label_, width_ - 16);
        lv_obj_set_style_bg_color(branch_hint_label_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(branch_hint_label_, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(branch_hint_label_, 6, 0);
        lv_obj_align(branch_hint_label_, LV_ALIGN_CENTER, 0, 10);
    }
    lv_obj_clear_flag(branch_hint_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(branch_hint_label_, highlight_red ? lv_color_hex(0xFF0000) : lv_color_black(), 0);
    lv_label_set_text(branch_hint_label_, text ? text : "");
    lv_refr_now(NULL);
    Unlock();
}

void EinkDisplayST7306::ClearBranchHint() {
    if (!Lock(100)) return;
    if (branch_hint_label_) {
        if (lv_obj_is_valid(branch_hint_label_)) {
            // Prefer hide over delete to avoid race with LVGL rendering
            lv_obj_add_flag(branch_hint_label_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(branch_hint_label_, "");
        }
        // Always clear pointer to avoid stale access
        branch_hint_label_ = nullptr;
        lv_refr_now(NULL);
    }
    Unlock();
}

void EinkDisplayST7306::LvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    auto* self = static_cast<EinkDisplayST7306*>(lv_display_get_user_data(disp));
    if (!self || !self->dev_) { lv_display_flush_ready(disp); return; }
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    const uint16_t* src = reinterpret_cast<const uint16_t*>(px_map); // RGB565
    // Statistics for debugging color mapping
    uint32_t cnt_black = 0, cnt_white = 0, cnt_red = 0;
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            uint16_t c565 = src[y * w + x];
            uint8_t r = (c565 >> 11) & 0x1F; // 5 bits
            uint8_t g = (c565 >> 5) & 0x3F;  // 6 bits
            uint8_t b = (c565) & 0x1F;       // 5 bits
            // Scale to 0..255 approximately
            uint16_t r8 = (r * 527 + 23) >> 6;   // ~ *255/31
            uint16_t g8 = (g * 259 + 33) >> 6;   // ~ *255/63
            uint16_t b8 = (b * 527 + 23) >> 6;
            uint16_t lum = (uint16_t)(r8 * 30 + g8 * 59 + b8 * 11) / 100; // 0..255
            // Detect pure red (approx) else map by luminance
            uint8_t st_color;
            if (r > 24 && g < 8 && b < 8) st_color = ST7306_COLOR_RED;
            else st_color = (lum < 128) ? ST7306_COLOR_BLACK : ST7306_COLOR_WHITE;
            if (st_color == ST7306_COLOR_BLACK) cnt_black++; else if (st_color == ST7306_COLOR_WHITE) cnt_white++; else if (st_color == ST7306_COLOR_RED) cnt_red++;
            st7306_draw_pixel(self->dev_, area->x1 + x, area->y1 + y, st_color);
        }
    }
    // Partial window flush for the damaged area (aligned to 2x2 block as buffer packs 4 pixels per byte)
    int x1 = area->x1; int y1 = area->y1; int x2 = area->x2; int y2 = area->y2;
    if (x1 < 0) { x1 = 0; }
    if (y1 < 0) { y1 = 0; }
    if (x2 >= ST7306_WIDTH) { x2 = ST7306_WIDTH - 1; }
    if (y2 >= ST7306_HEIGHT) { y2 = ST7306_HEIGHT - 1; }
    // Align to even boundaries to match 2x2 packing (expand area minimally to cover updated pixels)
    x1 &= ~1; y1 &= ~1; x2 |= 1; y2 |= 1;
    static uint32_t s_flush_log_cnt = 0;
    if ((s_flush_log_cnt++ % 20) == 0) {
        ESP_LOGI(TAG, "LVGL flush area=(%d,%d)-(%d,%d) w=%d h=%d mapped BW/R=%u/%u/%u, window=(%d,%d)-(%d,%d)",
                 (int)area->x1, (int)area->y1, (int)area->x2, (int)area->y2, (int)w, (int)h,
                 (unsigned)cnt_black, (unsigned)cnt_white, (unsigned)cnt_red,
                 (int)x1, (int)y1, (int)x2, (int)y2);
    }
    // Accumulate dirty area and schedule a coalesced commit
    if (!self->dirty_area_valid_) {
        self->dirty_area_ = { .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2 };
        self->dirty_area_valid_ = true;
        } else {
        if (x1 < self->dirty_area_.x1) self->dirty_area_.x1 = x1;
        if (y1 < self->dirty_area_.y1) self->dirty_area_.y1 = y1;
        if (x2 > self->dirty_area_.x2) self->dirty_area_.x2 = x2;
        if (y2 > self->dirty_area_.y2) self->dirty_area_.y2 = y2;
    }
    // 对于第一个 LVGL 帧，强制一次全屏提交，确保彻底覆盖上电残影
    if (!self->did_first_full_display_) {
        self->dirty_area_valid_ = true;
        self->dirty_area_.x1 = 0; self->dirty_area_.y1 = 0;
        self->dirty_area_.x2 = ST7306_WIDTH - 1; self->dirty_area_.y2 = ST7306_HEIGHT - 1;
        self->did_first_full_display_ = true;
    }
    self->ScheduleCommit(2000); // 2ms debounce to merge adjacent rows
    lv_display_flush_ready(disp);
}
void EinkDisplayST7306::CommitTimerThunk(void* arg) {
    reinterpret_cast<EinkDisplayST7306*>(arg)->CommitDirtyArea();
}

void EinkDisplayST7306::ScheduleCommit(uint64_t delay_us) {
    if (!commit_timer_) return;
    esp_timer_stop(commit_timer_);
    esp_timer_start_once(commit_timer_, delay_us);
}

void EinkDisplayST7306::CommitDirtyArea() {
    if (!dirty_area_valid_) return;
    int x1 = dirty_area_.x1, y1 = dirty_area_.y1, x2 = dirty_area_.x2, y2 = dirty_area_.y2;
    dirty_area_valid_ = false;
    // Controller requires even alignment
    x1 &= ~1; y1 &= ~1; x2 |= 1; y2 |= 1;
    static uint32_t s_commit_log_cnt = 0;
    if ((s_commit_log_cnt++ % 20) == 0) {
        ESP_LOGI(TAG, "Commit window (%d,%d)-(%d,%d)", x1, y1, x2, y2);
    }
    esp_err_t ret = st7306_display_window(dev_, x1, y1, x2, y2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "commit display_window failed (%d), fallback full", (int)ret);
        st7306_display(dev_);
    }
    // 记录一次局部提交，并周期性做全屏清洁，减轻鬼影
    partial_commits_since_full_++;
    uint64_t now = esp_timer_get_time();
    // 仅在空闲态执行强力擦拭，避免对话过程中闪白；提高阈值以减少整屏刷新频率
    auto state = Application::GetInstance().GetDeviceState();
    bool is_idle = (state == kDeviceStateIdle || state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring);
    const uint32_t kWipeThreshold = 120;
    const uint64_t kWipeIntervalUs = 180000000ULL; // 180s
    if (is_idle && (partial_commits_since_full_ >= kWipeThreshold || (now - last_wipe_us_) > kWipeIntervalUs)) {
        ESP_LOGI(TAG, "E-ink full wipe (anti-ghosting) after %u partial commits", (unsigned)partial_commits_since_full_);
        st7306_wipe_sequence(dev_);
        partial_commits_since_full_ = 0;
        last_wipe_us_ = now;
    }
}

void EinkDisplayST7306::NetBlinkTimerThunk(void* arg) {
    // 同上，避免在 esp_timer 线程里直接操作 LVGL
    auto* self = reinterpret_cast<EinkDisplayST7306*>(arg);
    Application::GetInstance().Schedule([self]() { self->UpdateNetBlink(); });
}

void EinkDisplayST7306::StartNetworkErrorBlink() {
    net_blink_on_ = false;
    if (net_blink_timer_) {
        esp_timer_stop(net_blink_timer_);
        // 500ms toggle; adjust if needed
        esp_timer_start_periodic(net_blink_timer_, 500000);
    }
}

void EinkDisplayST7306::StopNetworkErrorBlink() {
    if (net_blink_timer_) {
        esp_timer_stop(net_blink_timer_);
    }
    if (top_bar_) {
        lv_obj_set_style_bg_color(top_bar_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
    }
    if (action_bar_) {
        lv_obj_set_style_bg_color(action_bar_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(action_bar_, LV_OPA_COVER, 0);
    }
    // Touch up display
    lv_refr_now(NULL);
}

void EinkDisplayST7306::UpdateNetBlink() {
    net_blink_on_ = !net_blink_on_;
    lv_color_t c = net_blink_on_ ? lv_color_hex(0xFF0000) : lv_color_white();
    if (top_bar_) {
        lv_obj_set_style_bg_color(top_bar_, c, 0);
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
    }
    if (action_bar_) {
        lv_obj_set_style_bg_color(action_bar_, c, 0);
        lv_obj_set_style_bg_opa(action_bar_, LV_OPA_COVER, 0);
    }
    // Coalesce repaint
    lv_refr_now(NULL);
}

void EinkDisplayST7306::BuildWelcomePage() {
    // Root screen if not exists
    if (!screen_obj_) {
        screen_obj_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(screen_obj_, width_, height_);
        lv_obj_set_style_bg_color(screen_obj_, lv_color_white(), 0);
    }
    // Top bar
    if (!top_bar_) {
        top_bar_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(top_bar_, width_, 24);
        lv_obj_set_style_bg_color(top_bar_, lv_color_white(), 0);
        lv_obj_set_style_border_side(top_bar_, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(top_bar_, 1, 0);
        lv_obj_set_style_border_color(top_bar_, lv_color_black(), 0);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

        mode_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(mode_label_, &font_puhui_16_4, 0);
        lv_label_set_text(mode_label_, "WELCOME");
        lv_obj_align(mode_label_, LV_ALIGN_LEFT_MID, 4, 0);

        time_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(time_label_, &font_puhui_16_4, 0);
        {
            time_t now = time(NULL);
            if (!time_sync_completed_) { now += kStartupTimeOffsetSeconds; }
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%H:%M", localtime(&now));
            lv_label_set_text(time_label_, time_str);
        }
        lv_obj_align(time_label_, LV_ALIGN_CENTER, 0, 0);

        wifi_text_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(wifi_text_label_, &font_puhui_16_4, 0);
        lv_label_set_text(wifi_text_label_, "WiFi");
        lv_obj_align(wifi_text_label_, LV_ALIGN_RIGHT_MID, -60, 0);

        battery_text_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(battery_text_label_, &font_puhui_16_4, 0);
        lv_label_set_text(battery_text_label_, "--%");
        lv_obj_align(battery_text_label_, LV_ALIGN_RIGHT_MID, -4, 0);
        // Initialize battery percent immediately to avoid waiting for periodic update
        int level = 0; bool charging = false; bool discharging = false;
        auto &board = Board::GetInstance();
        if (board.GetBatteryLevel(level, charging, discharging)) {
            char buf[8]; snprintf(buf, sizeof(buf), "%d%%", level);
            lv_label_set_text(battery_text_label_, buf);
            ESP_LOGI(TAG, "Init battery percent on Welcome: %d%% (charging=%d, discharging=%d)", level, (int)charging, (int)discharging);
        }
    }

    // Content area
    if (!content_area_) {
        content_area_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(content_area_, width_, height_ - 24 - 24);
        lv_obj_align(content_area_, LV_ALIGN_TOP_MID, 0, 24);
        lv_obj_set_style_bg_color(content_area_, lv_color_white(), 0);
    }
    if (!welcome_title_label_) {
        welcome_title_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(welcome_title_label_, &font_puhui_16_4, 0);
        lv_obj_set_width(welcome_title_label_, width_ - 16);
        lv_label_set_long_mode(welcome_title_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(welcome_title_label_, "下午好!");
        lv_obj_align(welcome_title_label_, LV_ALIGN_TOP_MID, 0, 36);
        lv_obj_set_style_text_align(welcome_title_label_, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (!welcome_name_label_) {
        welcome_name_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(welcome_name_label_, &font_puhui_16_4, 0);
        lv_label_set_text(welcome_name_label_, "文先生");
        lv_obj_align(welcome_name_label_, LV_ALIGN_TOP_MID, 0, 58);
        lv_obj_set_style_text_align(welcome_name_label_, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (!welcome_sub_label_) {
        welcome_sub_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(welcome_sub_label_, &font_puhui_16_4, 0);
        lv_label_set_text(welcome_sub_label_, ""); // 初始不显示副标题
        lv_obj_align(welcome_sub_label_, LV_ALIGN_TOP_MID, 0, 80);
        lv_obj_set_style_text_align(welcome_sub_label_, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (!welcome_wake_hint_label_) {
        welcome_wake_hint_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(welcome_wake_hint_label_, &font_puhui_16_4, 0);
        lv_obj_set_width(welcome_wake_hint_label_, width_ - 16);
        lv_label_set_long_mode(welcome_wake_hint_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(welcome_wake_hint_label_, "");
        lv_obj_align(welcome_wake_hint_label_, LV_ALIGN_TOP_MID, 0, 100);
        lv_obj_set_style_text_align(welcome_wake_hint_label_, LV_TEXT_ALIGN_CENTER, 0);
    }

    // Update wake hint once when building the page
    {
        std::string hint;
        auto &app = Application::GetInstance();
        auto words = app.GetConfiguredWakeWords();
        if (!words.empty()) {
            // Join unique words with / and render in quotes
            // Avoid overly long strings by limiting to first 2 items
            std::string joined;
            size_t max_items = words.size() > 2 ? 2 : words.size();
            for (size_t i = 0; i < max_items; ++i) {
                if (i > 0) joined += "/";
                joined += words[i];
            }
            if (words.size() > max_items) joined += "/…";
            hint = std::string("说出“") + joined + "”唤醒设备";
        } else {
            hint = "说出“唤醒词”唤醒设备";
        }
        if (welcome_wake_hint_label_) {
            lv_label_set_text(welcome_wake_hint_label_, hint.c_str());
        }
    }

    // Action bar
    if (!action_bar_) {
        action_bar_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(action_bar_, width_, 24);
        lv_obj_set_style_bg_color(action_bar_, lv_color_white(), 0);
        lv_obj_set_style_border_side(action_bar_, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(action_bar_, 1, 0);
        lv_obj_set_style_border_color(action_bar_, lv_color_black(), 0);
        lv_obj_align(action_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_t* hint = lv_label_create(action_bar_);
        lv_obj_set_style_text_font(hint, &font_puhui_16_4, 0);
        lv_label_set_text(hint, "数派 AI 工牌");
        lv_obj_align(hint, LV_ALIGN_LEFT_MID, 4, 0);
    }
}

void EinkDisplayST7306::BuildChatPage() {
    if (!screen_obj_) {
        screen_obj_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(screen_obj_, width_, height_);
        lv_obj_set_style_bg_color(screen_obj_, lv_color_white(), 0);
    }
    // Top bar
    if (!top_bar_) {
        top_bar_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(top_bar_, width_, 24);
        lv_obj_set_style_bg_color(top_bar_, lv_color_white(), 0);
        lv_obj_set_style_border_side(top_bar_, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(top_bar_, 1, 0);
        lv_obj_set_style_border_color(top_bar_, lv_color_black(), 0);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

        mode_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(mode_label_, &font_puhui_16_4, 0);
        // 状态栏文案：Listening/Conversing 对齐
        auto state = Application::GetInstance().GetDeviceState();
        lv_label_set_text(mode_label_, (state == kDeviceStateListening) ? "聆听中" : (state == kDeviceStateSpeaking) ? "说话中" : "对话");
        lv_obj_align(mode_label_, LV_ALIGN_LEFT_MID, 4, 0);

        time_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(time_label_, &font_puhui_16_4, 0);
        {
            time_t now = time(NULL);
            if (!time_sync_completed_) { now += kStartupTimeOffsetSeconds; }
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%H:%M", localtime(&now));
            lv_label_set_text(time_label_, time_str);
        }
        lv_obj_align(time_label_, LV_ALIGN_CENTER, 0, 0);

        wifi_text_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(wifi_text_label_, &font_puhui_16_4, 0);
        lv_label_set_text(wifi_text_label_, "WiFi");
        lv_obj_align(wifi_text_label_, LV_ALIGN_RIGHT_MID, -60, 0);

        battery_text_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(battery_text_label_, &font_puhui_16_4, 0);
        lv_label_set_text(battery_text_label_, "--%");
        lv_obj_align(battery_text_label_, LV_ALIGN_RIGHT_MID, -4, 0);
    }

    // Content area
    if (!content_area_) {
        content_area_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(content_area_, width_, height_ - 24 - 24);
        lv_obj_align(content_area_, LV_ALIGN_TOP_MID, 0, 24);
        lv_obj_set_style_bg_color(content_area_, lv_color_white(), 0);
    }
    // "对话中：" title
    if (!chat_summary_label_) {
        chat_summary_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(chat_summary_label_, &font_puhui_16_4, 0);
        lv_label_set_text(chat_summary_label_, "> 摘要：");
        lv_obj_align(chat_summary_label_, LV_ALIGN_TOP_LEFT, 8, 6);
    }
    // Separator line (1px)
    if (!chat_summary_sep_) {
        chat_summary_sep_ = lv_obj_create(content_area_);
        lv_obj_set_size(chat_summary_sep_, width_ - 16, 1);
        lv_obj_set_style_bg_color(chat_summary_sep_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(chat_summary_sep_, LV_OPA_COVER, 0);
        lv_obj_align(chat_summary_sep_, LV_ALIGN_TOP_LEFT, 8, 24);
    }
    // Summary text
    if (!chat_summary_view_) {
        // Create a clipped viewport for summary with fixed height to protect bullets area
        // Increased height from 120px to 140px to accommodate more content
        chat_summary_view_ = lv_obj_create(content_area_);
        lv_obj_set_size(chat_summary_view_, width_ - 16, 140);
        lv_obj_align(chat_summary_view_, LV_ALIGN_TOP_LEFT, 8, 28);
        lv_obj_set_style_bg_opa(chat_summary_view_, LV_OPA_TRANSP, 0);
        lv_obj_set_scrollbar_mode(chat_summary_view_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(chat_summary_view_, LV_DIR_NONE);
        // clip enabled by default for containers; ensure no border
        lv_obj_set_style_border_width(chat_summary_view_, 0, 0);
        // add a small right padding to avoid last glyph being clipped
        lv_obj_set_style_pad_right(chat_summary_view_, 6, 0);
        summary_view_height_ = 140;
    }
    if (!chat_summary_text_label_) {
        chat_summary_text_label_ = lv_label_create(chat_summary_view_);
        lv_obj_set_style_text_font(chat_summary_text_label_, &font_puhui_16_4, 0);
        int view_w = lv_obj_get_width(chat_summary_view_);
        int pad_r = lv_obj_get_style_pad_right(chat_summary_view_, 0);
        int text_w = view_w - pad_r - 2; // keep a small safety margin
        if (text_w & 1) text_w -= 1;     // prefer even width to align with 2x2 packing
        if (text_w < 10) text_w = view_w - 4; // fallback in case of degenerate values
        lv_obj_set_width(chat_summary_text_label_, text_w);
        lv_label_set_long_mode(chat_summary_text_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(chat_summary_text_label_, "");
        lv_obj_align(chat_summary_text_label_, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    // "用户要点(近3条)：" title - adjusted position for increased viewport height
    if (!chat_history_title_label_) {
        chat_history_title_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(chat_history_title_label_, &font_puhui_16_4, 0);
        lv_label_set_text(chat_history_title_label_, "用户要点(近3条)：");
        lv_obj_align(chat_history_title_label_, LV_ALIGN_BOTTOM_LEFT, 8, -80);
    }
    // History separator
    if (!chat_history_sep_) {
        chat_history_sep_ = lv_obj_create(content_area_);
        lv_obj_set_size(chat_history_sep_, width_ - 16, 1);
        lv_obj_set_style_bg_color(chat_history_sep_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(chat_history_sep_, LV_OPA_COVER, 0);
        lv_obj_align(chat_history_sep_, LV_ALIGN_BOTTOM_LEFT, 8, -66);
    }
    // Bullet lines (3)
    for (int i = 0; i < 3; ++i) {
        if (!chat_bullet_labels_[i]) {
            chat_bullet_labels_[i] = lv_label_create(content_area_);
            lv_obj_set_style_text_font(chat_bullet_labels_[i], &font_puhui_16_4, 0);
            lv_obj_set_width(chat_bullet_labels_[i], width_ - 16);
            // Use DOT truncation to avoid stacking when text is long
            lv_label_set_long_mode(chat_bullet_labels_[i], LV_LABEL_LONG_DOT);
            lv_label_set_text(chat_bullet_labels_[i], "");
            // Slightly larger spacing between lines
            lv_obj_align(chat_bullet_labels_[i], LV_ALIGN_BOTTOM_LEFT, 8, -54 + i * 18);
        }
    }

    // Action bar
    if (!action_bar_) {
        action_bar_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(action_bar_, width_, 24);
        lv_obj_set_style_bg_color(action_bar_, lv_color_white(), 0);
        lv_obj_set_style_border_side(action_bar_, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(action_bar_, 1, 0);
        lv_obj_set_style_border_color(action_bar_, lv_color_black(), 0);
        lv_obj_align(action_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_t* hint = lv_label_create(action_bar_);
        lv_obj_set_style_text_font(hint, &font_puhui_16_4, 0);
        lv_label_set_text(hint, "数派 AI 工牌");
        lv_obj_align(hint, LV_ALIGN_LEFT_MID, 4, 0);
    }
}

void EinkDisplayST7306::BuildMeetingPage() {
    if (!screen_obj_) {
        screen_obj_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(screen_obj_, width_, height_);
        lv_obj_set_style_bg_color(screen_obj_, lv_color_white(), 0);
    }
    // Top bar
    if (!top_bar_) {
        top_bar_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(top_bar_, width_, 24);
        lv_obj_set_style_bg_color(top_bar_, lv_color_white(), 0);
        lv_obj_set_style_border_side(top_bar_, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(top_bar_, 1, 0);
        lv_obj_set_style_border_color(top_bar_, lv_color_black(), 0);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

        mode_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(mode_label_, &font_puhui_16_4, 0);
        lv_label_set_text(mode_label_, "会议模式");
        lv_obj_align(mode_label_, LV_ALIGN_LEFT_MID, 4, 0);

        time_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(time_label_, &font_puhui_16_4, 0);
        lv_label_set_text(time_label_, "14:35");
        lv_obj_align(time_label_, LV_ALIGN_CENTER, 0, 0);

        wifi_text_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(wifi_text_label_, &font_puhui_16_4, 0);
        lv_label_set_text(wifi_text_label_, "WiFi");
        lv_obj_align(wifi_text_label_, LV_ALIGN_RIGHT_MID, -60, 0);

        battery_text_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(battery_text_label_, &font_puhui_16_4, 0);
        lv_label_set_text(battery_text_label_, "--%");
        lv_obj_align(battery_text_label_, LV_ALIGN_RIGHT_MID, -4, 0);
    }

    // Content area
    if (!content_area_) {
        content_area_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(content_area_, width_, height_ - 24 - 24);
        lv_obj_align(content_area_, LV_ALIGN_TOP_MID, 0, 24);
        lv_obj_set_style_bg_color(content_area_, lv_color_white(), 0);
    }
    if (!meeting_topic_label_) {
        meeting_topic_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(meeting_topic_label_, &font_puhui_16_4, 0);
        lv_label_set_text(meeting_topic_label_, "会议主题: 产品需求评审");
        lv_obj_align(meeting_topic_label_, LV_ALIGN_TOP_LEFT, 8, 6);
    }
    if (!meeting_duration_label_) {
        meeting_duration_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(meeting_duration_label_, &font_puhui_16_4, 0);
        lv_label_set_text(meeting_duration_label_, "持续时间: 00:00:49");
        lv_obj_align(meeting_duration_label_, LV_ALIGN_TOP_LEFT, 8, 24);
    }
    if (!meeting_status_label_) {
        meeting_status_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(meeting_status_label_, &font_puhui_16_4, 0);
        lv_label_set_text(meeting_status_label_, "当前状态: ● REC 会议进行中...");
        lv_obj_align(meeting_status_label_, LV_ALIGN_TOP_LEFT, 8, 42);
    }
    if (!meeting_transcript_title_) {
        meeting_transcript_title_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(meeting_transcript_title_, &font_puhui_16_4, 0);
        lv_label_set_text(meeting_transcript_title_, "实时转写:");
        lv_obj_align(meeting_transcript_title_, LV_ALIGN_TOP_LEFT, 8, 64);
    }
    if (!meeting_transcript_label_) {
        meeting_transcript_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(meeting_transcript_label_, &font_puhui_16_4, 0);
        lv_obj_set_width(meeting_transcript_label_, width_ - 16);
        lv_label_set_long_mode(meeting_transcript_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(meeting_transcript_label_, "> 确认登录入口放在顶部导航\n> 后端鉴权改为基于令牌\n> UI 简化到两步登录流程");
        lv_obj_align(meeting_transcript_label_, LV_ALIGN_TOP_LEFT, 8, 84);
    }

    // Action bar
    if (!action_bar_) {
        action_bar_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(action_bar_, width_, 24);
        lv_obj_set_style_bg_color(action_bar_, lv_color_white(), 0);
        lv_obj_set_style_border_side(action_bar_, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(action_bar_, 1, 0);
        lv_obj_set_style_border_color(action_bar_, lv_color_black(), 0);
        lv_obj_align(action_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_t* hint = lv_label_create(action_bar_);
        lv_obj_set_style_text_font(hint, &font_puhui_16_4, 0);
        lv_label_set_text(hint, "数派 AI 工牌");
        lv_obj_align(hint, LV_ALIGN_LEFT_MID, 4, 0);
    }
}

void EinkDisplayST7306::BuildCodingPage() {
    if (!screen_obj_) {
        screen_obj_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(screen_obj_, width_, height_);
        lv_obj_set_style_bg_color(screen_obj_, lv_color_white(), 0);
    }
    // Top bar
    if (!top_bar_) {
        top_bar_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(top_bar_, width_, 24);
        lv_obj_set_style_bg_color(top_bar_, lv_color_white(), 0);
        lv_obj_set_style_border_side(top_bar_, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(top_bar_, 1, 0);
        lv_obj_set_style_border_color(top_bar_, lv_color_black(), 0);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

        mode_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(mode_label_, &font_puhui_16_4, 0);
        lv_label_set_text(mode_label_, "编码模式");
        lv_obj_align(mode_label_, LV_ALIGN_LEFT_MID, 4, 0);

        time_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(time_label_, &font_puhui_16_4, 0);
        lv_label_set_text(time_label_, "14:42");
        lv_obj_align(time_label_, LV_ALIGN_CENTER, 0, 0);

        wifi_text_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(wifi_text_label_, &font_puhui_16_4, 0);
        lv_label_set_text(wifi_text_label_, "WiFi");
        lv_obj_align(wifi_text_label_, LV_ALIGN_RIGHT_MID, -60, 0);

        battery_text_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(battery_text_label_, &font_puhui_16_4, 0);
        lv_label_set_text(battery_text_label_, "--%");
        lv_obj_align(battery_text_label_, LV_ALIGN_RIGHT_MID, -4, 0);
    }

    // Content
    if (!content_area_) {
        content_area_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(content_area_, width_, height_ - 24 - 24);
        lv_obj_align(content_area_, LV_ALIGN_TOP_MID, 0, 24);
        lv_obj_set_style_bg_color(content_area_, lv_color_white(), 0);
    }
    if (!coding_state_label_) {
        coding_state_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(coding_state_label_, &font_puhui_16_4, 0);
        lv_label_set_text(coding_state_label_, "当前状态: ● REC 需求聆听中... 00:15");
        lv_obj_align(coding_state_label_, LV_ALIGN_TOP_LEFT, 8, 6);
    }
    if (!coding_prompt_label_) {
        coding_prompt_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(coding_prompt_label_, &font_puhui_16_4, 0);
        lv_obj_set_width(coding_prompt_label_, width_ - 16);
        lv_label_set_long_mode(coding_prompt_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(coding_prompt_label_, "提示：请用自然语言描述您的需求");
        lv_obj_align(coding_prompt_label_, LV_ALIGN_TOP_LEFT, 8, 26);
    }
    if (!coding_transcript_title_) {
        coding_transcript_title_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(coding_transcript_title_, &font_puhui_16_4, 0);
        lv_label_set_text(coding_transcript_title_, "实时转写:");
        lv_obj_align(coding_transcript_title_, LV_ALIGN_TOP_LEFT, 8, 48);
    }
    if (!coding_transcript_label_) {
        coding_transcript_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(coding_transcript_label_, &font_puhui_16_4, 0);
        lv_obj_set_width(coding_transcript_label_, width_ - 16);
        lv_label_set_long_mode(coding_transcript_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(coding_transcript_label_, "> 我需要一个用户登录功能\n> 支持手机号 + 验证码登录\n> 登录成功后跳转到首页");
        lv_obj_align(coding_transcript_label_, LV_ALIGN_TOP_LEFT, 8, 68);
    }

    // Action bar
    if (!action_bar_) {
        action_bar_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(action_bar_, width_, 24);
        lv_obj_set_style_bg_color(action_bar_, lv_color_white(), 0);
        lv_obj_set_style_border_side(action_bar_, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(action_bar_, 1, 0);
        lv_obj_set_style_border_color(action_bar_, lv_color_black(), 0);
        lv_obj_align(action_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_t* hint = lv_label_create(action_bar_);
        lv_obj_set_style_text_font(hint, &font_puhui_16_4, 0);
        lv_label_set_text(hint, "说: '结束需求' | '停止分析' | '开始总结'");
        lv_obj_align(hint, LV_ALIGN_LEFT_MID, 4, 0);
    }
}

void EinkDisplayST7306::BuildWorkingPage() {
    if (!screen_obj_) {
        screen_obj_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(screen_obj_, width_, height_);
        lv_obj_set_style_bg_color(screen_obj_, lv_color_white(), 0);
    }
    // Top bar
    if (!top_bar_) {
        top_bar_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(top_bar_, width_, 24);
        lv_obj_set_style_bg_color(top_bar_, lv_color_white(), 0);
        lv_obj_set_style_border_side(top_bar_, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(top_bar_, 1, 0);
        lv_obj_set_style_border_color(top_bar_, lv_color_black(), 0);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

        mode_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(mode_label_, &font_puhui_16_4, 0);
        lv_label_set_text(mode_label_, "工作模式");
        lv_obj_align(mode_label_, LV_ALIGN_LEFT_MID, 4, 0);

        time_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(time_label_, &font_puhui_16_4, 0);
        lv_label_set_text(time_label_, "14:42");
        lv_obj_align(time_label_, LV_ALIGN_CENTER, 0, 0);

        wifi_text_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(wifi_text_label_, &font_puhui_16_4, 0);
        lv_label_set_text(wifi_text_label_, "WiFi");
        lv_obj_align(wifi_text_label_, LV_ALIGN_RIGHT_MID, -60, 0);

        battery_text_label_ = lv_label_create(top_bar_);
        lv_obj_set_style_text_font(battery_text_label_, &font_puhui_16_4, 0);
        lv_label_set_text(battery_text_label_, "--%");
        lv_obj_align(battery_text_label_, LV_ALIGN_RIGHT_MID, -4, 0);
    }

    // Content
    if (!content_area_) {
        content_area_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(content_area_, width_, height_ - 24 - 24);
        lv_obj_align(content_area_, LV_ALIGN_TOP_MID, 0, 24);
        lv_obj_set_style_bg_color(content_area_, lv_color_white(), 0);
    }
    if (!working_header_label_) {
        working_header_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(working_header_label_, &font_puhui_16_4, 0);
        lv_label_set_text(working_header_label_, "> 今日任务清单");
        lv_obj_align(working_header_label_, LV_ALIGN_TOP_LEFT, 8, 6);
    }
    if (!working_task_title_) {
        working_task_title_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(working_task_title_, &font_puhui_16_4, 0);
        lv_label_set_text(working_task_title_, "");
        lv_obj_align(working_task_title_, LV_ALIGN_TOP_LEFT, 8, 24);
    }
    const char* demo_tasks[6] = {
        "[优先级0][福州] 聚变平台后端异常 (已完成)",
        "[优先级1][新奥] 今晚8点交付技术说明文档",
        "[优先级1][福州] 周二前完成原型设计",
        "[优先级2][西安] 周三前修复API调用异常",
        "[优先级3][协作] 周五前协作前端完成连调",
        ""
    };
    for (int i = 0; i < 5; ++i) {
        if (!working_task_labels_[i]) {
            working_task_labels_[i] = lv_label_create(content_area_);
            lv_obj_set_style_text_font(working_task_labels_[i], &font_puhui_16_4, 0);
            lv_obj_set_width(working_task_labels_[i], width_ - 16);
            lv_label_set_long_mode(working_task_labels_[i], LV_LABEL_LONG_WRAP);
            lv_label_set_text(working_task_labels_[i], demo_tasks[i]);
            lv_obj_align(working_task_labels_[i], LV_ALIGN_TOP_LEFT, 8, 44 + i * 18);
        }
    }
    if (!working_reminder_title_) {
        working_reminder_title_ = lv_label_create(content_area_);
        lv_obj_set_style_text_font(working_reminder_title_, &font_puhui_16_4, 0);
        lv_label_set_text(working_reminder_title_, "> 提醒事项");
        lv_obj_align(working_reminder_title_, LV_ALIGN_TOP_LEFT, 8, 44 + 5 * 18 + 8);
    }
    const char* demo_reminders[3] = {
        "[会议][成都]下午5点 腾讯会议",
        "[交付][昆明]明早10点 线下对接",
        ""
    };
    for (int i = 0; i < 2; ++i) {
        if (!working_reminder_labels_[i]) {
            working_reminder_labels_[i] = lv_label_create(content_area_);
            lv_obj_set_style_text_font(working_reminder_labels_[i], &font_puhui_16_4, 0);
            lv_obj_set_width(working_reminder_labels_[i], width_ - 16);
            lv_label_set_long_mode(working_reminder_labels_[i], LV_LABEL_LONG_WRAP);
            lv_label_set_text(working_reminder_labels_[i], demo_reminders[i]);
            lv_obj_align(working_reminder_labels_[i], LV_ALIGN_TOP_LEFT, 8, 44 + 5 * 18 + 28 + i * 18);
        }
    }

    // Action bar
    if (!action_bar_) {
        action_bar_ = lv_obj_create(screen_obj_);
        lv_obj_set_size(action_bar_, width_, 24);
        lv_obj_set_style_bg_color(action_bar_, lv_color_white(), 0);
        lv_obj_set_style_border_side(action_bar_, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(action_bar_, 1, 0);
        lv_obj_set_style_border_color(action_bar_, lv_color_black(), 0);
        lv_obj_align(action_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_t* hint = lv_label_create(action_bar_);
        lv_obj_set_style_text_font(hint, &font_puhui_16_4, 0);
        lv_label_set_text(hint, "数派 AI 工牌");
        lv_obj_align(hint, LV_ALIGN_LEFT_MID, 4, 0);
    }
}


void EinkDisplayST7306::SwitchPage(PageType page) {
    // Clear previous root if exists
    if (screen_obj_) {
        // Before cleaning, hide and nullify transient overlays to avoid LVGL delete during tree mutation
        if (branch_hint_label_ && lv_obj_is_valid(branch_hint_label_)) {
            lv_obj_add_flag(branch_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clean(lv_screen_active());
        screen_obj_ = nullptr;
        // Overlay label should be destroyed and pointer reset to avoid stale access
        branch_hint_label_ = nullptr;
        content_label_ = nullptr;
        top_bar_ = nullptr;
        content_area_ = nullptr;
        action_bar_ = nullptr;
        mode_label_ = nullptr;
        time_label_ = nullptr;
        wifi_text_label_ = nullptr;
        battery_text_label_ = nullptr;
        welcome_title_label_ = nullptr;
        welcome_name_label_ = nullptr;
        welcome_sub_label_ = nullptr;
        welcome_wake_hint_label_ = nullptr;  // 【关键修复】重置Welcome页面的唤醒提示label指针
        chat_summary_label_ = nullptr;
        chat_summary_view_ = nullptr;
        chat_summary_sep_ = nullptr;
        chat_summary_text_label_ = nullptr;
        chat_history_title_label_ = nullptr;
        chat_history_sep_ = nullptr;
        for (auto &p : chat_bullet_labels_) p = nullptr;
    }
    current_page_ = page;
    if (page == kPageWelcome) BuildWelcomePage();
    else if (page == kPageChat) BuildChatPage();
    else if (page == kPageMeeting) BuildMeetingPage();
    else if (page == kPageCoding) BuildCodingPage();
    else if (page == kPageWorking) BuildWorkingPage();
    lv_refr_now(NULL);
}

void EinkDisplayST7306::UpdateChatTexts() {
    if (current_page_ != kPageChat) return;
    if (chat_summary_text_label_) {
        UpdateSummaryLabelTextWrapped();
        if (chat_summary_view_) {
            int view_w = lv_obj_get_width(chat_summary_view_);
            int pad_r = lv_obj_get_style_pad_right(chat_summary_view_, 0);
            int text_w = view_w - pad_r - 2;
            if (text_w & 1) text_w -= 1;
            if (text_w > 0) lv_obj_set_width(chat_summary_text_label_, text_w);
        }
        UpdateSummaryScrollState();
    }
    for (int i = 0; i < 3; ++i) {
        if (chat_bullet_labels_[i]) {
            const char* line = (i < (int)bullets_.size()) ? bullets_[i].c_str() : "";
            std::string text_src = line ? std::string(line) : std::string();
            if (!text_src.empty()) text_src = TruncateUtf8ByChars(text_src, 22);
            std::string t = (text_src.empty() ? std::string() : std::string("• ") + text_src);
            lv_label_set_text(chat_bullet_labels_[i], t.c_str());
        }
    }
}

void EinkDisplayST7306::SummaryScrollTimerThunk(void* arg) {
    auto* self = reinterpret_cast<EinkDisplayST7306*>(arg);
    Application::GetInstance().Schedule([self]() { self->OnSummaryScrollTick(); });
}

void EinkDisplayST7306::OnSummaryScrollTick() {
    if (current_page_ != kPageChat || !chat_summary_view_ || !chat_summary_text_label_) return;
    if (!Lock(50)) return;
    // Force LVGL to recalculate label height in case text was just updated
    lv_obj_update_layout(chat_summary_text_label_);
    lv_coord_t label_h = lv_obj_get_height(chat_summary_text_label_);
    if (label_h <= summary_view_height_) {
        if (summary_scroll_timer_) esp_timer_stop(summary_scroll_timer_);
        summary_scroll_active_ = false;
        Unlock();
        return;
    }
    summary_scroll_y_ += summary_scroll_step_px_;
    int max_offset = label_h - summary_view_height_;
    if (summary_scroll_y_ > max_offset) {
        summary_scroll_y_ = 0;
    }
    lv_obj_set_y(chat_summary_text_label_, -summary_scroll_y_);
    lv_refr_now(NULL);
    Unlock();
}

void EinkDisplayST7306::UpdateSummaryScrollState() {
    if (current_page_ != kPageChat || !chat_summary_view_ || !chat_summary_text_label_) return;
    lv_obj_set_y(chat_summary_text_label_, 0);
    summary_scroll_y_ = 0;
    // Force LVGL to recalculate layout after text/width changes
    lv_obj_update_layout(chat_summary_text_label_);
    lv_coord_t label_h = lv_obj_get_height(chat_summary_text_label_);
    if (label_h > summary_view_height_) {
        if (!summary_scroll_active_ && summary_scroll_timer_) {
            summary_scroll_active_ = true;
            esp_timer_stop(summary_scroll_timer_);
            esp_timer_start_periodic(summary_scroll_timer_, 2000000ULL);
        }
    } else {
        if (summary_scroll_timer_) esp_timer_stop(summary_scroll_timer_);
        summary_scroll_active_ = false;
    }
}

void EinkDisplayST7306::OnNetworkReady() {
    // Called when Wi‑Fi connected; show the service subtitle on welcome page only
    if (current_page_ == kPageWelcome && welcome_sub_label_) {
        lv_label_set_text(welcome_sub_label_, "数派喵喵为您服务");
        lv_refr_now(NULL);
    }
}
