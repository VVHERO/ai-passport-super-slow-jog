// main/ssj_logic.h —— 「超慢跑」核心状态机(纯逻辑,不依赖 ESP-IDF/LVGL,便于 host 测试)。
//
// 超慢跑(Super Slow Jogging)核心体验:固定步频节拍(默认 180 步/分)+ 计时。
// 本模块只负责"时间账本":累计活动时间、步数换算、间歇阶段(跑/歇)切换、
// 暂停/继续/完成判定。节拍发声、屏幕绘制由 ssj_audio / ssj_app 负责。
//
// 时间约定:所有时间用 int64 微秒(设备上来自 esp_timer_get_time(),host 测试
// 用虚构值),内部换算成毫秒对外,避免 32 位溢出(运动数小时也安全)。
//
// 线程约定:session 结构体不是线程安全的。设备上所有 tick/按键修改都发生在
// LVGL 任务(lv_timer 回调)或持有 bsp_lvgl_lock() 的输入任务里,天然串行。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
typedef enum {
    SSJ_MODE_FREE = 0,     // 自由跑:定时长,到点自动完成
    SSJ_MODE_INTERVAL = 1, // 间歇跑:跑 N 分 + 歇 M 分,重复 K 组
} ssj_mode_t;

// 参数取值范围。UI 的 OK 键在这些范围内循环递增,越界值一律由 ssj_config_clamp 收敛。
#define SSJ_CADENCE_MIN 120   // 步频下限(步/分)
#define SSJ_CADENCE_MAX 200   // 步频上限(步/分)。超慢跑推荐 180。
#define SSJ_FREE_MIN_MIN 1    // 自由跑时长下限(分)
#define SSJ_FREE_MIN_MAX 120  // 自由跑时长上限(分)
#define SSJ_CYCLES_MIN 1      // 间歇组数下限
#define SSJ_CYCLES_MAX 20     // 间歇组数上限
#define SSJ_RUN_MIN_MIN 1     // 单组跑时长下限(分)
#define SSJ_RUN_MIN_MAX 30    // 单组跑时长上限(分)
#define SSJ_REST_MIN_MIN 1    // 单组歇时长下限(分)
#define SSJ_REST_MIN_MAX 10   // 单组歇时长上限(分)

#define SSJ_MUSIC_CADENCE 180     // 伴跑音乐为 180 步/分:开音乐时步频固定(不再允许改)

// 屏幕随节拍闪烁的档位。0=关,1=柔(降到 45% 亮度),2=强(降到 8% 亮度)。
// 默认"柔":3 Hz 的整屏明暗在强档下连续跑十几分钟会晃眼,柔和档已经能看清拍点。
#define SSJ_FLASH_OFF 0
#define SSJ_FLASH_SOFT 1
#define SSJ_FLASH_STRONG 2
#define SSJ_FLASH_DEFAULT SSJ_FLASH_SOFT

#define SSJ_VOLUME_DEFAULT 60     // 出厂音量 60%(运行页 UP/DOWN 每次调 10%)

// 心率区间:按最大心率的百分比分档,决定运行页数字的颜色。
// 默认 190 ≈ 220 − 30 岁;用户可在设置页按自己的最大心率调整(步进 5)。
#define SSJ_HR_MAX_MIN 150
#define SSJ_HR_MAX_MAX 210
#define SSJ_HR_MAX_STEP 5
#define SSJ_HR_MAX_DEFAULT 190
#define SSJ_HR_ZONE_LOW 60    // < 60% 偏低(蓝)
#define SSJ_HR_ZONE_OK 70     // 60–70% 目标区(绿);70–80% 偏高(橙)
#define SSJ_HR_ZONE_HIGH 80   // >= 80% 高(红)

#define SSJ_CADENCE_DEFAULT 180   // 超慢跑经典步频
#define SSJ_FREE_MIN_DEFAULT 15   // 自由跑默认 15 分钟
#define SSJ_CYCLES_DEFAULT 5      // 间歇默认 5 组
#define SSJ_RUN_MIN_DEFAULT 3     // 间歇默认跑 3 分钟
#define SSJ_REST_MIN_DEFAULT 1    // 间歇默认歇 1 分钟

typedef struct {
    ssj_mode_t mode;   // 训练模式
    int cadence;       // 步频(步/分),[SSJ_CADENCE_MIN, SSJ_CADENCE_MAX]
    int free_min;      // 自由跑目标时长(分),[1,120]
    int cycles;        // 间歇组数,[1,20]
    int run_min;       // 单组跑时长(分),[1,30]
    int rest_min;      // 单组歇时长(分),[1,10]
    bool music;        // 伴跑音乐:开时本次会话步频固定为 SSJ_MUSIC_CADENCE
    uint8_t flash;     // 屏幕随拍闪烁档位:SSJ_FLASH_OFF / SOFT / STRONG
    uint8_t volume;    // 输出音量 0..100(%)
    bool hr_enabled;   // 从 BLE 心率传感器读实时心率(默认关,省电)
    int hr_max;        // 最大心率(用于分区间着色),[150,210]
} ssj_config_t;

// 本次会话实际生效的步频:开音乐时由音乐速度决定(180),否则用用户设置值。
int ssj_config_cadence(const ssj_config_t *cfg);

// 把任意配置收敛到合法范围(用于 NVS 读回旧/损坏数据时兜底)。
void ssj_config_clamp(ssj_config_t *cfg);

// 会话状态
typedef enum {
    SSJ_IDLE = 0,    // 未开始(主页)
    SSJ_RUNNING,     // 进行中(计时走动)
    SSJ_PAUSED,      // 用户暂停(时间冻结)
    SSJ_DONE,        // 达到目标自动完成
} ssj_state_t;

// 间歇阶段
typedef enum {
    SSJ_PHASE_RUN = 0,  // 跑步阶段
    SSJ_PHASE_REST = 1, // 休息阶段(仅间歇模式)
} ssj_phase_t;

// tick 事件位标志:ssj_session_tick() 返回,告知音频/UI 本 tick 发生了什么。
#define SSJ_EV_NONE 0x0
#define SSJ_EV_PHASE_REST 0x1  // 进入休息阶段(播放低音提示)
#define SSJ_EV_PHASE_RUN 0x2   // 进入跑步阶段(播放高音提示)
#define SSJ_EV_DONE 0x4        // 会话完成(播放结束音)

typedef struct {
    ssj_config_t cfg;         // 会话使用的配置(启动时从 UI 配置拷贝)
    ssj_state_t state;        // 当前状态
    ssj_phase_t phase;        // 间歇模式当前阶段;自由跑恒为 SSJ_PHASE_RUN
    int phase_cycle;          // 间歇模式当前第几组(0-based,最大 cycles-1)
    int64_t elapsed_us;       // 累计活动时间(不含暂停;微秒)
    int64_t phase_elapsed_us; // 当前阶段已进行时间(微秒;暂停时冻结)
    int64_t last_resume_us;   // 最近一次 start/resume 的时刻(微秒),暂停时无意义
} ssj_session_t;

// 初始化为 IDLE 会话(仅拷贝配置,不启动)。cfg 允许为 NULL(全零配置)。
void ssj_session_init(ssj_session_t *s, const ssj_config_t *cfg);

// IDLE/PAUSED/DONE -> RUNNING。从 now_us 开始走表。重复调用无害(重新对表)。
void ssj_session_start(ssj_session_t *s, int64_t now_us);

// RUNNING -> PAUSED:把 last_resume 到 now 的增量入账后冻结时间。
void ssj_session_pause(ssj_session_t *s, int64_t now_us);

// PAUSED -> RUNNING。从 now_us 继续走表。
void ssj_session_resume(ssj_session_t *s, int64_t now_us);

// 周期驱动(设备上 100ms 一次):推进累计时间,处理阶段切换与完成判定。
// 返回 SSJ_EV_* 位标志(可能同时含 PHASE 与 DONE)。非 RUNNING 状态返回 SSJ_EV_NONE。
int ssj_session_tick(ssj_session_t *s, int64_t now_us);

// 用户在 RUNNING/PAUSED 主动结束:直接置 DONE(完成页显示实际成绩)。
void ssj_session_finish(ssj_session_t *s);

// 累计活动时间(毫秒,向下取整)。任意状态可读。
int64_t ssj_session_elapsed_ms(const ssj_session_t *s);

// 累计步数 = 活动时间 x 步频,向下取整。任意状态可读。
int64_t ssj_session_steps(const ssj_session_t *s);

// 会话总目标时长(微秒)。自由跑 = free_min;间歇 = 组数x(跑+歇) - 最后一组不歇。
int64_t ssj_session_target_us(const ssj_session_t *s);

// 间歇模式当前阶段剩余时间(毫秒)。自由跑返回目标减已跑(到点即 DONE)。
int64_t ssj_session_phase_remaining_ms(const ssj_session_t *s);

// 会话是否已达成目标(elapsed >= target)。用于完成页区分"跑完了/提前结束"。
bool ssj_session_target_reached(const ssj_session_t *s);
