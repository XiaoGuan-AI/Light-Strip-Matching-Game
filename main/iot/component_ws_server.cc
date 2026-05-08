/**
 * @file component_ws_server.cc
 * @brief WebSocket服务器实现
 */
#include "component_ws_server.h"
#include "component_config.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <cJSON.h>
#include <cstring>
#include <algorithm>

#define TAG "ComponentWsServer"

// WebSocket URI
#define WS_URI "/ws"

// 全局实例指针
static ComponentWsServer* g_ws_server = nullptr;

ComponentWsServer::ComponentWsServer() 
    : port_(WS_SERVER_PORT), server_(nullptr), server_running_(false) {
    g_ws_server = this;
}

ComponentWsServer::~ComponentWsServer() {
    Stop();
    g_ws_server = nullptr;
}

bool ComponentWsServer::Initialize(uint16_t port) {
    if (server_running_) {
        ESP_LOGW(TAG, "Server already running");
        return true;
    }
    port_ = port;
    ESP_LOGI(TAG, "WebSocket server initialized on port %d", port_);
    return true;
}

bool ComponentWsServer::Start() {
    if (server_running_) {
        return true;
    }

    if (!register_websocket_uri()) {
        ESP_LOGE(TAG, "Failed to register WebSocket URI");
        return false;
    }

    server_running_ = true;
    ESP_LOGI(TAG, "WebSocket server started on port %d", port_);
    return true;
}

void ComponentWsServer::Stop() {
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    server_running_ = false;
    clients_.clear();
    ESP_LOGI(TAG, "WebSocket server stopped");
}

void ComponentWsServer::BroadcastMessage(const std::string& message) {
    if (!server_running_ || clients_.empty()) {
        return;
    }

    // 向所有客户端广播消息
    httpd_ws_frame_t ws_pkt = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)message.c_str(),
        .len = message.length()
    };
    
    for (int client_fd : clients_) {
        httpd_ws_send_frame_async(server_, client_fd, &ws_pkt);
    }
}

void ComponentWsServer::SetMessageCallback(WsMessageCallback callback) {
    message_callback_ = callback;
}

bool ComponentWsServer::register_websocket_uri() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port_;
    config.ctrl_port = port_ + 1;
    config.task_priority = tskIDLE_PRIORITY + 5;
    config.stack_size = 4096;
    config.core_id = 0;

    // 创建HTTP服务器
    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return false;
    }

    // 注册WebSocket处理程序
    httpd_uri_t ws_uri = {
        .uri = WS_URI,
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = this,
        .is_websocket = true
    };

    err = httpd_register_uri_handler(server_, &ws_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket URI registration failed: %s", esp_err_to_name(err));
        httpd_stop(server_);
        server_ = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "WebSocket URI registered: %s", WS_URI);
    return true;
}

esp_err_t ComponentWsServer::ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket connection accepted");
        
        // 获取客户端fd
        int client_fd = httpd_req_to_sockfd(req);
        
        // 添加到客户端列表
        if (g_ws_server) {
            g_ws_server->clients_.push_back(client_fd);
            ESP_LOGI(TAG, "Client added, total clients: %d", g_ws_server->clients_.size());
        }
        
        // 发送欢迎消息
        const char* welcome_msg = "{\"type\":\"connected\",\"message\":\"Welcome to Component Sorter\"}";
        httpd_ws_frame_t ws_pkt = {
            .final = true,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)welcome_msg,
            .len = strlen(welcome_msg)
        };
        httpd_ws_send_frame(req, &ws_pkt);

        // 持续接收WebSocket数据
        while (true) {
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.type = HTTPD_WS_TYPE_TEXT;

            esp_err_t err = httpd_ws_recv_frame(req, &ws_pkt, 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "ws_recv_frame failed: %s", esp_err_to_name(err));
                break;
            }

            if (ws_pkt.len == 0) {
                break;
            }

            if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
                ESP_LOGI(TAG, "WebSocket close frame received");
                break;
            }

            uint8_t* buf = (uint8_t*)malloc(ws_pkt.len + 1);
            if (buf == nullptr) {
                break;
            }

            ws_pkt.payload = buf;
            err = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "ws_recv_frame failed: %s", esp_err_to_name(err));
                free(buf);
                break;
            }
            buf[ws_pkt.len] = 0;

            ESP_LOGI(TAG, "Received WebSocket message: %s", buf);

            // 处理消息并生成响应
            if (g_ws_server) {
                std::string response = "{\"type\":\"response\",\"success\":true,\"data\":\"";
                response += (char*)buf;
                response += "\"}";
                httpd_ws_frame_t resp_pkt = {
                    .final = true,
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t*)response.c_str(),
                    .len = response.length()
                };
                httpd_ws_send_frame(req, &resp_pkt);
            }

            free(buf);
        }

        // 从客户端列表中移除
        if (g_ws_server) {
            auto& clients = g_ws_server->clients_;
            clients.erase(std::remove(clients.begin(), clients.end(), client_fd), clients.end());
            ESP_LOGI(TAG, "Client removed, remaining clients: %d", clients.size());
        }

        ESP_LOGI(TAG, "WebSocket connection closed");
        return ESP_OK;
    }
    return ESP_FAIL;
}

void ComponentWsServer::handle_message(const std::string& message) {
    if (message_callback_) {
        WsMessage msg;
        msg.type = parse_message_type(message);
        msg.data = message;
        message_callback_(msg);
    }
}

WsMessageType ComponentWsServer::parse_message_type(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        return WsMessageType::RESPONSE;
    }

    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!type || !cJSON_IsString(type)) {
        cJSON_Delete(root);
        return WsMessageType::RESPONSE;
    }

    WsMessageType msg_type = WsMessageType::RESPONSE;
    std::string type_str = type->valuestring;

    if (type_str == "find") {
        msg_type = WsMessageType::FIND_COMPONENT;
    } else if (type_str == "add") {
        msg_type = WsMessageType::ADD_COMPONENT;
    } else if (type_str == "remove") {
        msg_type = WsMessageType::REMOVE_COMPONENT;
    } else if (type_str == "get_all") {
        msg_type = WsMessageType::GET_ALL;
    } else if (type_str == "reset") {
        msg_type = WsMessageType::RESET_DB;
    }

    cJSON_Delete(root);
    return msg_type;
}

void ComponentWsServer::send_response(httpd_req_t *req, bool success, 
                                      const std::string& data, const std::string& error) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", success);
    cJSON_AddStringToObject(root, "data", data.c_str());
    
    if (!error.empty()) {
        cJSON_AddStringToObject(root, "error", error.c_str());
    }

    char* json_str = cJSON_Print(root);
    
    httpd_ws_frame_t ws_pkt = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)json_str,
        .len = strlen(json_str)
    };
    httpd_ws_send_frame(req, &ws_pkt);
    
    free(json_str);
    cJSON_Delete(root);
}

// C接口实现
extern "C" {

static httpd_handle_t g_httpd_handle = nullptr;

esp_err_t component_ws_server_start(uint16_t port) {
    if (g_httpd_handle) {
        ESP_LOGW(TAG, "Server already started");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = port + 1;

    esp_err_t err = httpd_start(&g_httpd_handle, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WebSocket server started on port %d", port);
    return ESP_OK;
}

void component_ws_server_stop() {
    if (g_httpd_handle) {
        httpd_stop(g_httpd_handle);
        g_httpd_handle = nullptr;
        ESP_LOGI(TAG, "WebSocket server stopped");
    }
}

void component_ws_server_broadcast(const char* message) {
    // 广播消息到所有WebSocket客户端
    // 需要在有客户端连接时实现
    ESP_LOGI(TAG, "Broadcast: %s", message);
}

} // extern "C"
