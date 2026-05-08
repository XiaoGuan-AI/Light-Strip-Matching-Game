/**
 * @file component_led_control.h
 * @brief 元器件分拣LED控制 - 简化接口
 */
#ifndef _COMPONENT_LED_CONTROL_H_
#define _COMPONENT_LED_CONTROL_H_

#include <stdint.h>
#include <vector>
#include <map>
#include <memory>
#include "led/circular_strip.h"
#include "component_config.h"

// LED闪烁任务结构
struct LedBlinkTask {
    uint8_t led_index;
    uint8_t r, g, b;
    uint32_t start_time;      // 开始时间（毫秒）
    uint32_t duration;        // 持续时间（毫秒）
    bool active;              // 是否活跃
    
    LedBlinkTask(uint8_t index, uint8_t red, uint8_t green, uint8_t blue, uint32_t dur)
        : led_index(index), r(red), g(green), b(blue), 
          start_time(0), duration(dur), active(true) {}
};

class ComponentLedControl {
public:
    ComponentLedControl();
    ~ComponentLedControl();

    // 初始化LED灯带
    bool Initialize();

    // 高亮指定LED（同步阻塞闪烁，已不推荐使用）
    // led_index: LED索引 (0 ~ LED_STRIP_LENGTH-1)
    // r,g,b: RGB颜色
    // blink_count: 闪烁次数
    // group: 分组编号(>1表示溢出组)
    void HighlightLed(uint8_t led_index, uint8_t r, uint8_t g, uint8_t b, 
                      uint8_t blink_count, uint8_t group);
    
    // 启动异步闪烁任务（不阻塞，闪烁指定时长后自动熄灭）
    // led_index: LED索引
    // r,g,b: RGB颜色
    // duration_ms: 闪烁持续时间（毫秒）
    void StartAsyncBlink(uint8_t led_index, uint8_t r, uint8_t g, uint8_t b, uint32_t duration_ms);
    
    // 停止指定LED的闪烁
    void StopBlink(uint8_t led_index);
    
    // 停止所有闪烁
    void StopAllBlink();
    
    // 更新LED状态（需要在主循环或定时器中调用）
    void Update();

    // 设置所有LED颜色
    void SetAllColor(uint8_t r, uint8_t g, uint8_t b);

    // 清除所有LED
    void Clear();

    // 流水灯效果
    void RunWaterfall();

    // 开机倒计时灯效（6x8蛇形矩阵显示 5,4,3,2,1,0）
    void RunStartupCountdown();

    // 获取LED数量
    uint8_t GetLedCount() const { return LED_STRIP_LENGTH; }

private:
    CircularStrip* strip_;
    bool initialized_;
    std::map<uint8_t, std::unique_ptr<LedBlinkTask>> blink_tasks_;  // 异步闪烁任务
};

#endif // _COMPONENT_LED_CONTROL_H_
