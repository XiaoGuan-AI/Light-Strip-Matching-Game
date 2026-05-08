/**
 * @file component_http_server.cc
 * @brief HTTP服务器实现 - 网页管理界面
 */
#include "component_http_server.h"
#include "component_config.h"
#include <esp_log.h>
#include <cJSON.h>
#include <cstring>

#define TAG "ComponentHttpServer"
#define STRINGIFY_VALUE(x) #x
#define STRINGIFY_MACRO(x) STRINGIFY_VALUE(x)

extern const uint8_t jsQR_js_start[] asm("_binary_jsQR_js_start");
extern const uint8_t jsQR_js_end[] asm("_binary_jsQR_js_end");

// 全局实例指针
static ComponentHttpServer* g_http_server = nullptr;
static const char* kInlineJsQrToken = "__INLINE_JSQR_SCRIPT__";

namespace {

void SetCorsHeaders(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
}

esp_err_t SendPreflightResponse(httpd_req_t* req) {
    SetCorsHeaders(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
}

}  // namespace

ComponentHttpServer::ComponentHttpServer() 
    : port_(HTTP_SERVER_PORT), server_(nullptr), server_running_(false) {
    g_http_server = this;
}

ComponentHttpServer::~ComponentHttpServer() {
    Stop();
    g_http_server = nullptr;
}

bool ComponentHttpServer::Initialize(uint16_t port) {
    if (server_running_) {
        ESP_LOGW(TAG, "Server already running");
        return true;
    }
    port_ = port;
    ESP_LOGI(TAG, "HTTP server initialized on port %d", port_);
    return true;
}

bool ComponentHttpServer::Start() {
    if (server_running_) {
        return true;
    }

    if (!register_uri_handlers()) {
        ESP_LOGE(TAG, "Failed to register URI handlers");
        return false;
    }

    server_running_ = true;
    ESP_LOGI(TAG, "HTTP server started on port %d", port_);
    return true;
}

void ComponentHttpServer::Stop() {
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    server_running_ = false;
    ESP_LOGI(TAG, "HTTP server stopped");
}

void ComponentHttpServer::SetRequestCallback(HttpRequestCallback callback) {
    request_callback_ = callback;
}

bool ComponentHttpServer::register_uri_handlers() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port_;
    config.ctrl_port = port_ + 1;
    config.task_priority = tskIDLE_PRIORITY + 3;
    // 查找接口现在会拼装更多 JSON 和页面数据，4KB 容易导致 httpd 任务栈溢出。
    config.stack_size = 8192;
    config.core_id = 0;
    config.max_uri_handlers = 32;

    // 创建HTTP服务器
    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return false;
    }

    // 注册根路径 - 返回Web界面
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &root_uri);

    httpd_uri_t jsqr_uri = {
        .uri = "/jsqr.js",
        .method = HTTP_GET,
        .handler = jsqr_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &jsqr_uri);

    // 注册API帮助和状态接口
    httpd_uri_t api_info_uri = {
        .uri = "/api",
        .method = HTTP_GET,
        .handler = api_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_info_uri);

    httpd_uri_t api_status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &api_status_uri);

    httpd_uri_t game_start_uri = {
        .uri = "/api/start",
        .method = HTTP_POST,
        .handler = api_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &game_start_uri);

    httpd_uri_t game_fire_uri = {
        .uri = "/api/fire",
        .method = HTTP_POST,
        .handler = api_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &game_fire_uri);

    httpd_uri_t game_start_options_uri = {
        .uri = "/api/start",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &game_start_options_uri);

    httpd_uri_t game_fire_options_uri = {
        .uri = "/api/fire",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &game_fire_options_uri);

    // 注册查找API
    httpd_uri_t find_uri = {
        .uri = "/api/find",
        .method = HTTP_POST,
        .handler = find_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &find_uri);

    httpd_uri_t find_options_uri = {
        .uri = "/api/find",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &find_options_uri);

    // 注册元器件列表API
    httpd_uri_t components_uri = {
        .uri = "/api/components",
        .method = HTTP_GET,
        .handler = components_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &components_uri);

    // 注册管理接口
    httpd_uri_t backup_components_uri = {
        .uri = "/api/components/backup",
        .method = HTTP_GET,
        .handler = api_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &backup_components_uri);

    httpd_uri_t restore_components_uri = {
        .uri = "/api/components/restore",
        .method = HTTP_POST,
        .handler = api_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &restore_components_uri);

    httpd_uri_t clear_components_uri = {
        .uri = "/api/components/clear",
        .method = HTTP_POST,
        .handler = api_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &clear_components_uri);

    httpd_uri_t update_components_uri = {
        .uri = "/api/components/update",
        .method = HTTP_POST,
        .handler = api_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &update_components_uri);

    httpd_uri_t delete_components_uri = {
        .uri = "/api/components/delete",
        .method = HTTP_POST,
        .handler = api_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &delete_components_uri);

    // 注册添加元器件API (POST)
    httpd_uri_t add_component_uri = {
        .uri = "/api/components/add",
        .method = HTTP_POST,
        .handler = add_component_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &add_component_uri);

    httpd_uri_t add_component_options_uri = {
        .uri = "/api/components/add",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &add_component_options_uri);

    // 保留批量接口用于兼容旧脚本，Web UI 主流程已切换为逐条实时录入
    httpd_uri_t batch_add_component_uri = {
        .uri = "/api/components/batch_add",
        .method = HTTP_POST,
        .handler = api_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &batch_add_component_uri);

    httpd_uri_t restore_components_options_uri = {
        .uri = "/api/components/restore",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &restore_components_options_uri);

    httpd_uri_t clear_components_options_uri = {
        .uri = "/api/components/clear",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &clear_components_options_uri);

    httpd_uri_t update_components_options_uri = {
        .uri = "/api/components/update",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &update_components_options_uri);

    httpd_uri_t delete_components_options_uri = {
        .uri = "/api/components/delete",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &delete_components_options_uri);

    httpd_uri_t batch_add_component_options_uri = {
        .uri = "/api/components/batch_add",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &batch_add_component_options_uri);

    ESP_LOGI(TAG, "HTTP URI handlers registered");
    return true;
}

bool ComponentHttpServer::read_request_body(httpd_req_t *req, std::string& body) {
    body.clear();

    int remaining = req->content_len;
    if (remaining <= 0) {
        return true;
    }

    body.reserve(remaining);
    while (remaining > 0) {
        char chunk[256];
        int to_read = remaining < static_cast<int>(sizeof(chunk)) ? remaining : static_cast<int>(sizeof(chunk));
        int ret = httpd_req_recv(req, chunk, to_read);
        if (ret <= 0) {
            ESP_LOGE(TAG, "Failed to receive request body, ret=%d, remaining=%d", ret, remaining);
            return false;
        }

        body.append(chunk, ret);
        remaining -= ret;
    }

    return true;
}

esp_err_t ComponentHttpServer::root_handler(httpd_req_t *req) {
    const char* html = get_html_page();

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    SetCorsHeaders(req);
    const char* token_pos = strstr(html, kInlineJsQrToken);
    if (!token_pos) {
        return httpd_resp_send(req, html, strlen(html));
    }

    const size_t prefix_len = static_cast<size_t>(token_pos - html);
    const char* suffix = token_pos + strlen(kInlineJsQrToken);
    const size_t jsqr_len = jsQR_js_end - jsQR_js_start;
    static const char* kJsQrPrefix =
        "<script>var module=undefined;var exports=undefined;var define=undefined;\n";
    static const char* kJsQrSuffix = "\n</script>\n";

    esp_err_t err = httpd_resp_send_chunk(req, html, prefix_len);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_send_chunk(req, kJsQrPrefix, strlen(kJsQrPrefix));
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_send_chunk(req,
                                reinterpret_cast<const char*>(jsQR_js_start),
                                jsqr_len);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_send_chunk(req, kJsQrSuffix, strlen(kJsQrSuffix));
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_send_chunk(req, suffix, strlen(suffix));
    if (err != ESP_OK) {
        return err;
    }

    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t ComponentHttpServer::jsqr_handler(httpd_req_t *req) {
    const size_t script_len = jsQR_js_end - jsQR_js_start;
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    SetCorsHeaders(req);
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req,
                           reinterpret_cast<const char*>(jsQR_js_start),
                           script_len);
}

esp_err_t ComponentHttpServer::api_handler(httpd_req_t *req) {
    SetCorsHeaders(req);

    // 获取 URI
    std::string uri(req->uri);
    
    // 处理 POST 请求
    if (req->method == HTTP_POST) {
        std::string body;
        if (!read_request_body(req, body)) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, "{\"success\":false,\"error\":\"Failed to read request body\"}",
                            strlen("{\"success\":false,\"error\":\"Failed to read request body\"}"));
            return ESP_FAIL;
        }
        
        std::string response;
        
        // 转发请求到 callback
        if (g_http_server && g_http_server->request_callback_) {
            // 检查是否是 components 相关的请求
            if (uri.find("/api/components/update") != std::string::npos) {
                g_http_server->request_callback_("/api/components/update", "POST", body, response);
            } else if (uri.find("/api/components/delete") != std::string::npos) {
                g_http_server->request_callback_("/api/components/delete", "POST", body, response);
            } else if (uri.find("/api/components/clear") != std::string::npos) {
                g_http_server->request_callback_("/api/components/clear", "POST", body, response);
            } else if (uri.find("/api/components/restore") != std::string::npos) {
                g_http_server->request_callback_("/api/components/restore", "POST", body, response);
            } else {
                // 其他 POST 请求
                g_http_server->request_callback_(uri.c_str(), "POST", body, response);
            }
        }

        if (response.empty()) {
            response = "{\"success\":false,\"error\":\"No response from handler\"}";
        }
        
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, response.c_str(), response.length());
    }
    
    // 处理 GET 请求
    if (req->method == HTTP_GET) {
        std::string response;
        
        // 转发 GET 请求到 callback
        if (g_http_server && g_http_server->request_callback_) {
            if (uri.find("/api/components/backup") != std::string::npos) {
                g_http_server->request_callback_("/api/components/backup", "GET", "", response);
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_send(req, response.c_str(), response.length());
            } else if (uri == "/api/status") {
                g_http_server->request_callback_("/api/status", "GET", "", response);
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_send(req, response.c_str(), response.length());
            } else if (uri == "/api" || uri == "/api/" || uri == "/api/components") {
                // 这些已有专门handler，不需要转发
            }
        }
        
        // 默认返回帮助信息
        const char* api_info = R"JSON({
        "status": "ok",
        "endpoints": {
            "GET /": "灯带消消乐界面",
            "POST /api/start": "开始/重开游戏",
            "POST /api/fire": "发射同色灯珠碰撞消除 (body: {color: 0..3})",
            "GET /api/components": "获取所有元器件",
            "GET /api/components/backup": "备份元器件数据",
            "POST /api/find": "查找元器件 (body: {part_number: 'C00001'})",
            "POST /api/components/add": "添加元器件",
            "POST /api/components/batch_add": "兼容接口: 批量添加元器件",
            "POST /api/components/update": "更新元器件",
            "POST /api/components/delete": "删除元器件",
            "POST /api/components/clear": "清空所有元器件",
            "POST /api/components/restore": "恢复元器件数据"
        }
    })JSON";
    
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, api_info, strlen(api_info));
    }
    
    // 其他方法不支持
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"error\":\"Method not supported\"}", strlen("{\"error\":\"Method not supported\"}"));
}

esp_err_t ComponentHttpServer::find_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "find_handler called");
    SetCorsHeaders(req);
    
    // 读取请求体
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        ESP_LOGE(TAG, "find_handler: failed to receive request body, ret=%d", ret);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    ESP_LOGI(TAG, "find_handler received: [%s]", buf);

    // 解析JSON
    cJSON* root = cJSON_Parse(buf);
    if (!root) {
        const char* err_resp = "{\"success\":false,\"error\":\"Invalid JSON\"}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, err_resp, strlen(err_resp));
    }

    cJSON* part_number = nullptr;
    
    // 支持两种JSON格式：
    // 1. 对象格式: {"part_number": "C00001"}
    // 2. 数组格式: [{"part_number": "C00001"}]
    ESP_LOGI(TAG, "find_handler: root type: IsArray=%d, IsObject=%d", cJSON_IsArray(root), cJSON_IsObject(root));
    
    if (cJSON_IsArray(root)) {
        // 数组格式：取第一个元素
        cJSON* first_item = cJSON_GetArrayItem(root, 0);
        if (first_item) {
            part_number = cJSON_GetObjectItem(first_item, "part_number");
            ESP_LOGI(TAG, "find_handler: parsed array format, first_item valid, part_number=%s", 
                     part_number ? part_number->valuestring : "null");
        }
    } else if (cJSON_IsObject(root)) {
        // 对象格式：直接获取
        part_number = cJSON_GetObjectItem(root, "part_number");
        ESP_LOGI(TAG, "find_handler: parsed object format, part_number=%s, IsString=%d", 
                 part_number ? (part_number->valuestring ? part_number->valuestring : "null") : "null",
                 part_number ? cJSON_IsString(part_number) : -1);
    } else {
        ESP_LOGI(TAG, "find_handler: unknown JSON root type");
    }
    
    // 先提取字符串到本地变量，再释放JSON（避免use-after-free）
    std::string part_number_str;
    if (part_number && cJSON_IsString(part_number) && part_number->valuestring) {
        part_number_str = part_number->valuestring;
    }
    
    cJSON_Delete(root);

    if (part_number_str.empty()) {
        ESP_LOGE(TAG, "find_handler: part_number is invalid or empty!");
        const char* err_resp = "{\"success\":false,\"error\":\"Missing or empty part_number\"}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, err_resp, strlen(err_resp));
    }

    ESP_LOGI(TAG, "find_handler: extracted part_number=%s", part_number_str.c_str());

    // 回调处理（由component_sorter设置）
    std::string response;
    bool has_callback = (g_http_server && g_http_server->request_callback_);
    ESP_LOGI(TAG, "find_handler: has_callback=%d, g_http_server=%p", has_callback, g_http_server);
    if (has_callback) {
        bool success = g_http_server->request_callback_(
            "/api/find", "POST", buf, response
        );
        ESP_LOGI(TAG, "find_handler: callback returned, success=%d, response=%s", success, response.c_str());
        
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, response.c_str(), response.length());
    }

    // 默认响应
    ESP_LOGI(TAG, "find_handler: using default response");
    const char* default_resp = "{\"success\":true,\"message\":\"Component found\",\"led_index\":0}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, default_resp, strlen(default_resp));
}

esp_err_t ComponentHttpServer::components_handler(httpd_req_t *req) {
    SetCorsHeaders(req);
    std::string response;
    
    // 处理清空请求（POST /api/components/clear）
    if (req->method == HTTP_POST) {
        char buf[512];
        int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (ret > 0) {
            buf[ret] = '\0';
            // 调用 callback 处理清空请求
            if (g_http_server && g_http_server->request_callback_) {
                g_http_server->request_callback_("/api/components/clear", "POST", buf, response);
            }
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, response.c_str(), response.length());
        }
    }
    
    // 默认 GET 请求返回所有元器件
    if (g_http_server && g_http_server->request_callback_) {
        g_http_server->request_callback_("/api/components", "GET", "", response);
    } else {
        response = "[]";
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response.c_str(), response.length());
}

esp_err_t ComponentHttpServer::add_component_handler(httpd_req_t *req) {
    SetCorsHeaders(req);
    std::string body;
    if (!read_request_body(req, body)) {
        ESP_LOGE(TAG, "add_component_handler: failed to receive request body");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "add_component_handler received: [%s]", body.c_str());

    std::string response;
    if (g_http_server && g_http_server->request_callback_) {
        g_http_server->request_callback_("/api/components/add", "POST", body, response);
    } else {
        response = "{\"success\":false,\"error\":\"Server not ready\"}";
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response.c_str(), response.length());
}

esp_err_t ComponentHttpServer::options_handler(httpd_req_t *req) {
    return SendPreflightResponse(req);
}

const char* ComponentHttpServer::get_html_page() {
    return R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>灯带消消乐</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; -webkit-tap-highlight-color: transparent; touch-action: manipulation; }
        body { font-family: 'Segoe UI', Arial, sans-serif; background: #0a0a2e; color: white; min-height: 100vh; display: flex; flex-direction: column; align-items: center; padding: 16px; }
        h1 { font-size: 1.8em; margin-bottom: 12px; text-align: center; }
        .status-bar { display: flex; justify-content: space-around; width: 100%; max-width: 400px; margin-bottom: 16px; background: rgba(255,255,255,0.08); border-radius: 12px; padding: 12px; }
        .stat { text-align: center; }
        .stat-value { font-size: 1.6em; font-weight: bold; color: #ffd700; }
        .stat-label { font-size: 0.75em; color: rgba(255,255,255,0.6); }
        .grid-container { background: rgba(255,255,255,0.05); border-radius: 12px; padding: 10px; margin-bottom: 16px; width: min(96vw, 900px); }
        .grid { display: grid; grid-template-columns: repeat()HTML" STRINGIFY_MACRO(LED_STRIP_LENGTH) R"HTML(, minmax(5px, 1fr)); grid-template-rows: 1fr; gap: 2px; width: 100%; height: 28px; }
        .cell { border-radius: 999px; background: rgba(255,255,255,0.05); transition: background-color 0.15s; min-width: 0; min-height: 0; }
        .cell.enemy { box-shadow: inset 0 0 8px rgba(255,255,255,0.3); }
        .cell.projectile { box-shadow: 0 0 10px currentColor; }
        .cell.explosion { animation: explode 0.4s ease-out; }
        @keyframes explode { 0% { transform: scale(1.3); filter: brightness(2); } 100% { transform: scale(1); filter: brightness(1); } }
        .controls { display: flex; gap: 12px; margin-bottom: 16px; flex-wrap: wrap; justify-content: center; }
        .fire-btn { width: 72px; height: 72px; border-radius: 50%; border: 3px solid rgba(255,255,255,0.3); font-size: 1em; font-weight: bold; color: white; cursor: pointer; display: flex; align-items: center; justify-content: center; text-shadow: 0 1px 3px rgba(0,0,0,0.5); transition: transform 0.1s, box-shadow 0.1s; }
        .fire-btn:active { transform: scale(0.9); box-shadow: 0 0 20px currentColor; }
        .btn-red { background: radial-gradient(circle, #ff4444, #cc0000); }
        .btn-yellow { background: radial-gradient(circle, #ffdd44, #ccaa00); color: #333; }
        .btn-blue { background: radial-gradient(circle, #4488ff, #0044cc); }
        .btn-green { background: radial-gradient(circle, #44ff66, #00aa22); }
        .start-btn { padding: 14px 40px; border-radius: 30px; border: none; font-size: 1.2em; font-weight: bold; color: white; background: linear-gradient(135deg, #667eea, #764ba2); cursor: pointer; margin-top: 8px; }
        .start-btn:active { transform: scale(0.95); }
        .message { text-align: center; padding: 8px; color: rgba(255,255,255,0.7); font-size: 0.9em; min-height: 2em; }
        .combo-popup { position: fixed; top: 30%; left: 50%; transform: translate(-50%, -50%); font-size: 3em; font-weight: bold; color: #ffd700; text-shadow: 0 0 20px #ffd700; pointer-events: none; opacity: 0; transition: opacity 0.3s; }
        .combo-popup.show { opacity: 1; animation: comboAnim 0.8s ease-out; }
        @keyframes comboAnim { 0% { transform: translate(-50%, -50%) scale(0.5); } 50% { transform: translate(-50%, -50%) scale(1.3); } 100% { transform: translate(-50%, -50%) scale(1); } }
    </style>
</head>
<body>
    <h1>灯带消消乐</h1>
    <div class="status-bar">
        <div class="stat"><div class="stat-value" id="score">0</div><div class="stat-label">分数</div></div>
        <div class="stat"><div class="stat-value" id="level">1</div><div class="stat-label">等级</div></div>
        <div class="stat"><div class="stat-value" id="combo">0</div><div class="stat-label">连击</div></div>
        <div class="stat"><div class="stat-value" id="high">0</div><div class="stat-label">最高分</div></div>
    </div>
    <div class="grid-container">
        <div class="grid" id="grid"></div>
    </div>
    <div class="controls" id="controls">
        <button class="fire-btn btn-red" onclick="fire(0)">红</button>
        <button class="fire-btn btn-yellow" onclick="fire(1)">黄</button>
        <button class="fire-btn btn-blue" onclick="fire(2)">蓝</button>
        <button class="fire-btn btn-green" onclick="fire(3)">绿</button>
    </div>
    <button class="start-btn" id="startBtn" onclick="startGame()">开始游戏</button>
    <div class="message" id="msg">等待开始...</div>
    <div class="combo-popup" id="comboPopup"></div>

    <script>
        const COLORS = ['#ff3030','#ffe630','#3078ff','#30ff3c'];
        const ROWS = 1, COLS = )HTML" STRINGIFY_MACRO(LED_STRIP_LENGTH) R"HTML(;
        let ws = null, connected = false;

        // 初始化线性灯带视图：左侧是 LED0，右侧是最后一颗灯
        const gridEl = document.getElementById('grid');
        for (let i = 0; i < ROWS * COLS; i++) {
            const cell = document.createElement('div');
            cell.className = 'cell';
            gridEl.appendChild(cell);
        }

        function updateGrid(enemies, projectile, explosions) {
            const cells = gridEl.children;
            for (let i = 0; i < cells.length; i++) cells[i].style.backgroundColor = '';
            // 清除class
            for (const c of cells) { c.className = 'cell'; }

            if (enemies) {
                enemies.forEach(e => {
                    const idx = e.row * COLS + e.col;
                    if (idx >= 0 && idx < cells.length) {
                        cells[idx].style.backgroundColor = COLORS[e.color];
                        cells[idx].classList.add('enemy');
                    }
                });
            }
            if (projectile && projectile.active) {
                const idx = projectile.row * COLS + projectile.col;
                if (idx >= 0 && idx < cells.length) {
                    cells[idx].style.backgroundColor = COLORS[projectile.color];
                    cells[idx].classList.add('projectile');
                }
            }
            if (explosions) {
                explosions.forEach(e => {
                    const idx = e.row * COLS + e.col;
                    if (idx >= 0 && idx < cells.length) {
                        cells[idx].style.backgroundColor = COLORS[e.color];
                        cells[idx].classList.add('explosion');
                    }
                });
            }
        }

        function fire(color) {
            fetch('/api/fire', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({color: color})
            });
        }

        function startGame() {
            fetch('/api/start', { method: 'POST' });
        }

        function showCombo(n) {
            if (n < 2) return;
            const el = document.getElementById('comboPopup');
            el.textContent = n + 'x COMBO!';
            el.classList.remove('show');
            void el.offsetWidth;
            el.classList.add('show');
            setTimeout(() => el.classList.remove('show'), 800);
        }

        function connectWS() {
            const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
            ws = new WebSocket(proto + '//' + location.hostname + ':32324/ws');
            ws.onopen = () => { connected = true; document.getElementById('msg').textContent = '已连接'; };
            ws.onclose = () => { connected = false; document.getElementById('msg').textContent = '连接断开，重连中...'; setTimeout(connectWS, 2000); };
            ws.onmessage = (e) => {
                try {
                    const d = JSON.parse(e.data);
                    document.getElementById('score').textContent = d.score || 0;
                    document.getElementById('level').textContent = d.level || 1;
                    document.getElementById('combo').textContent = d.combo || 0;
                    document.getElementById('high').textContent = d.high_score || 0;

                    if (d.state === 'playing') {
                        document.getElementById('startBtn').style.display = 'none';
                        document.getElementById('msg').textContent = '按同色按钮，发射同色灯珠去碰撞消除！';
                    } else if (d.state === 'game_over') {
                        document.getElementById('startBtn').style.display = '';
                        document.getElementById('startBtn').textContent = '再来一局';
                        document.getElementById('msg').textContent = '游戏结束！得分: ' + d.score;
                    } else {
                        document.getElementById('startBtn').style.display = '';
                        document.getElementById('msg').textContent = '按任意按钮或点击开始';
                    }

                    updateGrid(d.grid, d.projectile, d.explosions);
                    showCombo(d.combo || 0);
                } catch(ex) {}
            };
        }
        connectWS();
    </script>
</body>
</html>)HTML";
}

// C接口实现
extern "C" {

static httpd_handle_t g_httpd_handle = nullptr;

esp_err_t component_http_server_start(uint16_t port) {
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

    ESP_LOGI(TAG, "HTTP server started on port %d", port);
    return ESP_OK;
}

void component_http_server_stop() {
    if (g_httpd_handle) {
        httpd_stop(g_httpd_handle);
        g_httpd_handle = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

} // extern "C"
