/**
 * @file buzzer_control.h
 * @brief MAX98357A I2S 功放提示音控制
 */
#ifndef _BUZZER_CONTROL_H_
#define _BUZZER_CONTROL_H_

#include <stdint.h>
#include <driver/i2s_types.h>

class BuzzerControl {
public:
    BuzzerControl();
    ~BuzzerControl();

    // 初始化 MAX98357A I2S 输出
    bool Initialize();

    // 单次提示音
    // duration_ms: 提示音持续时间(毫秒)
    void Beep(uint16_t duration_ms);

    // 指定频率的提示音
    // frequency_hz: 音调频率(Hz)
    // duration_ms: 提示音持续时间(毫秒)
    void Tone(uint16_t frequency_hz, uint16_t duration_ms);

    // 多次提示音（用于溢出组指示）
    // count: 提示音次数
    // interval_ms: 每次提示音之间的间隔
    void BeepMultiple(uint8_t count, uint16_t duration_ms, uint16_t interval_ms);

    // 成功音效（找到元器件）
    void SuccessBeep();

    // 失败音效（未找到）
    void ErrorBeep();

    // 溢出组音效（根据组别次数）
    // group: 分组编号(1,2,3...)
    void OverflowBeep(uint8_t group);

private:
    void WriteSilence(uint16_t duration_ms);

    i2s_chan_handle_t tx_handle_;
    bool initialized_;
};

#endif // _BUZZER_CONTROL_H_
