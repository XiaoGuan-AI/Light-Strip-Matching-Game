/**
 * @file component_sorter.cc
 * @brief 元器件分拣器主逻辑实现
 */
#include "component_sorter.h"
#include "component_color_palette.h"
#include <esp_log.h>
#include <cJSON.h>
#include <cctype>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <vector>

#define TAG "ComponentSorter"

namespace {

const char* GetDisplayColorName(uint8_t r, uint8_t g, uint8_t b) {
    if (r >= 200 && g < 100 && b < 100) return "红色";
    if (g >= 200 && r < 100 && b < 100) return "绿色";
    if (b >= 200 && r < 100 && g < 100) return "蓝色";
    if (r >= 200 && g >= 200 && b < 100) return "黄色";
    if (r >= 200 && g < 100 && b >= 200) return "紫色";
    if (g >= 200 && b >= 200 && r < 100) return "青色";
    if (r >= 200 && g >= 100 && g < 200 && b < 100) return "橙色";
    if (r >= 200 && g >= 200 && b >= 200) return "白色";
    if (r >= 200 && g >= 150 && g < 220 && b >= 150 && b < 250) return "粉色";
    return "彩色";
}

struct AddComponentResult {
    bool success = false;
    bool duplicate = false;
    std::string part_number;
    uint16_t logical_led_index = 0;
    uint8_t actual_led_index = 0;
    uint8_t group = 0;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    std::string message;
    std::string error;
};

std::string TrimWhitespace(const std::string& value) {
    size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }

    size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string ToUpperAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool IsPartNumberChar(char ch) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) || ch == '_' || ch == '-' || ch == '.';
}

std::string ExtractFieldValue(const std::string& upper_text, size_t value_pos) {
    while (value_pos < upper_text.size() &&
           (upper_text[value_pos] == ' ' || upper_text[value_pos] == '"' ||
            upper_text[value_pos] == '\'')) {
        ++value_pos;
    }

    const size_t start = value_pos;
    while (value_pos < upper_text.size() && IsPartNumberChar(upper_text[value_pos])) {
        ++value_pos;
    }

    if (value_pos == start) {
        return "";
    }

    return upper_text.substr(start, value_pos - start);
}

std::string ResolvePartNumber(const std::string& part_number, const std::string& scan_text) {
    const std::string direct_part_number = ToUpperAscii(TrimWhitespace(part_number));
    if (!direct_part_number.empty()) {
        return direct_part_number;
    }

    const std::string trimmed_scan_text = TrimWhitespace(scan_text);
    if (trimmed_scan_text.empty()) {
        return "";
    }

    const std::string upper_scan_text = ToUpperAscii(trimmed_scan_text);
    static const char* kMarkers[] = {
        "PC:", "PC=",
        "PART_NUMBER:", "PART_NUMBER=",
        "PART-NUMBER:", "PART-NUMBER=",
        "PN:", "PN="
    };

    for (const char* marker : kMarkers) {
        const size_t pos = upper_scan_text.find(marker);
        if (pos == std::string::npos) {
            continue;
        }

        const std::string extracted = ExtractFieldValue(upper_scan_text, pos + strlen(marker));
        if (!extracted.empty()) {
            return extracted;
        }
    }

    bool all_part_number_chars = true;
    for (char ch : upper_scan_text) {
        if (!IsPartNumberChar(ch)) {
            all_part_number_chars = false;
            break;
        }
    }

    return all_part_number_chars ? upper_scan_text : "";
}

std::string JsonToString(cJSON* root) {
    char* json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        cJSON_Delete(root);
        return "{\"success\":false,\"error\":\"JSON error\"}";
    }

    std::string result(json_str);
    free(json_str);
    cJSON_Delete(root);
    return result;
}

cJSON* BuildAddResultJsonObject(const AddComponentResult& result) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddBoolToObject(item, "success", result.success);
    cJSON_AddBoolToObject(item, "duplicate", result.duplicate);
    cJSON_AddStringToObject(item, "part_number", result.part_number.c_str());

    if (result.success || result.duplicate) {
        cJSON_AddNumberToObject(item, "led_index", result.logical_led_index);
        cJSON_AddNumberToObject(item, "actual_led_index", result.actual_led_index);
        cJSON_AddNumberToObject(item, "group", result.group);
        cJSON_AddNumberToObject(item, "r", result.r);
        cJSON_AddNumberToObject(item, "g", result.g);
        cJSON_AddNumberToObject(item, "b", result.b);
    }

    if (!result.message.empty()) {
        cJSON_AddStringToObject(item, "message", result.message.c_str());
    }

    if (!result.error.empty()) {
        cJSON_AddStringToObject(item, "error", result.error.c_str());
    }

    return item;
}

}  // namespace

// 全局实例指针（用于回调）
static ComponentSorter* g_sorter = nullptr;

ComponentSorter::ComponentSorter() 
    : initialized_(false), running_(false), led_update_task_handle_(nullptr) {
    g_sorter = this;
}

ComponentSorter::~ComponentSorter() {
    Stop();
    g_sorter = nullptr;
}

bool ComponentSorter::Initialize() {
    if (initialized_) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }

    // 1. 初始化LED控制
    if (!led_control_.Initialize()) {
        ESP_LOGE(TAG, "LED control initialization failed");
        return false;
    }

    // 2. 初始化 MAX98357A I2S 提示音
    if (!buzzer_.Initialize()) {
        ESP_LOGE(TAG, "Speaker initialization failed");
        // 声音提示失败不致命，继续
    }

    // 3. 初始化数据库
    if (!database_.Initialize()) {
        ESP_LOGE(TAG, "Database initialization failed");
        return false;
    }

    // 4. 初始化HTTP服务器
    http_server_.Initialize(HTTP_SERVER_PORT);
    http_server_.SetRequestCallback([this](const std::string& uri, const std::string& method,
                                          const std::string& body, std::string& response) {
        return HandleHttpRequest(uri, method, body, response);
    });

    // 5. 初始化WebSocket服务器
    ws_server_.Initialize(WS_SERVER_PORT);
    ws_server_.SetMessageCallback([this](const WsMessage& msg) {
        HandleWsMessage(msg);
    });

    initialized_ = true;
    ESP_LOGI(TAG, "Component sorter initialized successfully");
    return true;
}

bool ComponentSorter::Start() {
    if (!initialized_) {
        ESP_LOGE(TAG, "Not initialized, call Initialize() first");
        return false;
    }

    if (running_) {
        ESP_LOGW(TAG, "Already running");
        return true;
    }

    // 启动LED更新任务（每秒更新闪烁状态）
    xTaskCreatePinnedToCore(
        [](void* param) {
            ((ComponentSorter*)param)->LedUpdateTask(param);
        },
        "led_update", 4096, this, 3, &led_update_task_handle_, 0
    );

    // 启动HTTP服务器
    if (!http_server_.Start()) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return false;
    }

    // 启动WebSocket服务器
    if (!ws_server_.Start()) {
        ESP_LOGE(TAG, "WebSocket server start failed");
        http_server_.Stop();
        return false;
    }

    running_ = true;
    ESP_LOGI(TAG, "Component sorter started");
    
    // 显示开机倒计时灯效
    led_control_.RunStartupCountdown();
    
    return true;
}

void ComponentSorter::Stop() {
    if (!running_) {
        return;
    }

    // 停止LED更新任务
    if (led_update_task_handle_) {
        vTaskDelete(led_update_task_handle_);
        led_update_task_handle_ = nullptr;
    }

    http_server_.Stop();
    ws_server_.Stop();
    led_control_.StopAllBlink();
    
    running_ = false;
    ESP_LOGI(TAG, "Component sorter stopped");
}

bool ComponentSorter::FindAndHighlight(const std::string& part_number) {
    if (!running_) {
        ESP_LOGW(TAG, "Sorter not running");
        return false;
    }

    uint8_t led_index, r, g, b, group;
    
    // 查找元器件
    bool found = database_.FindComponent(part_number, led_index, r, g, b, group);
    
    if (found) {
        ESP_LOGI(TAG, "Found component: %s -> LED%d (Group %d), Color(%d,%d,%d)",
                 part_number.c_str(), led_index, group, r, g, b);

        // 高亮LED
        led_control_.HighlightLed(led_index, r, g, b, LED_BLINK_COUNT, group);

        // 喇叭提示音
        if (group > 1) {
            // 溢出组 - 按组别次数beep
            buzzer_.OverflowBeep(group);
        } else {
            // 正常 - 成功音效
            buzzer_.SuccessBeep();
        }

        // 广播到WebSocket客户端
        std::string msg = "{\"type\":\"found\",\"part_number\":\"" + part_number + 
                         "\",\"led_index\":" + std::to_string(led_index) +
                         ",\"group\":" + std::to_string(group) + "}";
        Broadcast(msg);
    } else {
        ESP_LOGW(TAG, "Component not found: %s", part_number.c_str());
        buzzer_.ErrorBeep();

        // 广播未找到消息
        std::string msg = "{\"type\":\"not_found\",\"part_number\":\"" + part_number + "\"}";
        Broadcast(msg);
    }

    return found;
}

void ComponentSorter::Broadcast(const std::string& message) {
    if (running_) {
        ws_server_.BroadcastMessage(message);
    }
}

std::string ComponentSorter::GetStatusJson() const {
    cJSON* root = cJSON_CreateObject();
    
    cJSON_AddStringToObject(root, "status", running_ ? "running" : "stopped");
    cJSON_AddNumberToObject(root, "component_count", database_.GetCount());
    cJSON_AddNumberToObject(root, "led_count", LED_STRIP_LENGTH);
    cJSON_AddBoolToObject(root, "http_server", http_server_.IsRunning());
    cJSON_AddBoolToObject(root, "ws_server", ws_server_.IsRunning());
    cJSON_AddNumberToObject(root, "ws_clients", ws_server_.GetConnectedClients());

    char* json_str = cJSON_Print(root);
    std::string result(json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    return result;
}

bool ComponentSorter::HandleHttpRequest(const std::string& uri, const std::string& method,
                                       const std::string& body, std::string& response) {
    ESP_LOGI(TAG, "HTTP request: %s %s, body: [%s]", method.c_str(), uri.c_str(), body.c_str());

    auto add_component = [&](const std::string& requested_part_number,
                             const std::string& scan_text) -> AddComponentResult {
        AddComponentResult result;
        result.part_number = ResolvePartNumber(requested_part_number, scan_text);

        if (result.part_number.empty()) {
            result.error = "Missing or invalid part_number";
            return result;
        }

        uint8_t existing_led = 0;
        uint8_t existing_r = 0;
        uint8_t existing_g = 0;
        uint8_t existing_b = 0;
        uint8_t existing_group = 0;
        if (database_.FindComponent(result.part_number, existing_led, existing_r, existing_g, existing_b, existing_group)) {
            result.duplicate = true;
            result.logical_led_index = existing_led;
            result.actual_led_index = existing_led;
            result.group = existing_group;
            result.r = existing_r;
            result.g = existing_g;
            result.b = existing_b;
            result.message = "编号已存在";
            result.error = "编号已存在，请勿重复添加";
            led_control_.StartAsyncBlink(existing_led, existing_r, existing_g, existing_b, 1500);
            return result;
        }

        const uint16_t logical_led_index = database_.FindFreeLed(MAX_COMPONENTS);
        if (logical_led_index >= MAX_COMPONENTS) {
            ESP_LOGE(TAG, "No free logical slot available for %s", result.part_number.c_str());
            result.error = "LED已用完，请删除部分元器件后再添加";
            return result;
        }

        result.logical_led_index = logical_led_index;
        result.actual_led_index = logical_led_index % LED_STRIP_LENGTH;
        result.group = static_cast<uint8_t>(logical_led_index / LED_STRIP_LENGTH) + 1;

        const ComponentPaletteColor assigned_color = GetPaletteColor(logical_led_index);
        result.r = assigned_color.r;
        result.g = assigned_color.g;
        result.b = assigned_color.b;

        Component comp(result.part_number, logical_led_index, result.r, result.g, result.b);
        if (!database_.AddComponent(comp)) {
            result.error = "Failed to add component";
            return result;
        }

        ESP_LOGI(TAG, "Component added: %s -> slot %u, actual LED %u, group %u",
                 result.part_number.c_str(),
                 logical_led_index,
                 result.actual_led_index,
                 result.group);

        for (int i = 0; i < 3; i++) {
            led_control_.StartAsyncBlink(result.actual_led_index, result.r, result.g, result.b, 300);
            vTaskDelay(pdMS_TO_TICKS(400));
        }

        result.success = true;
        result.message = "Added successfully";
        return result;
    };

    // /api/find - 查找元器件
    if (uri == "/api/find" && method == "POST") {
        // 解析JSON
        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) {
            ESP_LOGE(TAG, "Invalid JSON in /api/find: %s", body.c_str());
            response = "{\"success\":false,\"error\":\"Invalid JSON\"}";
            return false;
        }

        cJSON* part_number = nullptr;
        cJSON* user_id_json = nullptr;
        
        // 支持两种JSON格式：
        // 1. 对象格式: {"part_number": "C00001", "user_id": 1}
        // 2. 数组格式: [{"part_number": "C00001", "user_id": 1}]
        if (cJSON_IsArray(root)) {
            cJSON* first_item = cJSON_GetArrayItem(root, 0);
            if (first_item) {
                part_number = cJSON_GetObjectItem(first_item, "part_number");
                user_id_json = cJSON_GetObjectItem(first_item, "user_id");
            }
            ESP_LOGI(TAG, "HandleHttpRequest: parsed array format, part_number=%s", 
                     part_number ? (part_number->valuestring ? part_number->valuestring : "null") : "null");
        } else if (cJSON_IsObject(root)) {
            part_number = cJSON_GetObjectItem(root, "part_number");
            user_id_json = cJSON_GetObjectItem(root, "user_id");
            ESP_LOGI(TAG, "HandleHttpRequest: parsed object format, part_number=%s", 
                     part_number ? (part_number->valuestring ? part_number->valuestring : "null") : "null");
        }
        
        // 先提取字符串到本地变量，再释放JSON（避免use-after-free）
        std::string pn;
        if (part_number && cJSON_IsString(part_number) && part_number->valuestring) {
            pn = part_number->valuestring;
        }
        
        // 转换为大写，不区分大小写
        for (auto& c : pn) c = toupper(c);
        
        // 提取user_id，如果没有或为0则使用元器件原始颜色
        uint8_t user_id = 0;
        if (user_id_json && cJSON_IsNumber(user_id_json)) {
            user_id = user_id_json->valueint;
            ESP_LOGI(TAG, "Received user_id=%d from request", user_id);
        } else {
            // 没有提供user_id，使用元器件原始颜色
            ESP_LOGI(TAG, "No user_id provided, will use component original color");
        }
        
        cJSON_Delete(root);

        if (pn.empty()) {
            ESP_LOGE(TAG, "HandleHttpRequest: part_number is invalid or empty!");
            response = "{\"success\":false,\"error\":\"Missing or empty part_number\"}";
            return false;
        }

        ESP_LOGI(TAG, "Searching for component: [%s], user_id=%d", pn.c_str(), user_id);

        uint8_t led_index, r, g, b, group;
        
        bool found = database_.FindComponent(pn, led_index, r, g, b, group);
        
        if (found) {
            // 如果指定了user_id > 0，使用用户颜色；否则使用元器件原始颜色
            uint8_t display_r, display_g, display_b;
            bool use_user_color = (user_id > 0);
            const char* color_name = nullptr;
            
            if (use_user_color) {
                // 获取用户颜色
                UserColor user_color = GetUserColor(user_id);
                display_r = user_color.r;
                display_g = user_color.g;
                display_b = user_color.b;
                color_name = GetDisplayColorName(display_r, display_g, display_b);
                ESP_LOGI(TAG, "Using user color: (%d,%d,%d) for user_id=%d", 
                         display_r, display_g, display_b, user_id);
            } else {
                // 使用元器件原始颜色
                display_r = r;
                display_g = g;
                display_b = b;
                color_name = GetDisplayColorName(display_r, display_g, display_b);
                ESP_LOGI(TAG, "Using component original color: (%d,%d,%d)", 
                         display_r, display_g, display_b);
            }
            
            // 启动异步闪烁（3分钟）
            led_control_.StartAsyncBlink(led_index, display_r, display_g, display_b, LED_HOLD_MS);
            
            // 喇叭提示音
            if (group > 1) {
                buzzer_.OverflowBeep(group);
            } else {
                buzzer_.SuccessBeep();
            }
            
            // 记录活跃搜索
            active_searches_[pn] = user_id;
            const uint32_t hold_seconds = LED_HOLD_MS / 1000;
            
            // 广播到WebSocket客户端
            std::string msg = "{\"type\":\"found\",\"part_number\":\"" + pn + 
                             "\",\"led_index\":" + std::to_string(led_index) +
                             ",\"group\":" + std::to_string(group) +
                             ",\"user_id\":" + std::to_string(user_id) +
                             ",\"use_user_color\":" + (use_user_color ? "true" : "false") +
                             ",\"r\":" + std::to_string(display_r) +
                             ",\"g\":" + std::to_string(display_g) +
                             ",\"b\":" + std::to_string(display_b) +
                             ",\"color_name\":\"" + std::string(color_name) + "\"" +
                             ",\"hold_ms\":" + std::to_string(LED_HOLD_MS) +
                             ",\"hold_seconds\":" + std::to_string(hold_seconds) +
                             ",\"led_color\":{\"r\":" + std::to_string(display_r) +
                             ",\"g\":" + std::to_string(display_g) +
                             ",\"b\":" + std::to_string(display_b) + "}}";
            Broadcast(msg);
            
            response = BuildFindResponseJson(true, pn, led_index, group, display_r, display_g, display_b, "", user_id);
        } else {
            response = BuildFindResponseJson(false, pn, 0, 0, 0, 0, 0, "Component not found");
        }
        
        return true;
    }

    // /api/components - 获取所有元器件
    if (uri == "/api/components" && method == "GET") {
        response = database_.GetAllComponentsJson();
        return true;
    }

    // /api/components/backup - 备份/导出元器件数据
    if (uri == "/api/components/backup" && method == "GET") {
        std::string json = database_.ExportToJson();
        response = "{\"success\":true,\"data\":" + json + "}";
        return true;
    }

    // /api/components/restore - 从备份恢复元器件数据
    if (uri == "/api/components/restore" && method == "POST") {
        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) {
            response = "{\"success\":false,\"error\":\"Invalid JSON\"}";
            return false;
        }

        cJSON* data = cJSON_GetObjectItem(root, "data");
        if (!data || !cJSON_IsString(data) || !data->valuestring) {
            cJSON_Delete(root);
            response = "{\"success\":false,\"error\":\"Missing or invalid data\"}";
            return false;
        }

        std::string json_data = data->valuestring;
        cJSON_Delete(root);

        bool success = database_.ImportFromJson(json_data);
        if (success) {
            database_.MigrateColorsToPurePalette();
            database_.SaveToFile();
            ESP_LOGI(TAG, "Components restored successfully, count: %zu", database_.GetCount());
            response = "{\"success\":true,\"count\":" + std::to_string(database_.GetCount()) + ",\"message\":\"Restore successful\"}";
        } else {
            response = "{\"success\":false,\"error\":\"Invalid backup data format\"}";
        }
        return true;
    }

    // /api/components/add - 添加元器件（扫码录入）
    if (uri == "/api/components/add" && method == "POST") {
        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) {
            response = "{\"success\":false,\"error\":\"Invalid JSON\"}";
            return false;
        }

        cJSON* part_number = cJSON_GetObjectItem(root, "part_number");
        cJSON* scan_text = cJSON_GetObjectItem(root, "scan_text");
        const std::string requested_part_number =
            (part_number && cJSON_IsString(part_number) && part_number->valuestring)
                ? part_number->valuestring
                : "";
        const std::string raw_scan_text =
            (scan_text && cJSON_IsString(scan_text) && scan_text->valuestring)
                ? scan_text->valuestring
                : "";
        cJSON_Delete(root);

        const AddComponentResult result = add_component(requested_part_number, raw_scan_text);
        response = JsonToString(BuildAddResultJsonObject(result));
        return result.success;
    }

    // /api/components/batch_add - 兼容旧调用的批量添加接口，当前 Web UI 默认逐条实时录入
    if (uri == "/api/components/batch_add" && method == "POST") {
        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) {
            response = "{\"success\":false,\"error\":\"Invalid JSON\"}";
            return false;
        }

        cJSON* items = cJSON_GetObjectItem(root, "items");
        if (!items || !cJSON_IsArray(items)) {
            cJSON_Delete(root);
            response = "{\"success\":false,\"error\":\"Missing or invalid items\"}";
            return false;
        }

        cJSON* resp_root = cJSON_CreateObject();
        cJSON* results = cJSON_CreateArray();
        int total = 0;
        int added_count = 0;
        int duplicate_count = 0;
        int error_count = 0;

        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, items) {
            ++total;

            std::string requested_part_number;
            std::string raw_scan_text;
            if (cJSON_IsObject(item)) {
                cJSON* part_number = cJSON_GetObjectItem(item, "part_number");
                cJSON* scan_text = cJSON_GetObjectItem(item, "scan_text");
                if (part_number && cJSON_IsString(part_number) && part_number->valuestring) {
                    requested_part_number = part_number->valuestring;
                }
                if (scan_text && cJSON_IsString(scan_text) && scan_text->valuestring) {
                    raw_scan_text = scan_text->valuestring;
                }
            }

            const AddComponentResult result = add_component(requested_part_number, raw_scan_text);
            if (result.success) {
                ++added_count;
            } else if (result.duplicate) {
                ++duplicate_count;
            } else {
                ++error_count;
            }

            cJSON_AddItemToArray(results, BuildAddResultJsonObject(result));
        }

        cJSON_Delete(root);

        cJSON_AddBoolToObject(resp_root, "success", true);
        cJSON_AddNumberToObject(resp_root, "total", total);
        cJSON_AddNumberToObject(resp_root, "added_count", added_count);
        cJSON_AddNumberToObject(resp_root, "duplicate_count", duplicate_count);
        cJSON_AddNumberToObject(resp_root, "error_count", error_count);
        cJSON_AddItemToObject(resp_root, "results", results);
        response = JsonToString(resp_root);
        return error_count == 0;
    }

    // /api/components/clear - 清空所有元器件（恢复出厂设置）
    if (uri == "/api/components/clear" && method == "POST") {
        ESP_LOGI(TAG, "Clearing all components...");
        database_.Clear();
        response = "{\"success\":true,\"message\":\"All components cleared\"}";
        return true;
    }

    // /api/components/delete - 删除元器件
    if (uri == "/api/components/delete" && method == "POST") {
        cJSON *root = cJSON_Parse(body.c_str());
        if (!root) {
            response = "{\"success\":false,\"error\":\"Invalid JSON\"}";
            return false;
        }
        
        cJSON* part_number = cJSON_GetObjectItem(root, "part_number");
        if (!part_number || !cJSON_IsString(part_number)) {
            cJSON_Delete(root);
            response = "{\"success\":false,\"error\":\"Missing part_number\"}";
            return false;
        }
        
        std::string pn = part_number->valuestring;
        for (auto& c : pn) c = toupper(c);
        cJSON_Delete(root);
        
        if (database_.RemoveComponent(pn)) {
            ESP_LOGI(TAG, "Deleted component: %s", pn.c_str());
            response = "{\"success\":true,\"message\":\"Component deleted\"}";
        } else {
            response = "{\"success\":false,\"error\":\"Component not found\"}";
        }
        return true;
    }

    // /api/components/update - 更新元器件信息
    if (uri == "/api/components/update" && method == "POST") {
        cJSON *root = cJSON_Parse(body.c_str());
        if (!root) {
            response = "{\"success\":false,\"error\":\"Invalid JSON\"}";
            return false;
        }
        
        cJSON* part_number = cJSON_GetObjectItem(root, "part_number");
        cJSON* new_led_index = cJSON_GetObjectItem(root, "led_index");
        
        if (!part_number || !cJSON_IsString(part_number) || !new_led_index) {
            cJSON_Delete(root);
            response = "{\"success\":false,\"error\":\"Missing parameters\"}";
            return false;
        }
        
        std::string pn = part_number->valuestring;
        for (auto& c : pn) c = toupper(c);
        const int led_idx = new_led_index->valueint;
        if (led_idx < 0 || led_idx >= MAX_COMPONENTS) {
            cJSON_Delete(root);
            response = "{\"success\":false,\"error\":\"LED index out of range\"}";
            return false;
        }
        
        // 查找并更新元器件
        for (auto& comp : database_.GetComponentsRef()) {
            if (comp.part_number == pn) {
                comp.led_index = static_cast<uint16_t>(led_idx);
                // 保存更新
                database_.SaveToFile();
                ESP_LOGI(TAG, "Updated component %s to LED %d", pn.c_str(), led_idx);
                response = "{\"success\":true,\"message\":\"Component updated\"}";
                cJSON_Delete(root);
                return true;
            }
        }
        
        cJSON_Delete(root);
        response = "{\"success\":false,\"error\":\"Component not found\"}";
        return true;
    }

    // /api/status - 获取状态
    if (uri == "/api/status" && method == "GET") {
        response = GetStatusJson();
        return true;
    }

    response = "{\"success\":false,\"error\":\"Unknown endpoint\"}";
    return false;
}

void ComponentSorter::HandleWsMessage(const WsMessage& msg) {
    ESP_LOGI(TAG, "WebSocket message: type=%d", (int)msg.type);

    switch (msg.type) {
        case WsMessageType::FIND_COMPONENT: {
            // 解析并查找
            cJSON* root = cJSON_Parse(msg.data.c_str());
            if (root) {
                cJSON* part_number = cJSON_GetObjectItem(root, "part_number");
                if (part_number && cJSON_IsString(part_number)) {
                    FindAndHighlight(part_number->valuestring);
                }
                cJSON_Delete(root);
            }
            break;
        }

        case WsMessageType::GET_ALL: {
            // 发送所有元器件列表
            std::string json = database_.GetAllComponentsJson();
            ws_server_.BroadcastMessage(json);
            break;
        }

        case WsMessageType::RESET_DB: {
            database_.ResetToDefault();
            ws_server_.BroadcastMessage("{\"type\":\"reset\",\"success\":true}");
            break;
        }

        default:
            break;
    }
}

std::string ComponentSorter::BuildFindResponseJson(bool success, const std::string& part_number,
                                                   uint8_t led_index, uint8_t group,
                                                   uint8_t r, uint8_t g, uint8_t b,
                                                   const std::string& error, uint8_t user_id) {
    cJSON* root = cJSON_CreateObject();
    
    cJSON_AddBoolToObject(root, "success", success);
    cJSON_AddStringToObject(root, "part_number", part_number.c_str());
    
    if (success) {
        cJSON_AddNumberToObject(root, "led_index", led_index);
        cJSON_AddNumberToObject(root, "group", group);
        cJSON_AddNumberToObject(root, "r", r);
        cJSON_AddNumberToObject(root, "g", g);
        cJSON_AddNumberToObject(root, "b", b);
        cJSON_AddNumberToObject(root, "user_id", user_id);
        
        // 生成颜色名称（使用元器件原始颜色）
        const uint32_t hold_seconds = LED_HOLD_MS / 1000;
        cJSON_AddStringToObject(root, "color_name", GetDisplayColorName(r, g, b));
        cJSON_AddNumberToObject(root, "hold_ms", LED_HOLD_MS);
        cJSON_AddNumberToObject(root, "hold_seconds", static_cast<double>(hold_seconds));
        cJSON_AddStringToObject(root, "message", "Component found");
    } else {
        cJSON_AddStringToObject(root, "error", error.empty() ? "Not found" : error.c_str());
    }

    char* json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        return "{\"success\":false,\"error\":\"JSON error\"}";
    }
    std::string result(json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    return result;
}

void ComponentSorter::LedUpdateTask(void* param) {
    ComponentSorter* self = (ComponentSorter*)param;
    ESP_LOGI(TAG, "LED update task started");
    
    while (true) {
        // 每50ms更新一次LED状态
        self->led_control_.Update();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

UserColor ComponentSorter::GetUserColor(uint8_t user_id) {
    // 如果用户已存在，返回其颜色
    auto it = user_colors_.find(user_id);
    if (it != user_colors_.end()) {
        return it->second;
    }
    
    // 分配新颜色
    UserColor uc;
    uc.user_id = user_id;
    const size_t palette_index = (user_id == 0) ? 0 : (user_id - 1);
    const ComponentPaletteColor preset = GetPaletteColor(palette_index);
    uc.r = preset.r;
    uc.g = preset.g;
    uc.b = preset.b;
    uc.name = std::string("用户") + std::to_string(user_id) + "-" + preset.name;
    
    user_colors_[user_id] = uc;
    return uc;
}

void ComponentSorter::ClearExpiredSearches() {
    // 清除超过10秒的搜索记录
    // 这里简化处理，实际可以用时间戳来管理
    if (active_searches_.size() > 10) {
        // 保留最近的搜索
        auto it = active_searches_.begin();
        std::advance(it, active_searches_.size() - 5);
        active_searches_.erase(active_searches_.begin(), it);
    }
}
