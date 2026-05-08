#include "display/waveguide_display.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>
// #include "../../reference code/src/jbd013_api.h"  // 注释掉外部依赖 reference code
#include <cstring>
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_16_4);

static const char* TAG __attribute__((unused)) = "WaveguideDisplay";
static bool s_lvgl_inited = false;

// 将 400x300 画面居中到 640x480 面板坐标（横屏）
static inline void panel_map_offset(int& col_off, int& row_off) {
    col_off = (640 - 400) / 2; // 120
    row_off = (480 - 300) / 2; // 90
}

// 硬件层清屏：将 400x300 可视窗口全部写 0（黑）
static void panel_clear_visible_window()
{
    // 注释掉 reference code 的 API 调用
    /*
    int col_off, row_off; panel_map_offset(col_off, row_off);
    static uint8_t zero_line[400/2];
    memset(zero_line, 0x00, sizeof(zero_line));
    for (int y = 0; y < 300; ++y) {
        spi_wr_cache((uint16_t)col_off, (uint16_t)(row_off + y), zero_line, sizeof(zero_line));
        if ((y & 0x0F) == 0) {
            vTaskDelay(1);
        }
    }
    send_cmd(SPI_SYNC);
    */
}

WaveguideDisplay::WaveguideDisplay() {
    mutex_ = xSemaphoreCreateMutex();
    InitLvgl();
}

WaveguideDisplay::~WaveguideDisplay() {
    if (screen_obj_) lv_obj_del(screen_obj_);
    if (draw_buf1_) free(draw_buf1_);
    if (mutex_) vSemaphoreDelete(mutex_);
}

void WaveguideDisplay::InitLvgl() {
    if (!s_lvgl_inited) { lv_init(); s_lvgl_inited = true; }
    // 横屏：宽 400, 高 300。确保刷新周期与 LVGL 时钟一致，避免长阻塞
    lv_display_t* disp = lv_display_create(400, 300);
    lv_tick_set_cb([](){ return (uint32_t)(esp_timer_get_time()/1000ULL); });
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    draw_buf1_ = static_cast<lv_color_t*>(malloc(400 * 16 * sizeof(lv_color_t)));
    if (draw_buf1_) {
        lv_display_set_buffers(disp, draw_buf1_, nullptr, 400 * 16 * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    }
    lv_display_set_user_data(disp, this);
    lv_display_set_flush_cb(disp, [](lv_display_t* d, const lv_area_t* a, uint8_t* px){ WaveguideDisplay::LvglFlushCb(d, a, px); });
    // 根屏设置为黑色（对 AR 等于"透明/不发光"），外层建立 1px 白边框，内层内容透明，模仿 BWR 结构
    lv_obj_t* root = lv_screen_active();
    lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    // frame: 白色边框（仿 ST7306 窗口边框），占满 400x300
    frame_obj_ = lv_obj_create(root);
    lv_obj_set_size(frame_obj_, 400, 300);
    lv_obj_set_style_bg_opa(frame_obj_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(frame_obj_, 1, 0);
    lv_obj_set_style_border_color(frame_obj_, lv_color_white(), 0);
    lv_obj_center(frame_obj_);

    // screen_obj_: 作为内容容器，透明，内缩 2px 以免压住边框
    screen_obj_ = lv_obj_create(frame_obj_);
    lv_obj_set_size(screen_obj_, 400 - 2*2, 300 - 2*2);
    lv_obj_set_style_bg_opa(screen_obj_, LV_OPA_TRANSP, 0);
    lv_obj_align(screen_obj_, LV_ALIGN_CENTER, 0, 0);

    // 初始化 AR 面板（一次性）
    ESP_LOGI(TAG, "Init AR panel");
    // panel_init();  // 注释掉 reference code API
    // 提升亮度（根据文档：25Hz 自刷新时 0~21331），先取中等亮度
    // wr_lum_reg(2000);  // 注释掉 reference code API
    // 简单欢迎文案，确认画面（去除版本号等信息）
    BuildSimpleFrameWithLabel("", &notification_label_);
    lv_refr_now(NULL);
}

bool WaveguideDisplay::Lock(int timeout_ms) {
    if (!mutex_) return true;
    TickType_t to = timeout_ms==0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(mutex_, to) == pdTRUE;
}
void WaveguideDisplay::Unlock() { if (mutex_) xSemaphoreGive(mutex_); }
void WaveguideDisplay::Update() {}

void WaveguideDisplay::SetStatus(const char* status) { (void)status; }
void WaveguideDisplay::ShowNotification(const char* notification, int duration_ms) {
    (void)duration_ms;
    if (!Lock(100)) return;
    // 从硬件层清除 400x300 可视窗口，避免任何残留
    panel_clear_visible_window();
    if (!notification_label_) { BuildSimpleFrameWithLabel("", &notification_label_); }
    // 设置新文本并立即刷新
    lv_label_set_text(notification_label_, notification ? notification : "");
    lv_refr_now(NULL);
    Unlock();
}
void WaveguideDisplay::SetEmotion(const char* emotion) { (void)emotion; }
void WaveguideDisplay::SetChatMessage(const char* role, const char* content) {
    if (!Lock(100)) return;
    // 仅显示 assistant 的回答
    if (role && strcmp(role, "assistant") != 0) {
        Unlock();
        return;
    }
    // 移除可能残留的版本/提示文字，避免与对话重叠
    if (notification_label_) { lv_obj_del(notification_label_); notification_label_ = nullptr; }
    if (hint_label_) { lv_obj_del(hint_label_); hint_label_ = nullptr; }

    // 使用 LVGL 背景快速清屏（黑底），替代整屏硬件清屏以降低延迟
    lv_obj_set_style_bg_color(screen_obj_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen_obj_, LV_OPA_COVER, 0);

    if (!chat_label_) {
        chat_label_ = lv_label_create(screen_obj_);
        lv_obj_set_style_bg_opa(chat_label_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_font(chat_label_, &font_puhui_30_4, 0);
        lv_obj_set_style_text_color(chat_label_, lv_color_hex(0x00FF00), 0);
        // 不使用缩放，避免换行测量与渲染不一致
        // 控制文本区域宽度，避免每行末尾字符被裁切
        lv_obj_set_width(chat_label_, 320);
        lv_label_set_long_mode(chat_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(chat_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(chat_label_, LV_ALIGN_TOP_LEFT, 12, 12);
    } else {
        // 确保样式为最新放大与字体
        lv_obj_set_style_text_font(chat_label_, &font_puhui_30_4, 0);
        lv_obj_set_width(chat_label_, 320);
        lv_obj_set_style_text_align(chat_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(chat_label_, LV_ALIGN_TOP_LEFT, 12, 12);
    }
    // 追加空格来防止末尾字符被紧贴边界裁切（渲染安全边距）
    if (content) {
        static char buf[1024];
        size_t len = strnlen(content, sizeof(buf) - 3);
        memcpy(buf, content, len);
        buf[len++] = ' ';
        buf[len++] = ' ';
        buf[len] = '\0';
        lv_label_set_text(chat_label_, buf);
    } else {
        lv_label_set_text(chat_label_, "");
    }
    lv_refr_now(NULL);
    Unlock();
}
void WaveguideDisplay::EnterChatMode() {}
void WaveguideDisplay::OnNetworkReady() {}
void WaveguideDisplay::StartNetworkErrorBlink() {}
void WaveguideDisplay::StopNetworkErrorBlink() {}
void WaveguideDisplay::ShowWelcome() {}
void WaveguideDisplay::ShowBranchHint(const char* text, bool highlight_red) {
    (void)highlight_red;
    if (!Lock(100)) return;
    panel_clear_visible_window();
    if (!hint_label_) { BuildSimpleFrameWithLabel("", &hint_label_); }
    // 设置新文本并立即刷新
    lv_label_set_text(hint_label_, text ? text : "");
    lv_refr_now(NULL);
    Unlock();
}
void WaveguideDisplay::ClearBranchHint() {
    if (!Lock(100)) return;
    if (hint_label_) {
        lv_obj_del(hint_label_);
        hint_label_ = nullptr;
        lv_refr_now(NULL);
    }
    Unlock();
}

void WaveguideDisplay::LvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    // 将 RGB565 转 4bit 灰度写入面板缓存，延后合并一次 SYNC，降低刷新率/热量
    (void)disp;
    auto* self = static_cast<WaveguideDisplay*>(lv_display_get_user_data(disp));
    int16_t w = area->x2 - area->x1 + 1;
    int16_t h = area->y2 - area->y1 + 1;
    const uint16_t* src = reinterpret_cast<const uint16_t*>(px_map);
    // 面板以 2 像素打包 1 字节，灰度4bit
    // 简单亮度近似：Y = 0.299R + 0.587G + 0.114B → 0..255 → 4bit
    // 注意：为了演示，逐行推送；后续可批量行缓冲优化
    // 将 400x300 画面居中到 640x480 面板坐标（横屏）
    const int col_off = (640 - 400) / 2; // 120
    const int row_off = (480 - 300) / 2; // 90
    for (int y = 0; y < h; ++y) {
        int row = area->y1 + y;
        uint16_t panel_row = (uint16_t)(row_off + row);
        // 每次调用把整行写入缓存：len = ceil(w/2)
        int packed_len = (w + 1) / 2;
        static uint8_t line_buf[320];
        for (int x = 0; x < w; x += 2) {
            // 水平镜像：从右向左取像素
            int xr0 = (w - 1 - x);
            int xr1 = (w - 1 - (x + 1));
            auto pix0 = src[y * w + xr0];
            uint8_t r0 = (pix0 >> 11) & 0x1F;
            uint8_t g0 = (pix0 >> 5) & 0x3F;
            uint8_t b0 = (pix0) & 0x1F;
            uint16_t r0_8 = (r0 * 527 + 23) >> 6;
            uint16_t g0_8 = (g0 * 259 + 33) >> 6;
            uint16_t b0_8 = (b0 * 527 + 23) >> 6;
            uint16_t y0_8 = (r0_8 * 30 + g0_8 * 59 + b0_8 * 11) / 100;
            uint8_t g0_4 = (uint8_t)(y0_8 >> 4);
            uint8_t g1_4 = 0;
            if (x + 1 < w) {
                auto pix1 = src[y * w + xr1];
                uint8_t r1 = (pix1 >> 11) & 0x1F;
                uint8_t g1 = (pix1 >> 5) & 0x3F;
                uint8_t b1 = (pix1) & 0x1F;
                uint16_t r1_8 = (r1 * 527 + 23) >> 6;
                uint16_t g1_8 = (g1 * 259 + 33) >> 6;
                uint16_t b1_8 = (b1 * 527 + 23) >> 6;
                uint16_t y1_8 = (r1_8 * 30 + g1_8 * 59 + b1_8 * 11) / 100;
                g1_4 = (uint8_t)(y1_8 >> 4);
            }
            line_buf[x / 2] = (uint8_t)((g0_4 << 4) | (g1_4 & 0x0F));
        }
        // 写入缓存并同步
        uint16_t panel_col = (uint16_t)(col_off + area->x1);
        // spi_wr_cache(panel_col, panel_row, line_buf, packed_len);  // 注释掉 reference code API
        (void)panel_col; (void)panel_row; (void)line_buf; (void)packed_len;  // 避免未使用变量警告
        if ((y & 0x0F) == 0) {
            vTaskDelay(0);
        }
    }
    // 合并触发 SYNC：将节流降到 20ms，降低显示滞后
    if (self) {
        uint64_t now = esp_timer_get_time();
        if (now - self->last_sync_us_ >= 20000ULL) {
            self->last_sync_us_ = now;
            // send_cmd(SPI_SYNC);  // 注释掉 reference code API
        }
    }
    lv_display_flush_ready(disp);
}
void WaveguideDisplay::CommitTimerThunk(void* arg) { reinterpret_cast<WaveguideDisplay*>(arg)->CommitDirty(); }
void WaveguideDisplay::ScheduleCommit(uint64_t delay_us) { (void)delay_us; }
void WaveguideDisplay::CommitDirty() {}

void WaveguideDisplay::BuildSimpleFrameWithLabel(const char* text, lv_obj_t** out_label) {
    if (!frame_obj_) return;
    if (*out_label == nullptr) {
        *out_label = lv_label_create(screen_obj_);
        lv_obj_set_style_bg_opa(*out_label, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_font(*out_label, &font_puhui_30_4, 0);
        lv_obj_set_style_text_color(*out_label, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_transform_zoom(*out_label, 320, 0);
        lv_obj_center(*out_label);
    }
    lv_label_set_text(*out_label, text ? text : "");
}



