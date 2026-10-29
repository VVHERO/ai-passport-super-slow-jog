// tests/test_ssj_logic.c —— 「超慢跑」状态机 host 测试(无 ESP-IDF/LVGL 依赖)。
// 编译: cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_ssj_logic.c main/ssj_logic.c
#include "ssj_logic.h"

#include <stdio.h>

static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define US_PER_MIN (60LL * 1000 * 1000)
#define US_PER_SEC (1000LL * 1000)

static ssj_config_t free_cfg(void) {
    ssj_config_t c = {0};
    c.mode = SSJ_MODE_FREE;
    c.cadence = 180;
    c.free_min = 15;
    c.cycles = 5;
    c.run_min = 3;
    c.rest_min = 1;
    return c;
}

static void test_config_clamp(void) {
    ssj_config_t c = {0};
    c.mode = (ssj_mode_t)99;   // 非法模式
    c.cadence = 10;
    c.free_min = 0;
    c.cycles = 1000;
    c.run_min = -5;
    c.rest_min = 99;
    ssj_config_clamp(&c);
    CHECK(c.mode == SSJ_MODE_FREE);
    CHECK(c.cadence == SSJ_CADENCE_MIN);
    CHECK(c.free_min == SSJ_FREE_MIN_MIN);
    CHECK(c.cycles == SSJ_CYCLES_MAX);
    CHECK(c.run_min == SSJ_RUN_MIN_MIN);
    CHECK(c.rest_min == SSJ_REST_MIN_MAX);

    // 合法值应原样通过
    ssj_config_t good = free_cfg();
    ssj_config_t before = good;
    ssj_config_clamp(&good);
    CHECK(good.mode == before.mode && good.cadence == before.cadence &&
          good.free_min == before.free_min && good.cycles == before.cycles &&
          good.run_min == before.run_min && good.rest_min == before.rest_min);
}

static void test_free_mode_countup_and_done(void) {
    ssj_session_t s;
    ssj_config_t c = free_cfg();
    c.free_min = 2;   // 缩短目标便于测试
    ssj_session_init(&s, &c);
    CHECK(s.state == SSJ_IDLE);

    ssj_session_start(&s, 1000 * US_PER_SEC);
    CHECK(s.state == SSJ_RUNNING);

    // 跑 59 秒:未到点,无事件
    CHECK(ssj_session_tick(&s, 1000 * US_PER_SEC + 59 * US_PER_SEC) == SSJ_EV_NONE);
    CHECK(ssj_session_elapsed_ms(&s) == 59000);
    CHECK(ssj_session_steps(&s) == 59000LL * 180 / 60000);  // 177 步

    // 跑到 2 分钟整:完成,时间收敛到目标,不超跑
    int ev = ssj_session_tick(&s, 1000 * US_PER_SEC + 2 * US_PER_MIN + 500 * 1000);
    CHECK(ev & SSJ_EV_DONE);
    CHECK(s.state == SSJ_DONE);
    CHECK(ssj_session_elapsed_ms(&s) == 120000);
    CHECK(ssj_session_phase_remaining_ms(&s) == 0);
}

static void test_pause_resume_accounting(void) {
    ssj_session_t s;
    ssj_config_t c = free_cfg();
    c.free_min = 10;
    ssj_session_init(&s, &c);

    ssj_session_start(&s, 0);
    ssj_session_tick(&s, 30 * US_PER_SEC);
    ssj_session_pause(&s, 40 * US_PER_SEC);   // 跑 40s 后暂停
    CHECK(s.state == SSJ_PAUSED);
    CHECK(ssj_session_elapsed_ms(&s) == 40000);

    // 暂停期间时间冻结(暂停 1 小时也不入账)
    ssj_session_tick(&s, 40 * US_PER_SEC + 3600 * US_PER_SEC);
    CHECK(ssj_session_elapsed_ms(&s) == 40000);
    CHECK(ssj_session_steps(&s) == 40000LL * 180 / 60000);

    // 恢复后继续累计
    ssj_session_resume(&s, 40 * US_PER_SEC + 3600 * US_PER_SEC);
    CHECK(s.state == SSJ_RUNNING);
    ssj_session_tick(&s, 40 * US_PER_SEC + 3600 * US_PER_SEC + 20 * US_PER_SEC);
    CHECK(ssj_session_elapsed_ms(&s) == 60000);

    // PAUSED 状态误触 start 不丢账、不把冻结期算进去
    // (此刻累计 65s:start(999) 只重挂时间起点,tick 到 1004s 再入账 5s -> 70s)
    ssj_session_pause(&s, 3665 * US_PER_SEC);
    ssj_session_start(&s, 999 * US_PER_SEC);
    CHECK(s.state == SSJ_RUNNING);
    ssj_session_tick(&s, 999 * US_PER_SEC + 5 * US_PER_SEC);
    CHECK(ssj_session_elapsed_ms(&s) == 70000);
}

static void test_interval_phase_transitions(void) {
    ssj_session_t s;
    ssj_config_t c = free_cfg();
    c.mode = SSJ_MODE_INTERVAL;
    c.cycles = 2;
    c.run_min = 1;
    c.rest_min = 1;
    ssj_session_init(&s, &c);

    // 目标 = 2 组 x (1+1) 分 - 最后不歇 = 3 分钟
    CHECK(ssj_session_target_us(&s) == 3 * US_PER_MIN);

    ssj_session_start(&s, 0);
    CHECK(s.phase == SSJ_PHASE_RUN && s.phase_cycle == 0);

    // 59.9 秒:仍在第一组跑步
    int ev = ssj_session_tick(&s, 59900 * 1000);
    CHECK(ev == SSJ_EV_NONE);

    // 61 秒:已切入休息
    ev = ssj_session_tick(&s, 61 * US_PER_SEC);
    CHECK(ev & SSJ_EV_PHASE_REST);
    CHECK(s.phase == SSJ_PHASE_REST && s.phase_cycle == 0);

    // 121 秒:休息结束,进入第二组跑步
    ev = ssj_session_tick(&s, 121 * US_PER_SEC);
    CHECK(ev & SSJ_EV_PHASE_RUN);
    CHECK(s.phase == SSJ_PHASE_RUN && s.phase_cycle == 1);

    // 181 秒:最后一组跑完 -> 完成(不再进入休息)
    ev = ssj_session_tick(&s, 181 * US_PER_SEC);
    CHECK(ev & SSJ_EV_DONE);
    CHECK(s.state == SSJ_DONE);
    CHECK(ssj_session_elapsed_ms(&s) == 181000);
    CHECK(ssj_session_phase_remaining_ms(&s) == 0);
    CHECK(ssj_session_target_reached(&s));
}

static void test_interval_single_tick_spanning_phases(void) {
    // 极端情况:一次 tick 跨过整个短阶段,也要正确连跳(阶段时长 1 分,tick 间隔 5 分)。
    ssj_session_t s;
    ssj_config_t c = free_cfg();
    c.mode = SSJ_MODE_INTERVAL;
    c.cycles = 1;
    c.run_min = 1;
    c.rest_min = 1;
    ssj_session_init(&s, &c);

    ssj_session_start(&s, 0);
    int ev = ssj_session_tick(&s, 5 * US_PER_MIN);
    // 1 组:跑 1 分即完成,不进入休息
    CHECK(ev & SSJ_EV_DONE);
    CHECK(s.state == SSJ_DONE);

    // 2 组配置:一次 tick 应穿过 跑->歇->跑->歇->跑->完成
    ssj_config_t c2 = c;
    c2.cycles = 2;
    ssj_session_init(&s, &c2);
    ssj_session_start(&s, 0);
    ev = ssj_session_tick(&s, 5 * US_PER_MIN);
    CHECK(ev & SSJ_EV_DONE);
    CHECK(s.state == SSJ_DONE);
}

static void test_finish_and_steps(void) {
    ssj_session_t s;
    ssj_config_t c = free_cfg();
    c.cadence = 150;
    ssj_session_init(&s, &c);
    ssj_session_start(&s, 0);
    ssj_session_tick(&s, 60 * US_PER_SEC);  // 1 分钟
    CHECK(ssj_session_steps(&s) == 150);

    ssj_session_finish(&s);   // 用户提前结束
    CHECK(s.state == SSJ_DONE);
    CHECK(!ssj_session_target_reached(&s));   // 未达目标
    CHECK(ssj_session_steps(&s) == 150);      // 成绩保留
}

static void test_null_and_idle_safety(void) {
    ssj_session_t s;
    ssj_session_init(&s, NULL);   // NULL 配置 -> 默认值
    CHECK(s.state == SSJ_IDLE);
    CHECK(s.cfg.cadence == SSJ_CADENCE_DEFAULT);
    CHECK(ssj_session_tick(&s, 1000) == SSJ_EV_NONE);   // IDLE 不走表
    CHECK(ssj_session_elapsed_ms(&s) == 0);
    CHECK(ssj_session_steps(&s) == 0);

    ssj_session_init(NULL, NULL);   // 不崩溃
    ssj_session_tick(NULL, 0);
    CHECK(ssj_session_steps(NULL) == 0);
}

int main(void) {
    test_config_clamp();
    test_free_mode_countup_and_done();
    test_pause_resume_accounting();
    test_interval_phase_transitions();
    test_interval_single_tick_spanning_phases();
    test_finish_and_steps();
    test_null_and_idle_safety();

    if (g_failures) {
        printf("ssj_logic: %d FAILURES\n", g_failures);
        return 1;
    }
    printf("SSJ logic state-machine tests: PASS\n");
    return 0;
}
