/**
 * @file match_game.h
 * @brief 灯带消消乐 - 按同色按钮发射灯珠碰撞消除的游戏引擎
 *
 * 4个彩色按钮分别对应红/黄/蓝/绿，按下后发射同色灯珠撞向同色目标。
 * 当前按一整条线性灯带显示：LED0 -> LED_STRIP_LENGTH-1 正常顺序。
 */
#ifndef _MATCH_GAME_H_
#define _MATCH_GAME_H_

#include <stdint.h>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <driver/gpio.h>
#include "component_config.h"
#include "led/circular_strip.h"
#include "buzzer_control.h"
#include "component_http_server.h"
#include "component_ws_server.h"

// 4种游戏颜色
struct GameColor {
    uint8_t r, g, b;
    const char* name;
};

// 敌人实体
struct Enemy {
    uint8_t row;
    uint8_t col;
    uint8_t color_index;  // 0=红, 1=黄, 2=蓝, 3=绿
    bool active;
};

// 飞行动画实体；当前主要使用快速碰撞动画，保留用于状态兼容。
struct Projectile {
    uint8_t row;
    uint8_t col;
    uint8_t color_index;
    bool active;
    bool moving;  // true = 正在飞行中
};

// 爆炸动画帧
struct Explosion {
    uint8_t row;
    uint8_t col;
    uint8_t color_index;
    uint8_t frame;      // 当前帧 0-5
    bool active;
};

class MatchGame {
public:
    MatchGame();
    ~MatchGame();

    bool Initialize();
    bool Start();
    void Stop();

    void OnButtonPressed(uint8_t color_index);

    bool IsInitialized() const { return initialized_; }
    bool IsGameRunning() const { return state_ == GameState::PLAYING; }

    std::string GetStatusJson() const;
    void Broadcast(const std::string& message);

private:
    static constexpr int ROWS = GAME_ROWS;
    static constexpr int COLS = GAME_COLS;
    static constexpr int TOTAL_LEDS = ROWS * COLS;
    static constexpr int NUM_COLORS = 4;
    static constexpr int MAX_ENEMIES = 30;
    static constexpr int MAX_EXPLOSIONS = 10;
    static constexpr int BUTTON_QUEUE_LEN = 8;
    static constexpr int BUTTON_SCAN_MS = 10;
    static constexpr int BUTTON_STABLE_SAMPLES = 3;
    static constexpr int BUTTON_EVENT_COOLDOWN_MS = 120;
    static constexpr int BUTTON_DIAG_INTERVAL_MS = 5000;

    enum class GameState { IDLE, PLAYING, GAME_OVER };

    // 游戏状态
    volatile GameState state_;
    uint32_t score_;
    uint8_t level_;
    uint32_t combo_;
    uint32_t last_eliminate_tick_;
    uint32_t game_tick_;
    uint16_t high_score_;

    // 实体
    Enemy enemies_[MAX_ENEMIES];
    int enemy_count_;
    Projectile projectile_;
    Explosion explosions_[MAX_EXPLOSIONS];

    // 组件
    CircularStrip* strip_;
    BuzzerControl* buzzer_;
    TaskHandle_t game_task_;
    TaskHandle_t button_task_;
    QueueHandle_t button_queue_;
    bool initialized_;

    // HTTP/WS
    ComponentHttpServer http_server_;
    ComponentWsServer ws_server_;

    // 按钮中断队列（避免在中断中做复杂逻辑）
    volatile uint8_t pending_button_;  // 0xFF = 无, 0-3 = 颜色索引
    volatile TickType_t last_button_tick_[NUM_COLORS];
    bool button_down_[NUM_COLORS];
    uint8_t button_active_level_[NUM_COLORS];
    uint8_t button_released_level_[NUM_COLORS];
    uint8_t button_raw_level_[NUM_COLORS];
    uint8_t button_stable_level_[NUM_COLORS];
    uint8_t button_candidate_level_[NUM_COLORS];
    uint8_t button_candidate_count_[NUM_COLORS];
    gpio_num_t button_gpios_[NUM_COLORS];

    // 核心方法
    static void GameTaskFunc(void* param);
    void GameLoop();
    void HandleIdle();
    void HandlePlaying();
    void HandleGameOver();

    // 游戏逻辑
    void ResetGame(uint8_t first_color = 0xFF);
    void SpawnEnemy(uint8_t forced_color = 0xFF);
    void MoveEnemies();
    void FireProjectile(uint8_t color_index);
    void AnimateShotToTarget(int target_idx, uint8_t color_index);
    void MoveProjectile();
    bool CheckCollision();
    void EliminateEnemy(int enemy_idx);
    void UpdateExplosions();
    void UpdateLevel();

    // 渲染
    void Render();
    void RenderIdle();
    void RenderPlaying();
    void RenderGameOver();
    void SetGridLed(uint8_t row, uint8_t col, const StripColor& color);

    // 工具方法
    static uint8_t MatrixIndex(uint8_t row, uint8_t col);
    static GameColor GetGameColor(uint8_t index);
    void InitButtonGpios();
    static void ButtonTaskFunc(void* param);
    void ButtonLoop();
    void DrainButtonQueue();
    void QueueButtonPress(uint8_t color_index);
    void PollButtons();
    static void IRAM_ATTR ButtonIsr(void* arg);

    // HTTP/WS
    bool HandleHttpRequest(const std::string& uri, const std::string& method,
                           const std::string& body, std::string& response);
    void HandleWsMessage(const WsMessage& msg);
    void BroadcastState();
};

#endif  // _MATCH_GAME_H_
