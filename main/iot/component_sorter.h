/**
 * @file component_sorter.h
 * @brief 元器件分拣器主逻辑 - 协调LED、MAX98357A功放、数据库、HTTP/WS服务器
 */
#ifndef _COMPONENT_SORTER_H_
#define _COMPONENT_SORTER_H_

#include <string>
#include <map>
#include "component_database.h"
#include "component_led_control.h"
#include "buzzer_control.h"
#include "component_http_server.h"
#include "component_ws_server.h"

// 用户颜色结构
struct UserColor {
    uint8_t user_id;
    uint8_t r, g, b;
    std::string name;
};

class ComponentSorter {
public:
    ComponentSorter();
    ~ComponentSorter();

    // 初始化所有组件
    bool Initialize();

    // 检查是否已初始化
    bool IsInitialized() const { return initialized_; }

    // 检查是否正在运行
    bool IsRunning() const { return http_server_.IsRunning() || ws_server_.IsRunning(); }

    // 启动服务
    bool Start();

    // 停止服务
    void Stop();

    // 查找元器件并显示
    // 返回true表示找到
    bool FindAndHighlight(const std::string& part_number);

    // 获取数据库实例（用于外部访问）
    ComponentDatabase* GetDatabase() { return &database_; }

    // 获取LED控制实例
    ComponentLedControl* GetLedControl() { return &led_control_; }

    // 获取提示音控制实例
    BuzzerControl* GetBuzzer() { return &buzzer_; }

    // 广播消息到WebSocket客户端
    void Broadcast(const std::string& message);

    // 获取状态信息JSON
    std::string GetStatusJson() const;

private:
    ComponentDatabase database_;
    ComponentLedControl led_control_;
    BuzzerControl buzzer_;
    ComponentHttpServer http_server_;
    ComponentWsServer ws_server_;
    bool initialized_;
    bool running_;
    TaskHandle_t led_update_task_handle_;  // LED更新任务句柄

    // 用户颜色管理（支持多人同时查找）
    std::map<uint8_t, UserColor> user_colors_;
    std::map<std::string, uint8_t> active_searches_;  // 正在搜索的元器件->用户ID

    // 获取或分配用户颜色
    UserColor GetUserColor(uint8_t user_id);

    // 清除超时搜索
    void ClearExpiredSearches();

    // LED更新任务（后台刷新闪烁状态）
    static void LedUpdateTask(void* param);

    // HTTP请求回调处理
    bool HandleHttpRequest(const std::string& uri, const std::string& method, 
                          const std::string& body, std::string& response);

    // WebSocket消息回调处理
    void HandleWsMessage(const WsMessage& msg);

    // 构建查找响应JSON
    std::string BuildFindResponseJson(bool success, const std::string& part_number,
                                     uint8_t led_index, uint8_t group,
                                     uint8_t r, uint8_t g, uint8_t b,
                                     const std::string& error = "", uint8_t user_id = 0);
};

#endif // _COMPONENT_SORTER_H_
