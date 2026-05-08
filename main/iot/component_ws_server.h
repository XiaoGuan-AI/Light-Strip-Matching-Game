/**
 * @file component_ws_server.h
 * @brief WebSocket服务器 - 实时推送元器件查找结果
 */
#ifndef _COMPONENT_WS_SERVER_H_
#define _COMPONENT_WS_SERVER_H_

#include <string>
#include <vector>
#include <functional>
#include <esp_http_server.h>
#include "component_config.h"

// WebSocket消息类型
enum class WsMessageType {
    FIND_COMPONENT,     // 查找元器件
    ADD_COMPONENT,      // 添加元器件
    REMOVE_COMPONENT,   // 删除元器件
    GET_ALL,            // 获取所有元器件
    RESET_DB,           // 重置数据库
    RESPONSE            // 响应消息
};

struct WsMessage {
    WsMessageType type;
    std::string data;
    bool success;
    std::string error_msg;
};

// WebSocket消息回调
using WsMessageCallback = std::function<void(const WsMessage& msg)>;

class ComponentWsServer {
public:
    ComponentWsServer();
    ~ComponentWsServer();

    // 初始化WebSocket服务器
    bool Initialize(uint16_t port = WS_SERVER_PORT);

    // 启动服务器
    bool Start();

    // 停止服务器
    void Stop();

    // 发送消息到所有客户端
    void BroadcastMessage(const std::string& message);

    // 设置消息回调
    void SetMessageCallback(WsMessageCallback callback);

    // 获取服务器状态
    bool IsRunning() const { return server_running_; }

    // 获取连接客户端数量
    size_t GetConnectedClients() const { return clients_.size(); }

private:
    uint16_t port_;
    httpd_handle_t server_;
    bool server_running_;
    std::vector<int> clients_;
    WsMessageCallback message_callback_;

    // WebSocket处理函数
    static esp_err_t ws_handler(httpd_req_t *req);
    static esp_err_t ws_ping_handler(httpd_req_t *req);

    // 注册WebSocket URI
    bool register_websocket_uri();

    // 处理接收到的消息
    void handle_message(const std::string& message);

    // 发送JSON响应
    void send_response(httpd_req_t *req, bool success, const std::string& data, const std::string& error = "");

    // 解析消息类型
    WsMessageType parse_message_type(const std::string& json);
};

// C接口用于ESP-IDF注册
extern "C" {
    esp_err_t component_ws_server_start(uint16_t port);
    void component_ws_server_stop();
    void component_ws_server_broadcast(const char* message);
}

#endif // _COMPONENT_WS_SERVER_H_
