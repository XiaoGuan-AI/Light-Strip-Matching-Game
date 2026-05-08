/**
 * @file component_http_server.h
 * @brief HTTP服务器 - 网页管理界面
 */
#ifndef _COMPONENT_HTTP_SERVER_H_
#define _COMPONENT_HTTP_SERVER_H_

#include <string>
#include <functional>
#include <esp_http_server.h>

// 回调函数类型
using HttpRequestCallback = std::function<bool(const std::string& uri, const std::string& method, 
                                                 const std::string& body, std::string& response)>;

class ComponentHttpServer {
public:
    ComponentHttpServer();
    ~ComponentHttpServer();

    // 初始化HTTP服务器 (默认端口80)
    bool Initialize(uint16_t port = 80);

    // 启动服务器
    bool Start();

    // 停止服务器
    void Stop();

    // 设置请求回调
    void SetRequestCallback(HttpRequestCallback callback);

    // 获取服务器状态
    bool IsRunning() const { return server_running_; }

private:
    uint16_t port_;
    httpd_handle_t server_;
    bool server_running_;
    HttpRequestCallback request_callback_;

    // HTTP请求处理函数
    static esp_err_t root_handler(httpd_req_t *req);
    static esp_err_t api_handler(httpd_req_t *req);
    static esp_err_t find_handler(httpd_req_t *req);
    static esp_err_t components_handler(httpd_req_t *req);
    static esp_err_t add_component_handler(httpd_req_t *req);
    static esp_err_t options_handler(httpd_req_t *req);
    static esp_err_t jsqr_handler(httpd_req_t *req);
    static bool read_request_body(httpd_req_t *req, std::string& body);

    // HTML页面内容
    static const char* get_html_page();
    
    // 注册URI处理程序
    bool register_uri_handlers();
};

// C接口用于ESP-IDF
extern "C" {
    esp_err_t component_http_server_start(uint16_t port);
    void component_http_server_stop();
}

#endif // _COMPONENT_HTTP_SERVER_H_
