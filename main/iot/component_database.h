/**
 * @file component_database.h
 * @brief 元器件数据库 - 支持文件存储和分组溢出机制
 */
#ifndef _COMPONENT_DATABASE_H_
#define _COMPONENT_DATABASE_H_

#include <string>
#include <vector>
#include "component_config.h"

struct Component {
    std::string part_number;  // 元器件编号 (如 "C25882")
    uint16_t led_index;       // 逻辑LED索引，可跨组映射到实际LED
    uint8_t r, g, b;          // RGB颜色值

    Component() : led_index(0), r(0), g(0), b(0) {}
    Component(const std::string& pn, uint16_t led, uint8_t red, uint8_t green, uint8_t blue)
        : part_number(pn), led_index(led), r(red), g(green), b(blue) {}
};

class ComponentDatabase {
public:
    ComponentDatabase();
    ~ComponentDatabase();

    // 初始化数据库（加载NVS或使用默认数据）
    bool Initialize();

    // 查找元器件
    // 返回true表示找到，out_led/out_color接收实际LED索引和颜色
    // out_group返回分组编号(1,2,3...)用于溢出处理
    bool FindComponent(const std::string& part_number, 
                       uint8_t& out_led_index,
                       uint8_t& out_r, uint8_t& out_g, uint8_t& out_b,
                       uint8_t& out_group);

    // 添加元器件
    bool AddComponent(const Component& comp);

    // 删除元器件
    bool RemoveComponent(const std::string& part_number);

    // 清空所有元器件
    void Clear();

    // 获取所有元器件（JSON格式）
    std::string GetAllComponentsJson() const;

    // 获取元器件数量
    size_t GetCount() const { return components_.size(); }

    // 获取元器件引用（用于更新）
    std::vector<Component>& GetComponentsRef() { return components_; }

    // 查找第一个空闲的LED索引
    uint16_t FindFreeLed(size_t capacity) const;
    
    // 导出到JSON字符串
    std::string ExportToJson() const;

    // 从JSON导入
    bool ImportFromJson(const std::string& json);

    // 修复重复的LED分配问题
    void FixDuplicateLedAssignments();

    // 重置为默认数据
    bool ResetToDefault();

    // 保存到文件（公开给外部调用）
    bool SaveToFile();

    // 将已有元器件颜色迁移为固定高亮纯色
    bool MigrateColorsToPurePalette();

private:
    std::vector<Component> components_;
    bool initialized_;

    // 加载默认元器件数据
    void LoadDefaultComponents();

    // 从文件加载
    bool LoadFromFile();
};

#endif // _COMPONENT_DATABASE_H_
