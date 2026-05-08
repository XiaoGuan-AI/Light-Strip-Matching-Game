/**
 * @file match_game.cc
 * @brief 灯带消消乐 - 按同色按钮发射灯珠碰撞消除的游戏引擎实现
 */
#include "match_game.h"
#include "component_config.h"
#include <esp_log.h>
#include <esp_random.h>
#include <cJSON.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#define TAG "MatchGame"

// ============================================================================
// 颜色定义
// ============================================================================
static const GameColor kGameColors[4] = {
    {255, 30, 30, "red"},       // 红
    {255, 230, 30, "yellow"},   // 黄
    {30, 120, 255, "blue"},     // 蓝
    {30, 255, 60, "green"},     // 绿
};

static const gpio_num_t kDefaultButtonGpios[4] = {
    BTN_RED_GPIO,
    BTN_YELLOW_GPIO,
    BTN_BLUE_GPIO,
    BTN_GREEN_GPIO,
};

static const uint8_t kDefaultButtonActiveLevels[4] = {
    BTN_RED_ACTIVE_LEVEL,
    BTN_YELLOW_ACTIVE_LEVEL,
    BTN_BLUE_ACTIVE_LEVEL,
    BTN_GREEN_ACTIVE_LEVEL,
};

// 全局实例（用于ISR回调）
static MatchGame* g_game = nullptr;

// ============================================================================
// 构造/析构
// ============================================================================
MatchGame::MatchGame()
    : state_(GameState::IDLE),
      score_(0), level_(1), combo_(0), last_eliminate_tick_(0),
      game_tick_(0), high_score_(0), enemy_count_(0),
      strip_(nullptr), buzzer_(nullptr), game_task_(nullptr),
      button_task_(nullptr), button_queue_(nullptr),
      initialized_(false), pending_button_(0xFF) {
    memset(enemies_, 0, sizeof(enemies_));
    memset(explosions_, 0, sizeof(explosions_));
    memset(&projectile_, 0, sizeof(projectile_));
    memset((void*)last_button_tick_, 0, sizeof(last_button_tick_));
    memset(button_down_, 0, sizeof(button_down_));
    memset(button_active_level_, 1, sizeof(button_active_level_));
    memset(button_released_level_, 1, sizeof(button_released_level_));
    memset(button_raw_level_, 0, sizeof(button_raw_level_));
    memset(button_stable_level_, 0, sizeof(button_stable_level_));
    memset(button_candidate_level_, 0, sizeof(button_candidate_level_));
    memset(button_candidate_count_, 0, sizeof(button_candidate_count_));
    memcpy(button_gpios_, kDefaultButtonGpios, sizeof(button_gpios_));
    memcpy(button_active_level_, kDefaultButtonActiveLevels, sizeof(button_active_level_));
    g_game = this;
}

MatchGame::~MatchGame() {
    Stop();
    g_game = nullptr;
}

// ============================================================================
// MatrixIndex - 一整条线性灯带，正常顺序 LED0 -> LED_STRIP_LENGTH-1
// ============================================================================
uint8_t MatchGame::MatrixIndex(uint8_t row, uint8_t col) {
    return row * COLS + col;
}

GameColor MatchGame::GetGameColor(uint8_t index) {
    return kGameColors[index % NUM_COLORS];
}

// ============================================================================
// 初始化
// ============================================================================
bool MatchGame::Initialize() {
    if (initialized_) return true;

    // 创建LED灯带
    strip_ = new CircularStrip(LED_STRIP_PIN, LED_STRIP_LENGTH);
    if (!strip_) {
        ESP_LOGE(TAG, "Failed to create LED strip");
        return false;
    }
    strip_->SetBrightness(LED_BRIGHTNESS, 10);
    strip_->SetAllColor({0, 0, 0});

    // 创建 MAX98357A I2S 提示音控制
    buzzer_ = new BuzzerControl();
    if (!buzzer_ || !buzzer_->Initialize()) {
        ESP_LOGW(TAG, "Speaker init failed, continuing without sound");
    }

    // 初始化按钮GPIO
    InitButtonGpios();

    // 初始化HTTP服务器
    http_server_.Initialize(HTTP_SERVER_PORT);
    http_server_.SetRequestCallback(
        [this](const std::string& uri, const std::string& method,
               const std::string& body, std::string& response) {
            return HandleHttpRequest(uri, method, body, response);
        });

    // 初始化WebSocket服务器
    ws_server_.Initialize(WS_SERVER_PORT);
    ws_server_.SetMessageCallback([this](const WsMessage& msg) {
        HandleWsMessage(msg);
    });

    initialized_ = true;
    ESP_LOGI(TAG, "Match game initialized as linear strip (%d LEDs)", LED_STRIP_LENGTH);
    return true;
}

void MatchGame::InitButtonGpios() {
    for (int i = 0; i < NUM_COLORS; i++) {
        const bool active_high = (button_active_level_[i] != 0);
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << button_gpios_[i]);
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE;
        io_conf.pull_down_en = active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_conf));

        button_released_level_[i] = active_high ? 0 : 1;
        uint8_t level = gpio_get_level(button_gpios_[i]) ? 1 : 0;
        button_raw_level_[i] = level;
        button_stable_level_[i] = level;
        button_candidate_level_[i] = level;
        button_candidate_count_[i] = 0;
        button_down_[i] = (level == button_active_level_[i]);

        ESP_LOGI(TAG, "Button %d (color %d) on GPIO%d, active_level=%d, idle_level=%d, boot_level=%d",
                 i, i, button_gpios_[i], button_active_level_[i],
                 button_released_level_[i], level);
        if (button_down_[i]) {
            ESP_LOGW(TAG, "Button %d GPIO%d looks PRESSED at boot. If you are not holding it, check OUT/VCC/GND or the active level config.",
                     i, button_gpios_[i]);
        }
    }
}

void IRAM_ATTR MatchGame::ButtonIsr(void* arg) {
    if (!g_game) return;
    uint8_t color_index = (uint8_t)(intptr_t)arg;
    if (color_index >= NUM_COLORS) return;

    int level = gpio_get_level(g_game->button_gpios_[color_index]);
    if (level != g_game->button_active_level_[color_index]) {
        return;
    }

    TickType_t now = xTaskGetTickCountFromISR();
    if ((now - g_game->last_button_tick_[color_index]) < pdMS_TO_TICKS(120)) {
        return;
    }
    g_game->last_button_tick_[color_index] = now;
    g_game->pending_button_ = color_index;
}

void MatchGame::PollButtons() {
    for (int i = 0; i < NUM_COLORS; ++i) {
        int level = gpio_get_level(button_gpios_[i]);
        const bool pressed = (level == button_active_level_[i]);
        if (pressed && !button_down_[i]) {
            button_down_[i] = true;
            if (pending_button_ == 0xFF) {
                pending_button_ = i;
            }
            ESP_LOGI(TAG, "Button poll detected: color %d on GPIO%d level=%d active=%d",
                     i, button_gpios_[i], level, button_active_level_[i]);
        } else if (!pressed && button_down_[i]) {
            button_down_[i] = false;
            ESP_LOGI(TAG, "Button released: color %d on GPIO%d level=%d",
                     i, button_gpios_[i], level);
        }
    }
}

void MatchGame::ButtonTaskFunc(void* param) {
    auto* self = static_cast<MatchGame*>(param);
    self->ButtonLoop();
}

void MatchGame::QueueButtonPress(uint8_t color_index) {
    if (color_index >= NUM_COLORS) {
        return;
    }

    if (button_queue_) {
        if (xQueueSend(button_queue_, &color_index, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Button queue full, dropped color %d", color_index);
        }
    } else if (pending_button_ == 0xFF) {
        pending_button_ = color_index;
    }
}

void MatchGame::ButtonLoop() {
    ESP_LOGI(TAG, "Button scan task started");
    TickType_t last_diag_tick = 0;

    while (true) {
        TickType_t now = xTaskGetTickCount();

        for (int i = 0; i < NUM_COLORS; ++i) {
            uint8_t level = gpio_get_level(button_gpios_[i]) ? 1 : 0;
            if (level != button_raw_level_[i]) {
                ESP_LOGI(TAG, "Button raw change: color %d GPIO%d %d->%d active=%d",
                         i, button_gpios_[i], button_raw_level_[i], level,
                         button_active_level_[i]);
                button_raw_level_[i] = level;
            }

            if (level != button_candidate_level_[i]) {
                button_candidate_level_[i] = level;
                button_candidate_count_[i] = 1;
                continue;
            }
            if (button_candidate_count_[i] < BUTTON_STABLE_SAMPLES) {
                button_candidate_count_[i]++;
            }
            if (button_candidate_count_[i] < BUTTON_STABLE_SAMPLES ||
                level == button_stable_level_[i]) {
                continue;
            }

            button_stable_level_[i] = level;
            const bool pressed = (level == button_active_level_[i]);
            if (pressed && !button_down_[i]) {
                button_down_[i] = true;
                if ((now - last_button_tick_[i]) >= pdMS_TO_TICKS(BUTTON_EVENT_COOLDOWN_MS)) {
                    last_button_tick_[i] = now;
                    QueueButtonPress(i);
                    ESP_LOGI(TAG, "Button queued: color %d GPIO%d level=%d active=%d",
                             i, button_gpios_[i], level, button_active_level_[i]);
                } else {
                    ESP_LOGI(TAG, "Button debounce ignored: color %d GPIO%d", i, button_gpios_[i]);
                }
            } else if (!pressed && button_down_[i]) {
                button_down_[i] = false;
                ESP_LOGI(TAG, "Button released: color %d GPIO%d level=%d",
                         i, button_gpios_[i], level);
            }
        }

        if ((now - last_diag_tick) >= pdMS_TO_TICKS(BUTTON_DIAG_INTERVAL_MS)) {
            last_diag_tick = now;
            ESP_LOGI(TAG, "Button raw levels: R=%d Y=%d B=%d G=%d (active R=%d Y=%d B=%d G=%d)",
                     button_raw_level_[0], button_raw_level_[1],
                     button_raw_level_[2], button_raw_level_[3],
                     button_active_level_[0], button_active_level_[1],
                     button_active_level_[2], button_active_level_[3]);
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_MS));
    }
}

void MatchGame::DrainButtonQueue() {
    uint8_t btn = 0;
    while (button_queue_ && xQueueReceive(button_queue_, &btn, 0) == pdTRUE) {
        OnButtonPressed(btn);
    }

    btn = pending_button_;
    if (btn != 0xFF) {
        pending_button_ = 0xFF;
        OnButtonPressed(btn);
    }
}

// ============================================================================
// 启动/停止
// ============================================================================
bool MatchGame::Start() {
    if (!initialized_) return false;

    if (!button_queue_) {
        button_queue_ = xQueueCreate(BUTTON_QUEUE_LEN, sizeof(uint8_t));
        if (!button_queue_) {
            ESP_LOGE(TAG, "Failed to create button queue");
            return false;
        }
    }

    // 启动HTTP服务器
    if (!http_server_.Start()) {
        ESP_LOGE(TAG, "HTTP server start failed");
    }
    if (!ws_server_.Start()) {
        ESP_LOGE(TAG, "WS server start failed");
    }

    if (!button_task_) {
        xTaskCreatePinnedToCore(
            ButtonTaskFunc, "button_scan", 3072, this, 6, &button_task_, 0);
    }

    // 启动游戏主循环任务
    if (!game_task_) {
        xTaskCreatePinnedToCore(
            GameTaskFunc, "game_loop", 4096, this, 5, &game_task_, 0);
    }

    ESP_LOGI(TAG, "Match game started");
    return true;
}

void MatchGame::Stop() {
    if (button_task_) {
        vTaskDelete(button_task_);
        button_task_ = nullptr;
    }
    if (game_task_) {
        vTaskDelete(game_task_);
        game_task_ = nullptr;
    }
    if (button_queue_) {
        vQueueDelete(button_queue_);
        button_queue_ = nullptr;
    }
    http_server_.Stop();
    ws_server_.Stop();
    if (strip_) {
        strip_->SetAllColor({0, 0, 0});
    }
}

// ============================================================================
// 游戏主循环
// ============================================================================
void MatchGame::GameTaskFunc(void* param) {
    auto* self = static_cast<MatchGame*>(param);
    self->GameLoop();
}

void MatchGame::GameLoop() {
    ESP_LOGI(TAG, "Game loop started");

    // 开机动画
    strip_->SetAllColor({0, 30, 60});
    vTaskDelay(pdMS_TO_TICKS(500));
    strip_->SetAllColor({0, 0, 0});

    while (true) {
        // 处理按钮输入
        DrainButtonQueue();

        switch (state_) {
            case GameState::IDLE:
                HandleIdle();
                break;
            case GameState::PLAYING:
                HandlePlaying();
                break;
            case GameState::GAME_OVER:
                HandleGameOver();
                break;
        }

        Render();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ============================================================================
// IDLE 状态
// ============================================================================
void MatchGame::HandleIdle() {
    // 显示等待动画 - 呼吸灯效果
    static uint8_t breath = 0;
    static bool increasing = true;
    static uint32_t idle_tick = 0;

    idle_tick++;
    if (idle_tick % 3 == 0) {  // 每150ms更新一次呼吸
        if (increasing) {
            breath += 2;
            if (breath >= 80) increasing = false;
        } else {
            breath -= 2;
            if (breath <= 5) increasing = true;
        }
    }

    // 在网格边缘显示柔和的颜色提示
    for (int r = 0; r < ROWS; r++) {
        StripColor c = {
            static_cast<uint8_t>(breath / 4),
            static_cast<uint8_t>(breath / 3),
            static_cast<uint8_t>(breath / 2),
        };
        SetGridLed(r, 0, c);
        SetGridLed(r, COLS - 1, c);
    }
    for (int c = 0; c < COLS; c++) {
        StripColor bc = {
            static_cast<uint8_t>(breath / 4),
            static_cast<uint8_t>(breath / 3),
            static_cast<uint8_t>(breath / 2),
        };
        SetGridLed(0, c, bc);
        SetGridLed(ROWS - 1, c, bc);
    }
}

// ============================================================================
// PLAYING 状态
// ============================================================================
void MatchGame::HandlePlaying() {
    game_tick_++;

    // 计算当前tick间隔（根据等级加速）
    int ticks_per_move = std::max(GAME_TICK_BASE_MS / 50 - (level_ - 1), 4);

    // 保留旧版飞行动画更新；当前主要使用快速碰撞动画，默认不会激活弹丸。
    if (projectile_.active && projectile_.moving) {
        static uint32_t proj_tick = 0;
        proj_tick++;
        if (proj_tick >= (PROJECTILE_SPEED_MS / 50)) {
            proj_tick = 0;
            MoveProjectile();
        }
    }

    // 敌人移动
    static uint32_t move_tick = 0;
    move_tick++;
    if (move_tick >= (uint32_t)ticks_per_move) {
        move_tick = 0;
        MoveEnemies();
    }

    // 敌人生成
    int spawn_interval = std::max(SPAWN_INTERVAL_BASE - (level_ - 1) / 2, 1);
    if (game_tick_ % (spawn_interval * ticks_per_move) == 0) {
        SpawnEnemy();
    }

    // 更新爆炸动画
    UpdateExplosions();

    // 检查游戏结束
    for (int i = 0; i < enemy_count_; i++) {
        if (enemies_[i].active && enemies_[i].col == 0) {
            state_ = GameState::GAME_OVER;
            if (score_ > high_score_) {
                high_score_ = score_;
            }
            ESP_LOGI(TAG, "GAME OVER! Score=%lu, Level=%d", score_, level_);

            // 游戏结束音效
            if (buzzer_) {
                buzzer_->BeepMultiple(3, 200, 150);
            }

            BroadcastState();
            return;
        }
    }
}

void MatchGame::ResetGame(uint8_t first_color) {
    memset(enemies_, 0, sizeof(enemies_));
    memset(explosions_, 0, sizeof(explosions_));
    memset(&projectile_, 0, sizeof(projectile_));
    enemy_count_ = 0;
    score_ = 0;
    level_ = 1;
    combo_ = 0;
    last_eliminate_tick_ = 0;
    game_tick_ = 0;
    state_ = GameState::PLAYING;
    SpawnEnemy(first_color);
    ESP_LOGI(TAG, "Game reset - LET'S GO! LEDs=%d first_color=%d", LED_STRIP_LENGTH, first_color);
    BroadcastState();
}

// ============================================================================
// 敌人
// ============================================================================
void MatchGame::SpawnEnemy(uint8_t forced_color) {
    int free_slot = -1;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies_[i].active) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) return;

    uint8_t row = esp_random() % ROWS;
    bool row_available = false;
    for (int attempt = 0; attempt < ROWS; attempt++) {
        uint8_t candidate = (row + attempt) % ROWS;
        bool blocked = false;
        for (int i = 0; i < enemy_count_; i++) {
            if (enemies_[i].active && enemies_[i].row == candidate && enemies_[i].col >= COLS - 1) {
                blocked = true;
                break;
            }
        }
        if (!blocked) {
            row = candidate;
            row_available = true;
            break;
        }
    }
    if (!row_available) return;

    uint8_t color = (forced_color < NUM_COLORS) ? forced_color : (esp_random() % NUM_COLORS);

    enemies_[free_slot] = {row, (uint8_t)(COLS - 1), color, true};
    if (free_slot >= enemy_count_) enemy_count_ = free_slot + 1;
    ESP_LOGI(TAG, "Spawn enemy: slot=%d led=%d color=%d", free_slot,
             MatrixIndex(row, COLS - 1), color);
}

void MatchGame::MoveEnemies() {
    for (int i = 0; i < enemy_count_; i++) {
        if (enemies_[i].active && enemies_[i].col > 0) {
            // 检查前方是否有其他敌人
            bool blocked = false;
            for (int j = 0; j < enemy_count_; j++) {
                if (i != j && enemies_[j].active &&
                    enemies_[j].row == enemies_[i].row &&
                    enemies_[j].col == enemies_[i].col - 1) {
                    blocked = true;
                    break;
                }
            }
            if (!blocked) {
                enemies_[i].col--;
            }
        }
    }
}

// ============================================================================
// 发射同色灯珠并碰撞消除
// ============================================================================
void MatchGame::FireProjectile(uint8_t color_index) {
    if (color_index >= NUM_COLORS) return;

    // 按下颜色键后，发射同色灯珠，从玩家侧快速跑向最近的同色目标。
    int target_idx = -1;
    uint8_t min_col = COLS;
    for (int i = 0; i < enemy_count_; i++) {
        if (enemies_[i].active && enemies_[i].color_index == color_index) {
            if (enemies_[i].col < min_col) {
                min_col = enemies_[i].col;
                target_idx = i;
            }
        }
    }

    projectile_.active = false;
    projectile_.moving = false;
    projectile_.color_index = color_index;

    if (target_idx < 0) {
        ESP_LOGI(TAG, "Shot missed: color=%d has no target", color_index);
        if (buzzer_) buzzer_->Beep(30);
        BroadcastState();
        return;
    }

    ESP_LOGI(TAG, "Shot fired: color=%d target_col=%d target_led=%d",
             color_index, enemies_[target_idx].col,
             MatrixIndex(enemies_[target_idx].row, enemies_[target_idx].col));
    AnimateShotToTarget(target_idx, color_index);
    EliminateEnemy(target_idx);
}

void MatchGame::AnimateShotToTarget(int target_idx, uint8_t color_index) {
    if (target_idx < 0 || target_idx >= MAX_ENEMIES || !enemies_[target_idx].active) {
        return;
    }

    const uint8_t target_col = enemies_[target_idx].col;
    const GameColor gc = GetGameColor(color_index);
    const StripColor shot_color = {gc.r, gc.g, gc.b};
    const uint8_t target_row = enemies_[target_idx].row;
    const int step = std::max(1, static_cast<int>(target_col) / 30);

    auto draw_frame = [&](int shot_col, bool collision_flash) {
        std::vector<StripColor> frame(LED_STRIP_LENGTH, {0, 0, 0});

        for (int i = 0; i < enemy_count_; i++) {
            if (!enemies_[i].active) {
                continue;
            }
            uint8_t led = MatrixIndex(enemies_[i].row, enemies_[i].col);
            GameColor enemy_color = GetGameColor(enemies_[i].color_index);
            if (led < frame.size()) {
                frame[led] = {enemy_color.r, enemy_color.g, enemy_color.b};
            }
        }

        uint8_t shot_led = MatrixIndex(target_row, static_cast<uint8_t>(shot_col));
        if (shot_led < frame.size()) {
            frame[shot_led] = collision_flash ? StripColor{255, 255, 255} : shot_color;
        }
        if (!collision_flash && shot_col > 0) {
            uint8_t tail_led = MatrixIndex(target_row, static_cast<uint8_t>(shot_col - 1));
            if (tail_led < frame.size()) {
                frame[tail_led] = {
                    static_cast<uint8_t>(gc.r / 3),
                    static_cast<uint8_t>(gc.g / 3),
                    static_cast<uint8_t>(gc.b / 3),
                };
            }
        }

        if (collision_flash && shot_col == target_col) {
            if (target_col > 0) {
                uint8_t left = MatrixIndex(target_row, target_col - 1);
                if (left < frame.size()) {
                    frame[left] = {
                        static_cast<uint8_t>(gc.r / 2),
                        static_cast<uint8_t>(gc.g / 2),
                        static_cast<uint8_t>(gc.b / 2),
                    };
                }
            }
            if (target_col + 1 < COLS) {
                uint8_t right = MatrixIndex(target_row, target_col + 1);
                if (right < frame.size()) {
                    frame[right] = {
                        static_cast<uint8_t>(gc.r / 2),
                        static_cast<uint8_t>(gc.g / 2),
                        static_cast<uint8_t>(gc.b / 2),
                    };
                }
            }
        }

        strip_->SetColors(frame);
    };

    // 从玩家侧 LED0 向目标方向快速跑过去。最后强制画到目标列，形成明确碰撞。
    int last_col = 0;
    for (int col = 0; col < target_col; col += step) {
        draw_frame(col, false);
        last_col = col;
        vTaskDelay(pdMS_TO_TICKS(12));
    }

    if (last_col != target_col) {
        draw_frame(target_col, false);
        vTaskDelay(pdMS_TO_TICKS(28));
    }

    ESP_LOGI(TAG, "Shot collision: color=%d target_col=%d target_led=%d",
             color_index, target_col, MatrixIndex(target_row, target_col));
    draw_frame(target_col, true);
    vTaskDelay(pdMS_TO_TICKS(55));
}

void MatchGame::MoveProjectile() {
    if (!projectile_.active || !projectile_.moving) return;

    projectile_.col++;

    if (projectile_.col >= COLS) {
        // 飞出边界，消失
        projectile_.active = false;
        projectile_.moving = false;
        ESP_LOGI(TAG, "Projectile missed: color=%d", projectile_.color_index);
        return;
    }

    // 碰撞检测
    if (CheckCollision()) {
        return;
    }
}

bool MatchGame::CheckCollision() {
    int matched_enemy = -1;
    uint8_t matched_col = COLS;

    for (int i = 0; i < enemy_count_; i++) {
        if (!enemies_[i].active || enemies_[i].row != projectile_.row) {
            continue;
        }

        // 只消除同色目标；异色灯不会挡住弹丸。
        // 使用 >= 避免弹丸和敌人相向移动时刚好错过同一格。
        if (enemies_[i].color_index == projectile_.color_index &&
            projectile_.col >= enemies_[i].col &&
            enemies_[i].col < matched_col) {
            matched_enemy = i;
            matched_col = enemies_[i].col;
        }
    }

    if (matched_enemy < 0) {
        return false;
    }

    EliminateEnemy(matched_enemy);
    projectile_.active = false;
    projectile_.moving = false;
    return true;
}

void MatchGame::EliminateEnemy(int enemy_idx) {
    if (enemy_idx < 0 || enemy_idx >= MAX_ENEMIES) return;

    uint8_t row = enemies_[enemy_idx].row;
    uint8_t col = enemies_[enemy_idx].col;
    uint8_t color_idx = enemies_[enemy_idx].color_index;

    // 创建爆炸动画
    for (int i = 0; i < MAX_EXPLOSIONS; i++) {
        if (!explosions_[i].active) {
            explosions_[i] = {row, col, color_idx, 0, true};
            break;
        }
    }

    // 标记敌人为不活跃
    enemies_[enemy_idx].active = false;

    // 更新Combo
    if (game_tick_ - last_eliminate_tick_ < 40) {  // 2秒内连续消除
        combo_++;
    } else {
        combo_ = 1;
    }
    last_eliminate_tick_ = game_tick_;

    // 计分
    uint32_t points = 10 * combo_;
    score_ += points;

    // 音效 - Combo越高音越高
    if (buzzer_) {
        if (combo_ >= 5) {
            buzzer_->BeepMultiple(3, 50, 30);
        } else if (combo_ >= 3) {
            buzzer_->BeepMultiple(2, 60, 40);
        } else {
            buzzer_->Beep(80);
        }
    }

    // 升级检查
    UpdateLevel();

    ESP_LOGI(TAG, "ELIMINATED! Row=%d Col=%d Color=%d Score=%lu Combo=%lu Level=%d",
             row, col, color_idx, score_, combo_, level_);

    BroadcastState();
}

void MatchGame::UpdateExplosions() {
    for (int i = 0; i < MAX_EXPLOSIONS; i++) {
        if (explosions_[i].active) {
            explosions_[i].frame++;
            if (explosions_[i].frame >= 8) {
                explosions_[i].active = false;
            }
        }
    }
}

void MatchGame::UpdateLevel() {
    uint8_t new_level = 1 + score_ / LEVEL_UP_SCORE;
    if (new_level > level_) {
        level_ = new_level;
        ESP_LOGI(TAG, "LEVEL UP! Level=%d", level_);
        // 升级音效
        if (buzzer_) {
            buzzer_->Beep(50);
            vTaskDelay(pdMS_TO_TICKS(50));
            buzzer_->Beep(80);
            vTaskDelay(pdMS_TO_TICKS(50));
            buzzer_->Beep(120);
        }
    }
}

// ============================================================================
// GAME_OVER 状态
// ============================================================================
void MatchGame::HandleGameOver() {
    static uint32_t go_tick = 0;
    static bool flash_on = false;
    go_tick++;

    // 红色闪烁3次
    if (go_tick < 30) {
        if (go_tick % 10 == 0) {
            flash_on = !flash_on;
            StripColor c = flash_on ? StripColor{200, 0, 0} : StripColor{0, 0, 0};
            strip_->SetAllColor(c);
        }
    } else {
        // 闪烁完毕，显示分数（简化：用亮度表示分数范围）
        uint8_t brightness = std::min(score_ / 10, (uint32_t)200);
        StripColor dim = {0, static_cast<uint8_t>(brightness / 2), brightness};
        strip_->SetAllColor(dim);
    }
}

// ============================================================================
// 按钮处理
// ============================================================================
void MatchGame::OnButtonPressed(uint8_t color_index) {
    if (color_index >= NUM_COLORS) return;

    ESP_LOGI(TAG, "Button pressed: color %d (state=%d)", color_index, (int)state_);

    switch (state_) {
        case GameState::IDLE:
            ResetGame(color_index);
            FireProjectile(color_index);
            break;
        case GameState::PLAYING:
            FireProjectile(color_index);
            break;
        case GameState::GAME_OVER:
            ResetGame(color_index);
            FireProjectile(color_index);
            break;
    }
}

// ============================================================================
// 渲染
// ============================================================================
void MatchGame::Render() {
    switch (state_) {
        case GameState::IDLE:
            RenderIdle();
            break;
        case GameState::PLAYING:
            RenderPlaying();
            break;
        case GameState::GAME_OVER:
            RenderGameOver();
            break;
    }
}

void MatchGame::RenderPlaying() {
    std::vector<StripColor> frame(LED_STRIP_LENGTH, {0, 0, 0});

    // 绘制敌人
    for (int i = 0; i < enemy_count_; i++) {
        if (enemies_[i].active) {
            uint8_t led = MatrixIndex(enemies_[i].row, enemies_[i].col);
            GameColor gc = GetGameColor(enemies_[i].color_index);
            if (led < frame.size()) {
                frame[led] = {gc.r, gc.g, gc.b};
            }
        }
    }

    // 兼容旧版飞行动画：当前快速碰撞玩法通常不会进入这个分支。
    if (projectile_.active) {
        uint8_t led = MatrixIndex(projectile_.row, projectile_.col);
        GameColor gc = GetGameColor(projectile_.color_index);
        if (led < frame.size()) {
            // 弹丸主体 - 高亮白+原色混合
            frame[led] = {
                (uint8_t)std::min(gc.r + 80, 255),
                (uint8_t)std::min(gc.g + 80, 255),
                (uint8_t)std::min(gc.b + 80, 255)
            };
        }
        // 拖尾
        if (projectile_.col > 0) {
            uint8_t tail_led = MatrixIndex(projectile_.row, projectile_.col - 1);
            if (tail_led < frame.size()) {
                frame[tail_led] = {(uint8_t)(gc.r / 3), (uint8_t)(gc.g / 3), (uint8_t)(gc.b / 3)};
            }
        }
    }

    // 绘制爆炸
    for (int i = 0; i < MAX_EXPLOSIONS; i++) {
        if (explosions_[i].active) {
            uint8_t led = MatrixIndex(explosions_[i].row, explosions_[i].col);
            GameColor gc = GetGameColor(explosions_[i].color_index);
            uint8_t frame_idx = explosions_[i].frame;

            StripColor exp_color;
            if (frame_idx < 2) {
                // 前2帧：白色闪烁
                uint8_t w = 255 - frame_idx * 60;
                exp_color = {w, w, w};
            } else if (frame_idx < 5) {
                // 中间帧：原色渐暗
                uint8_t fade = 255 - (frame_idx - 2) * 70;
                exp_color = {
                    (uint8_t)(gc.r * fade / 255),
                    (uint8_t)(gc.g * fade / 255),
                    (uint8_t)(gc.b * fade / 255)
                };
            } else {
                // 后续帧：快速消失
                uint8_t fade = std::max(255 - frame_idx * 40, 0);
                exp_color = {
                    (uint8_t)(gc.r * fade / 255),
                    (uint8_t)(gc.g * fade / 255),
                    (uint8_t)(gc.b * fade / 255)
                };
            }
            if (led < frame.size()) {
                frame[led] = exp_color;
            }

            // 波及上下相邻LED
            if (frame_idx < 4) {
                uint8_t wave_fade = 150 - frame_idx * 40;
                if (explosions_[i].row > 0) {
                    uint8_t adj = MatrixIndex(explosions_[i].row - 1, explosions_[i].col);
                    if (adj < frame.size()) {
                        frame[adj] = {
                            (uint8_t)(gc.r * wave_fade / 255),
                            (uint8_t)(gc.g * wave_fade / 255),
                            (uint8_t)(gc.b * wave_fade / 255)
                        };
                    }
                }
                if (explosions_[i].row < ROWS - 1) {
                    uint8_t adj = MatrixIndex(explosions_[i].row + 1, explosions_[i].col);
                    if (adj < frame.size()) {
                        frame[adj] = {
                            (uint8_t)(gc.r * wave_fade / 255),
                            (uint8_t)(gc.g * wave_fade / 255),
                            (uint8_t)(gc.b * wave_fade / 255)
                        };
                    }
                }
            }
        }
    }

    strip_->SetColors(frame);
}

void MatchGame::RenderIdle() {
    // HandleIdle 已直接操作LED
}

void MatchGame::RenderGameOver() {
    // HandleGameOver 已直接操作LED
}

void MatchGame::SetGridLed(uint8_t row, uint8_t col, const StripColor& color) {
    // 注意：此方法用于RenderIdle等直接操作场景
    // 对于RenderPlaying，使用批量frame方式更高效
    uint8_t led = MatrixIndex(row, col);
    strip_->SetSingleColor(led, color);
}

// ============================================================================
// HTTP/WS
// ============================================================================
std::string MatchGame::GetStatusJson() const {
    cJSON* root = cJSON_CreateObject();
    const char* state_str = "idle";
    if (state_ == GameState::PLAYING) state_str = "playing";
    else if (state_ == GameState::GAME_OVER) state_str = "game_over";

    cJSON_AddStringToObject(root, "state", state_str);
    cJSON_AddNumberToObject(root, "score", score_);
    cJSON_AddNumberToObject(root, "level", level_);
    cJSON_AddNumberToObject(root, "combo", combo_);
    cJSON_AddNumberToObject(root, "high_score", high_score_);
    cJSON_AddNumberToObject(root, "rows", ROWS);
    cJSON_AddNumberToObject(root, "cols", COLS);
    cJSON_AddNumberToObject(root, "led_count", LED_STRIP_LENGTH);
    int active_enemies = 0;
    for (int i = 0; i < enemy_count_; i++) {
        if (enemies_[i].active) active_enemies++;
    }
    cJSON_AddNumberToObject(root, "enemies", active_enemies);
    cJSON_AddBoolToObject(root, "http_server", http_server_.IsRunning());
    cJSON_AddBoolToObject(root, "ws_server", ws_server_.IsRunning());

    // 添加网格状态
    cJSON* grid = cJSON_CreateArray();
    for (int i = 0; i < enemy_count_; i++) {
        if (enemies_[i].active) {
            cJSON* e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "row", enemies_[i].row);
            cJSON_AddNumberToObject(e, "col", enemies_[i].col);
            cJSON_AddNumberToObject(e, "color", enemies_[i].color_index);
            cJSON_AddItemToArray(grid, e);
        }
    }
    cJSON_AddItemToObject(root, "grid", grid);

    cJSON* projectile = cJSON_CreateObject();
    cJSON_AddBoolToObject(projectile, "active", projectile_.active);
    cJSON_AddNumberToObject(projectile, "row", projectile_.row);
    cJSON_AddNumberToObject(projectile, "col", projectile_.col);
    cJSON_AddNumberToObject(projectile, "color", projectile_.color_index);
    cJSON_AddItemToObject(root, "projectile", projectile);

    cJSON* explosions = cJSON_CreateArray();
    for (int i = 0; i < MAX_EXPLOSIONS; i++) {
        if (explosions_[i].active) {
            cJSON* e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "row", explosions_[i].row);
            cJSON_AddNumberToObject(e, "col", explosions_[i].col);
            cJSON_AddNumberToObject(e, "color", explosions_[i].color_index);
            cJSON_AddNumberToObject(e, "frame", explosions_[i].frame);
            cJSON_AddItemToArray(explosions, e);
        }
    }
    cJSON_AddItemToObject(root, "explosions", explosions);

    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str);
    free(json_str);
    cJSON_Delete(root);
    return result;
}

void MatchGame::BroadcastState() {
    if (ws_server_.IsRunning()) {
        std::string json = GetStatusJson();
        ws_server_.BroadcastMessage(json);
    }
}

void MatchGame::Broadcast(const std::string& message) {
    ws_server_.BroadcastMessage(message);
}

bool MatchGame::HandleHttpRequest(const std::string& uri, const std::string& method,
                                   const std::string& body, std::string& response) {
    if (uri == "/api/status" && method == "GET") {
        response = GetStatusJson();
        return true;
    }

    if (uri == "/api/start" && method == "POST") {
        if (state_ == GameState::IDLE || state_ == GameState::GAME_OVER) {
            ResetGame();
            response = "{\"success\":true,\"message\":\"Game started\"}";
        } else {
            response = "{\"success\":false,\"message\":\"Game already running\"}";
        }
        return true;
    }

    if (uri == "/api/fire" && method == "POST") {
        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) {
            response = "{\"success\":false,\"error\":\"Invalid JSON\"}";
            return false;
        }
        cJSON* color = cJSON_GetObjectItem(root, "color");
        uint8_t c = 0;
        if (color && cJSON_IsNumber(color)) {
            c = color->valueint;
        }
        cJSON_Delete(root);

        OnButtonPressed(c);
        response = "{\"success\":true}";
        return true;
    }

    response = "{\"success\":false,\"error\":\"Unknown endpoint\"}";
    return false;
}

void MatchGame::HandleWsMessage(const WsMessage& msg) {
    if (msg.type == WsMessageType::FIND_COMPONENT) {
        // 复用FIND_COMPONENT类型作为"按颜色消除"命令
        cJSON* root = cJSON_Parse(msg.data.c_str());
        if (root) {
            cJSON* color = cJSON_GetObjectItem(root, "color");
            if (color && cJSON_IsNumber(color)) {
                OnButtonPressed(color->valueint);
            }
            cJSON_Delete(root);
        }
    } else if (msg.type == WsMessageType::GET_ALL) {
        BroadcastState();
    }
}
