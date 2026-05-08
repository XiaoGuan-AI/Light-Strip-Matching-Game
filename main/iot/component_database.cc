/**
 * @file component_database.cc
 * @brief 元器件数据库实现
 */
#include "component_database.h"
#include "component_color_palette.h"
#include <esp_log.h>
#include <cJSON.h>
#include <cstring>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <esp_spiffs.h>
#include <inttypes.h>
#include <map>

#define TAG "ComponentDatabase"

// ============================================================================
// 预留：随机元器件编号前缀
// ============================================================================
// static const char* PART_PREFIXES[] = {"C", "R", "L", "U", "D", "Q", "J", "K", "X", "Y"};

// 生成随机RGB颜色（预留）
// static void GenerateRandomColor(uint8_t& r, uint8_t& g, uint8_t& b) {
//     uint8_t hue = esp_random() % 360;
//     uint8_t s = 200 + (esp_random() % 56);
//     uint8_t v = 200 + (esp_random() % 56);
//     // ... HSV转RGB 逻辑
// }

ComponentDatabase::ComponentDatabase() : initialized_(false) {
}

ComponentDatabase::~ComponentDatabase() {
}

bool ComponentDatabase::Initialize() {
    // 初始化SPIFFS
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s, using memory only", esp_err_to_name(ret));
        LoadDefaultComponents();
        initialized_ = true;
        return true;
    }

    ESP_LOGI(TAG, "SPIFFS mounted successfully");

    // 先标记为已初始化
    initialized_ = true;
    
    // 尝试从文件加载
    if (LoadFromFile()) {
        if (MigrateColorsToPurePalette()) {
            SaveToFile();
        }
        ESP_LOGI(TAG, "Loaded %zu components from file", components_.size());
    } else {
        // 文件不存在或加载失败，加载默认数据
        ESP_LOGI(TAG, "No file or load failed, loading defaults");
        LoadDefaultComponents();
        SaveToFile();
    }

    ESP_LOGI(TAG, "Database initialized with %zu components", components_.size());
    return true;
}

bool ComponentDatabase::FindComponent(const std::string& part_number, 
                                     uint8_t& out_led_index,
                                     uint8_t& out_r, uint8_t& out_g, uint8_t& out_b,
                                     uint8_t& out_group) {
    // 转换为大写进行不区分大小写的查找
    std::string upper_pn = part_number;
    for (auto& c : upper_pn) c = toupper(c);
    
    // 查找元器件
    ESP_LOGI(TAG, "FindComponent: looking for [%s] (uppercase: [%s]), database has %zu components", 
             part_number.c_str(), upper_pn.c_str(), components_.size());
    for (const auto& comp : components_) {
        std::string existing_upper = comp.part_number;
        for (auto& c : existing_upper) c = toupper(c);
        
        ESP_LOGI(TAG, "  comparing with [%s]", existing_upper.c_str());
        if (existing_upper == upper_pn) {
            // 计算分组和实际LED索引
            out_group = comp.led_index / LED_STRIP_LENGTH + 1;
            out_led_index = comp.led_index % LED_STRIP_LENGTH;
            out_r = comp.r;
            out_g = comp.g;
            out_b = comp.b;
            ESP_LOGI(TAG, "Found: %s -> LED%d (Group %d), Color(%d,%d,%d)", 
                     part_number.c_str(), out_led_index, out_group, out_r, out_g, out_b);
            return true;
        }
    }
    ESP_LOGW(TAG, "Not found: %s", part_number.c_str());
    return false;
}

bool ComponentDatabase::AddComponent(const Component& comp) {
    // 将元器件编号转换为大写（统一存储格式）
    std::string upper_pn = comp.part_number;
    for (auto& c : upper_pn) c = toupper(c);
    
    // 检查是否已存在（不区分大小写比较）
    for (auto& existing : components_) {
        std::string existing_upper = existing.part_number;
        for (auto& c : existing_upper) c = toupper(c);
        
        if (existing_upper == upper_pn) {
            // 已存在，返回失败，让上层提示用户
            ESP_LOGW(TAG, "Component already exists: %s", upper_pn.c_str());
            return false;
        }
    }

    // 检查容量
    if (components_.size() >= MAX_COMPONENTS) {
        ESP_LOGE(TAG, "Database full, cannot add more components");
        return false;
    }

    // 添加新元器件
    Component new_comp = comp;
    new_comp.part_number = upper_pn;  // 存储为大写格式

    // 调用方可以显式指定逻辑槽位；否则自动分配第一个空闲槽位。
    const uint16_t requested_slot = new_comp.led_index;
    if (requested_slot >= MAX_COMPONENTS) {
        new_comp.led_index = FindFreeLed(MAX_COMPONENTS);
    } else {
        for (const auto& existing : components_) {
            if (existing.led_index == requested_slot) {
                new_comp.led_index = FindFreeLed(MAX_COMPONENTS);
                break;
            }
        }
    }

    if (new_comp.led_index >= MAX_COMPONENTS) {
        ESP_LOGE(TAG, "No free logical LED slot available");
        return false;
    }
    
    components_.push_back(new_comp);
    
    // 尝试保存（即使失败也不删除，因为元器件已在内存中）
    bool save_result = SaveToFile();
    ESP_LOGI(TAG, "Added component: %s, SaveToFile result: %s", 
             new_comp.part_number.c_str(), save_result ? "SUCCESS" : "FAILED");
    
    return true;  // 始终返回成功，因为元器件已添加到内存中
}

bool ComponentDatabase::RemoveComponent(const std::string& part_number) {
    // 转换为大写进行不区分大小写的比较
    std::string upper_pn = part_number;
    for (auto& c : upper_pn) c = toupper(c);
    
    for (auto it = components_.begin(); it != components_.end(); ++it) {
        std::string existing_upper = it->part_number;
        for (auto& c : existing_upper) c = toupper(c);
        
        if (existing_upper == upper_pn) {
            components_.erase(it);
            SaveToFile();
            ESP_LOGI(TAG, "Removed component: %s", part_number.c_str());
            return true;
        }
    }
    return false;
}

void ComponentDatabase::Clear() {
    components_.clear();
    SaveToFile();
    ESP_LOGI(TAG, "All components cleared");
}

std::string ComponentDatabase::GetAllComponentsJson() const {
    // 返回数组格式（给API用）
    cJSON* root = cJSON_CreateArray();
    
    for (const auto& comp : components_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "part_number", comp.part_number.c_str());
        cJSON_AddNumberToObject(item, "led_index", comp.led_index);
        cJSON_AddNumberToObject(item, "r", comp.r);
        cJSON_AddNumberToObject(item, "g", comp.g);
        cJSON_AddNumberToObject(item, "b", comp.b);
        cJSON_AddItemToArray(root, item);
    }

    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return result;
}

uint16_t ComponentDatabase::FindFreeLed(size_t capacity) const {
    // 创建LED使用标记数组
    std::vector<bool> led_used(capacity, false);
    
    // 标记已使用的LED
    for (const auto& comp : components_) {
        if (comp.led_index < capacity) {
            led_used[comp.led_index] = true;
            ESP_LOGI(TAG, "FindFreeLed: slot %u used by %s", comp.led_index, comp.part_number.c_str());
        }
    }
    
    // 找到第一个空闲的LED
    for (size_t i = 0; i < capacity; i++) {
        if (!led_used[i]) {
            ESP_LOGI(TAG, "FindFreeLed: found free slot %u", static_cast<unsigned>(i));
            return static_cast<uint16_t>(i);
        }
    }
    
    // 如果所有槽位都使用了，返回 capacity 作为失败标记。
    ESP_LOGW(TAG, "FindFreeLed: all slots used, returning overflow index %u", static_cast<unsigned>(capacity));
    return static_cast<uint16_t>(capacity);
}

std::string ComponentDatabase::ExportToJson() const {
    // 保存到文件用的格式（带版本号）
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", COMPONENT_DB_VERSION);
    
    cJSON* array = cJSON_CreateArray();
    for (const auto& comp : components_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "part_number", comp.part_number.c_str());
        cJSON_AddNumberToObject(item, "led_index", comp.led_index);
        cJSON_AddNumberToObject(item, "r", comp.r);
        cJSON_AddNumberToObject(item, "g", comp.g);
        cJSON_AddNumberToObject(item, "b", comp.b);
        cJSON_AddItemToArray(array, item);
    }
    cJSON_AddItemToObject(root, "components", array);

    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return result;
}

bool ComponentDatabase::ImportFromJson(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Invalid JSON format");
        return false;
    }

    // 检查是否是新版格式（有version字段的对象）
    if (cJSON_IsObject(root)) {
        // 检查版本号
        cJSON* version = cJSON_GetObjectItem(root, "version");
        if (version && cJSON_IsNumber(version)) {
            int file_version = version->valueint;
            ESP_LOGI(TAG, "File version: %d, Code version: %d", file_version, COMPONENT_DB_VERSION);
            if (file_version != COMPONENT_DB_VERSION) {
                ESP_LOGW(TAG, "Version mismatch! File version %d != Code version %d, ignoring file", 
                          file_version, COMPONENT_DB_VERSION);
                cJSON_Delete(root);
                return false;  // 版本不匹配，返回false让调用者加载默认值
            }
        }
        
        // 获取components数组
        cJSON* components_array = cJSON_GetObjectItem(root, "components");
        if (!components_array || !cJSON_IsArray(components_array)) {
            ESP_LOGE(TAG, "No components array in JSON");
            cJSON_Delete(root);
            return false;
        }
        
        components_.clear();
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, components_array) {
            Component comp;
            cJSON* pn = cJSON_GetObjectItem(item, "part_number");
            cJSON* led = cJSON_GetObjectItem(item, "led_index");
            cJSON* r = cJSON_GetObjectItem(item, "r");
            cJSON* g = cJSON_GetObjectItem(item, "g");
            cJSON* b = cJSON_GetObjectItem(item, "b");

            if (pn && cJSON_IsString(pn)) {
                comp.part_number = pn->valuestring;
                comp.led_index = static_cast<uint16_t>(led ? led->valueint : 0);
                comp.r = r ? r->valueint : 255;
                comp.g = g ? g->valueint : 0;
                comp.b = b ? b->valueint : 0;
                components_.push_back(comp);
            }
        }
        
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Imported %d components (new format)", components_.size());
        return true;
    }
    
    // 旧版格式（直接是数组）- 视为版本不匹配
    ESP_LOGW(TAG, "Old format detected (no version field), treating as invalid");
    cJSON_Delete(root);
    return false;
}

bool ComponentDatabase::ResetToDefault() {
    components_.clear();
    LoadDefaultComponents();
    SaveToFile();
    ESP_LOGI(TAG, "Reset to default %d components", components_.size());
    return true;
}

bool ComponentDatabase::MigrateColorsToPurePalette() {
    bool changed = false;

    for (auto& comp : components_) {
        const ComponentPaletteColor palette_color = GetPaletteColor(comp.led_index);
        if (comp.r == palette_color.r &&
            comp.g == palette_color.g &&
            comp.b == palette_color.b) {
            continue;
        }

        ESP_LOGI(TAG, "Migrating color: %s LED%d (%d,%d,%d) -> (%d,%d,%d)",
                 comp.part_number.c_str(),
                 comp.led_index,
                 comp.r, comp.g, comp.b,
                 palette_color.r, palette_color.g, palette_color.b);

        comp.r = palette_color.r;
        comp.g = palette_color.g;
        comp.b = palette_color.b;
        changed = true;
    }

    if (changed) {
        ESP_LOGI(TAG, "Migrated existing component colors to pure palette");
    }

    return changed;
}

void ComponentDatabase::LoadDefaultComponents() {
    // 不再预装任何元器件，让用户手动添加
    // 这样第一个添加的元器件会映射到 LED0
    components_.clear();
    ESP_LOGI(TAG, "LoadDefaultComponents: no default components loaded (user will add manually)");
}

// 保存到文件
bool ComponentDatabase::SaveToFile() {
    if (!initialized_) {
        ESP_LOGE(TAG, "SaveToFile called but not initialized!");
        return false;
    }

    std::string json = ExportToJson();
    if (json.empty()) {
        ESP_LOGE(TAG, "ExportToJson returned empty string!");
        return false;
    }

    // 写入文件
    FILE* f = fopen(COMPONENTS_FILE_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", COMPONENTS_FILE_PATH);
        return false;
    }

    size_t written = fwrite(json.c_str(), 1, json.size(), f);
    fclose(f);

    if (written != json.size()) {
        ESP_LOGE(TAG, "Write failed: %zu/%zu bytes written", written, json.size());
        return false;
    }

    ESP_LOGI(TAG, "SaveToFile SUCCESS: %zu bytes", json.size());
    return true;
}

// 从文件加载
bool ComponentDatabase::LoadFromFile() {
    struct stat st;
    if (stat(COMPONENTS_FILE_PATH, &st) != 0) {
        ESP_LOGI(TAG, "No file found, will use defaults");
        return false;
    }

    FILE* f = fopen(COMPONENTS_FILE_PATH, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return false;
    }

    char* buffer = (char*)malloc(st.st_size + 1);
    if (!buffer) {
        fclose(f);
        ESP_LOGE(TAG, "malloc failed");
        return false;
    }

    size_t read_size = fread(buffer, 1, st.st_size, f);
    fclose(f);
    buffer[read_size] = '\0';

    if (read_size != (size_t)st.st_size) {
        ESP_LOGE(TAG, "Read failed: %zu/%d bytes", read_size, (int)st.st_size);
        free(buffer);
        return false;
    }

    ESP_LOGI(TAG, "LoadFromFile: read %zu bytes", read_size);
    bool success = ImportFromJson(buffer);
    free(buffer);

    ESP_LOGI(TAG, "LoadFromFile: ImportFromJson result = %s", success ? "SUCCESS" : "FAILED");
    
    if (success) {
        // 修复重复的LED分配
        FixDuplicateLedAssignments();
    }
    
    return success;
}

// 修复重复的LED分配问题
void ComponentDatabase::FixDuplicateLedAssignments() {
    const size_t max_slots = MAX_COMPONENTS;
    bool has_duplicate = false;
    
    // 第一步：检测是否有重复
    std::map<uint16_t, std::vector<std::string>> led_to_components;
    for (const auto& comp : components_) {
        if (comp.led_index < max_slots) {
            led_to_components[comp.led_index].push_back(comp.part_number);
            if (led_to_components[comp.led_index].size() > 1) {
                has_duplicate = true;
            }
        }
    }
    
    if (!has_duplicate) {
        ESP_LOGI(TAG, "No duplicate LED assignments found");
        return;
    }
    
    ESP_LOGW(TAG, "Found duplicate LED assignments, fixing...");
    
    // 第二步：重新分配重复的LED
    std::vector<bool> led_used(max_slots, false);
    
    // 先标记所有当前使用的LED
    for (const auto& comp : components_) {
        if (comp.led_index < max_slots) {
            led_used[comp.led_index] = true;
        }
    }
    
    // 需要删除的组件列表
    std::vector<std::string> to_remove;
    
    // 对每个有重复的LED，重新分配除第一个外的所有组件
    for (auto& entry : led_to_components) {
        uint16_t led_index = entry.first;
        std::vector<std::string>& comps = entry.second;
        
        if (comps.size() > 1) {
            // 从第二个组件开始尝试重新分配
            for (size_t i = 1; i < comps.size(); i++) {
                std::string& part_num = comps[i];
                
                // 找到第一个空闲的LED
                int new_led = -1;
                for (size_t j = 0; j < max_slots; j++) {
                    if (!led_used[j]) {
                        new_led = static_cast<int>(j);
                        break;
                    }
                }
                
                if (new_led >= 0) {
                    // 更新组件的LED索引
                    for (auto& comp : components_) {
                        if (comp.part_number == part_num) {
                            ESP_LOGI(TAG, "Reassigning component %s from LED %d to LED %d",
                                     part_num.c_str(), led_index, new_led);
                            comp.led_index = static_cast<uint16_t>(new_led);
                            led_used[new_led] = true;
                            break;
                        }
                    }
                } else {
                    // 没有空闲LED，删除重复的组件
                    ESP_LOGW(TAG, "No free LED for %s (LED %d), marking for removal", 
                             part_num.c_str(), led_index);
                    to_remove.push_back(part_num);
                }
            }
        }
    }
    
    // 删除重复的组件
    for (const auto& part_num : to_remove) {
        ESP_LOGI(TAG, "Removing duplicate component: %s", part_num.c_str());
        RemoveComponent(part_num);
    }
    
    // 保存修复后的数据
    ESP_LOGI(TAG, "Saving fixed LED assignments to file");
    SaveToFile();
}
