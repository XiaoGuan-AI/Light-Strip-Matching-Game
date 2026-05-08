#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <functional>

#include "led/led.h"
#include "backlight.h"

void* create_board();
class AudioCodec;
class Display;

/**
 * @brief 打印结果状态 (前向声明)
 */
enum class PrintStatus;
struct PrintResult;

class Board {
private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作
    virtual std::string GetBoardJson() = 0;

protected:
    Board();
    std::string GenerateUuid();

    // 软件生成的设备唯一标识
    std::string uuid_;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual Backlight* GetBacklight() { return nullptr; }
    virtual Led* GetLed();
    virtual AudioCodec* GetAudioCodec() { return nullptr; }
    virtual Display* GetDisplay();
    virtual Http* CreateHttp() = 0;
    virtual WebSocket* CreateWebSocket() = 0;
    virtual Mqtt* CreateMqtt() = 0;
    virtual Udp* CreateUdp() = 0;
    virtual void StartNetwork() = 0;
    virtual const char* GetNetworkStateIcon() = 0;
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    virtual std::string GetJson();
    virtual void SetPowerSaveMode(bool enabled) = 0;
    // Ensure concrete board display is allocated and ready (no-op by default)
    virtual void EnsureDisplayEnabled() {}

    // ============================================
    // 打印功能接口 (仅 AI-Printer 板支持)
    // ============================================

    /**
     * @brief 检查是否支持打印功能
     */
    virtual bool SupportsPrinting() { return false; }

    /**
     * @brief 处理打印通知
     * @param task_id 任务 ID
     * @param url 位图下载 URL
     * @param width 位图宽度 (像素)
     * @param height 位图高度 (像素)
     * @param checksum CRC32 校验值 (8 位十六进制)
     * @param on_complete 完成回调 (status, error_msg)
     */
    virtual void HandlePrintNotification(
        const std::string& task_id,
        const std::string& url,
        uint16_t width,
        uint16_t height,
        const std::string& checksum,
        std::function<void(const std::string& status, const std::string& error)> on_complete,
        const std::string& preview_url = "") {
        // 默认实现：不支持打印
        if (on_complete) {
            on_complete("not_supported", "Printing not supported on this board");
        }
    }

    /**
     * @brief 处理图片预览后的打印确认
     *
     * 用于语音文生图流程：
     * 1. 收到 image_preview 消息后，Display 显示预览图
     * 2. 调用此方法保存待打印任务信息
     * 3. 用户按 BOOT 按钮确认后执行打印
     *
     * @param task_id 任务 ID
     * @param bitmap_url 位图下载 URL
     * @param width 位图宽度 (像素)
     * @param height 位图高度 (像素)
     * @param checksum CRC32 校验值
     * @param on_complete 打印完成回调
     * @param preview_url RGB565 彩色预览图 URL (可选)
     */
    virtual void HandleImagePreviewForPrint(
        const std::string& task_id,
        const std::string& bitmap_url,
        uint16_t width,
        uint16_t height,
        const std::string& checksum,
        std::function<void(const std::string& status, const std::string& error)> on_complete,
        const std::string& preview_url = "") {
        // 默认实现：直接调用打印处理
        // 具体板子可以重写此方法实现预览确认逻辑
        HandlePrintNotification(task_id, bitmap_url, width, height, checksum, on_complete, preview_url);
    }

    /**
     * @brief 仅更新 LCD 预览图，不影响待打印任务
     *
     * 用于后台上色完成后，将 LCD 上的线稿预览替换为彩色预览。
     * 仅在 task_id 匹配当前待确认任务时生效。
     *
     * @param task_id 任务 ID（必须匹配当前待确认任务）
     * @param preview_url 新的 RGB565 彩色预览图 URL
     */
    virtual void HandlePreviewUpdate(
        const std::string& task_id,
        const std::string& preview_url) {
        // 默认实现：忽略（无LCD的板子不需要处理）
    }
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
