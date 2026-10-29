// main/ssj_logic.c —— 「超慢跑」核心状态机实现。全部为纯整数运算,可在 host 上测试。
// 设计要点见 ssj_logic.h 头注释。
#include "ssj_logic.h"

#include <stddef.h>

#define US_PER_MS 1000LL
#define US_PER_SEC (1000LL * US_PER_MS)
#define US_PER_MIN (60LL * US_PER_SEC)

// ---------------------------------------------------------------------------
// 配置收敛
// ---------------------------------------------------------------------------
static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void ssj_config_clamp(ssj_config_t *cfg) {
    if (!cfg) return;
    // mode 越界(如 NVS 损坏)回退到自由跑;其余字段夹到各自合法区间。
    if (cfg->mode != SSJ_MODE_FREE && cfg->mode != SSJ_MODE_INTERVAL) {
        cfg->mode = SSJ_MODE_FREE;
    }
    cfg->cadence = clampi(cfg->cadence, SSJ_CADENCE_MIN, SSJ_CADENCE_MAX);
    cfg->free_min = clampi(cfg->free_min, SSJ_FREE_MIN_MIN, SSJ_FREE_MIN_MAX);
    cfg->cycles = clampi(cfg->cycles, SSJ_CYCLES_MIN, SSJ_CYCLES_MAX);
    cfg->run_min = clampi(cfg->run_min, SSJ_RUN_MIN_MIN, SSJ_RUN_MIN_MAX);
    cfg->rest_min = clampi(cfg->rest_min, SSJ_REST_MIN_MIN, SSJ_REST_MIN_MAX);
}

// ---------------------------------------------------------------------------
// 会话生命周期
// ---------------------------------------------------------------------------
void ssj_session_init(ssj_session_t *s, const ssj_config_t *cfg) {
    if (!s) return;
    // 全零兜底:调用方传 NULL 或漏字段时也保持一个合法 IDLE 会话。
    ssj_config_t safe = {0};
    safe.mode = SSJ_MODE_FREE;
    safe.cadence = SSJ_CADENCE_DEFAULT;
    safe.free_min = SSJ_FREE_MIN_DEFAULT;
    safe.cycles = SSJ_CYCLES_DEFAULT;
    safe.run_min = SSJ_RUN_MIN_DEFAULT;
    safe.rest_min = SSJ_REST_MIN_DEFAULT;
    if (cfg) safe = *cfg;
    ssj_config_clamp(&safe);

    s->cfg = safe;
    s->state = SSJ_IDLE;
    s->phase = SSJ_PHASE_RUN;
    s->phase_cycle = 0;
    s->elapsed_us = 0;
    s->phase_elapsed_us = 0;
    s->last_resume_us = 0;
}

void ssj_session_start(ssj_session_t *s, int64_t now_us) {
    if (!s) return;
    // IDLE 之外重复 start 视为"重新对表":不清累计时间,只重置暂停挂起点,
    // 保证 PAUSED 误触 start 不会把冻结期间算进运动时间。
    s->last_resume_us = now_us;
    s->state = SSJ_RUNNING;
    if (s->elapsed_us == 0 && s->phase_elapsed_us == 0) {
        s->phase = SSJ_PHASE_RUN;   // 全新会话从跑步阶段开始
        s->phase_cycle = 0;
    }
}

void ssj_session_pause(ssj_session_t *s, int64_t now_us) {
    if (!s || s->state != SSJ_RUNNING) return;
    // 先把 last_resume 以来走过的时间入账,再冻结。
    s->elapsed_us += now_us - s->last_resume_us;
    s->phase_elapsed_us += now_us - s->last_resume_us;
    s->state = SSJ_PAUSED;
}

void ssj_session_resume(ssj_session_t *s, int64_t now_us) {
    if (!s || s->state != SSJ_PAUSED) return;
    s->last_resume_us = now_us;
    s->state = SSJ_RUNNING;
}

void ssj_session_finish(ssj_session_t *s) {
    if (!s) return;
    // RUNNING 时最后一次 tick 后的残余时间(<=100ms)不入账:秒级显示下无感,
    // 且避免在本模块引入第二个时间源。完成页显示前必有一次 tick。
    s->state = SSJ_DONE;
}

// 当前阶段的总时长(微秒)。仅间歇模式有"阶段"概念;自由跑按整体目标处理。
static int64_t phase_duration_us(const ssj_session_t *s) {
    return (s->phase == SSJ_PHASE_RUN ? (int64_t)s->cfg.run_min : (int64_t)s->cfg.rest_min)
           * US_PER_MIN;
}

int ssj_session_tick(ssj_session_t *s, int64_t now_us) {
    if (!s || s->state != SSJ_RUNNING) return SSJ_EV_NONE;

    // now_us 可能早于 last_resume(测试构造负增量/时钟回拨):防御性忽略负增量。
    int64_t delta = now_us - s->last_resume_us;
    if (delta < 0) delta = 0;
    s->last_resume_us = now_us;
    s->elapsed_us += delta;
    s->phase_elapsed_us += delta;

    int events = SSJ_EV_NONE;

    if (s->cfg.mode == SSJ_MODE_FREE) {
        // 自由跑:整体到点即完成。
        if (s->elapsed_us >= (int64_t)s->cfg.free_min * US_PER_MIN) {
            s->elapsed_us = (int64_t)s->cfg.free_min * US_PER_MIN;  // 不超出目标
            s->phase_elapsed_us = s->elapsed_us;
            s->state = SSJ_DONE;
            events |= SSJ_EV_DONE;
        }
        return events;
    }

    // 间歇模式:用 while 处理"一次 tick 跨过整个短阶段"的情况
    // (极端:阶段 1 分钟而 UI 卡了 2 分钟没 tick,也要正确连跳)。
    for (;;) {
        const int64_t dur = phase_duration_us(s);
        if (s->phase_elapsed_us < dur) break;

        s->phase_elapsed_us -= dur;

        if (s->phase == SSJ_PHASE_RUN) {
            // 本组跑完。若是最后一组,直接完成(最后一组之后不安排休息)。
            if (s->phase_cycle >= s->cfg.cycles - 1) {
                s->state = SSJ_DONE;
                s->phase_elapsed_us = 0;
                events |= SSJ_EV_DONE;
                break;
            }
            s->phase = SSJ_PHASE_REST;
            events |= SSJ_EV_PHASE_REST;
        } else {
            // 休息结束,进入下一组跑步。
            s->phase = SSJ_PHASE_RUN;
            s->phase_cycle++;
            events |= SSJ_EV_PHASE_RUN;
        }
    }

    // 完成后清零阶段表,让 UI 显示整齐的 00:00。
    if (s->state == SSJ_DONE) s->phase_elapsed_us = 0;
    return events;
}

// ---------------------------------------------------------------------------
// 只读查询
// ---------------------------------------------------------------------------
int64_t ssj_session_elapsed_ms(const ssj_session_t *s) {
    if (!s) return 0;
    // RUNNING 时返回账面值:最后一次 tick 后的残余时间(<=100ms)下次 tick 入账,
    // 对秒级显示无影响,且保持本模块无第二个时间源。
    return s->elapsed_us / US_PER_MS;
}

int64_t ssj_session_steps(const ssj_session_t *s) {
    if (!s) return 0;
    // 步数 = 活动毫秒 x 步频 / 60000,向下取整。
    return ssj_session_elapsed_ms(s) * (int64_t)s->cfg.cadence / 60000LL;
}

int64_t ssj_session_target_us(const ssj_session_t *s) {
    if (!s) return 0;
    if (s->cfg.mode == SSJ_MODE_FREE) {
        return (int64_t)s->cfg.free_min * US_PER_MIN;
    }
    // 间歇:每组 = 跑 + 歇,最后一组没有休息。
    int64_t per_cycle = ((int64_t)s->cfg.run_min + (int64_t)s->cfg.rest_min) * US_PER_MIN;
    return per_cycle * (int64_t)s->cfg.cycles - (int64_t)s->cfg.rest_min * US_PER_MIN;
}

int64_t ssj_session_phase_remaining_ms(const ssj_session_t *s) {
    if (!s) return 0;
    if (s->state == SSJ_DONE) return 0;
    if (s->cfg.mode == SSJ_MODE_FREE) {
        int64_t rem = (int64_t)s->cfg.free_min * US_PER_MIN - s->elapsed_us;
        return rem > 0 ? rem / US_PER_MS : 0;
    }
    int64_t rem = phase_duration_us(s) - s->phase_elapsed_us;
    return rem > 0 ? rem / US_PER_MS : 0;
}

bool ssj_session_target_reached(const ssj_session_t *s) {
    if (!s) return false;
    return s->elapsed_us >= ssj_session_target_us(s);
}
