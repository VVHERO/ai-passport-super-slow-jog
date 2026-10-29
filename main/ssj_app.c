// main/ssj_app.c —— 超慢跑应用:页面、按键与会话编排。
//
// 页面结构(全部为本应用重新设计,不复用 demo/ui_pixel 外壳):
//   HOME  主页:参数列表(模式/步频/时长或间歇参数) + 开始;UP/DOWN 选行,OK 改值。
//   RUN   运行页:阶段名 + 阶段倒计时(间歇) + 大字总时长 + 步数 + 节拍脉冲点。
//   DONE  完成页:本次成绩(时长/步数),OK 返回主页。
//
// 并发模型:
//   - 所有 LVGL 对象操作都在 LVGL 任务上下文(lv_timer 回调)或持有
//     bsp_lvgl_lock() 的输入任务里,单线程访问,无锁竞争。
//   - 会话计时由 lv_timer(100ms)驱动 ssj_session_tick();音频走独立任务,
//     通过 ssj_audio_* 命令队列通信,互不触碰对方数据。
//   - 电量经 bsp_battery_soc() 每 1s 读取一次(读值 -1 时隐藏电池标签)。
#include "ssj_app.h"

#include "ssj_audio.h"
#include "ssj_logic.h"
#include "ssj_store.h"

#include "bsp_battery.h"
#include "bsp_display.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ssj_app";

// ---------------------------------------------------------------------------
// 视觉令牌(本应用独立配色:深底 + 运动绿/休息蓝,与 demo 主题无关)
// ---------------------------------------------------------------------------
#define SSJ_COL_BG 0x0E1526       // 页面背景:深夜蓝
#define SSJ_COL_PANEL 0x1A2436    // 行面板底
#define SSJ_COL_PANEL_SEL 0x1F4D3D// 选中行底:深绿
#define SSJ_COL_TEXT 0xF2F5F7     // 主文字
#define SSJ_COL_SUB 0x8A97A8      // 次级文字
#define SSJ_COL_ACCENT 0x4CC38A   // 强调/跑步:运动绿
#define SSJ_COL_REST 0x4C9AFF     // 休息:蓝
#define SSJ_COL_WARN 0xFFB454     // 暂停:暖橙

// 字体(assets/fonts 生成的子集,见 lvgl-chinese-fonts 指南)。
LV_FONT_DECLARE(ssj_font_20);   // 正文:中文 + ASCII
LV_FONT_DECLARE(ssj_font_48);   // 大字计时:数字/冒号/百分号

// 主页行高与布局(240x320,四角 30px 圆角遮罩,避开边角)。
#define SSJ_ROW_H 36
#define SSJ_ROW_GAP 4
#define SSJ_ROW_X 12
#define SSJ_ROW_W 216
#define SSJ_ROW_Y0 46
#define SSJ_MAX_ROWS 6   // 间歇模式:模式/步频/组数/跑/歇/开始

// ---------------------------------------------------------------------------
// 应用状态(全部仅在 LVGL 上下文访问)
// ---------------------------------------------------------------------------
typedef enum {
    SSJ_PAGE_HOME = 0,
    SSJ_PAGE_RUN,
    SSJ_PAGE_DONE,
} ssj_page_t;

static ssj_page_t s_page;
static ssj_config_t s_cfg;          // 主页正在编辑的配置
static ssj_session_t s_sess;        // 运行会话
static uint8_t s_volume = 60;       // 音量 0..100(运行页 UP/DOWN 调节)
static int s_home_sel;              // 主页选中行索引

// 主页对象
static lv_obj_t *s_home_scr;
static lv_obj_t *s_rows[SSJ_MAX_ROWS];    // 每行面板
static lv_obj_t *s_row_labels[SSJ_MAX_ROWS]; // 每行 "名称  值" 标签
static int s_row_count;
static lv_obj_t *s_battery_home;

// 运行页对象
static lv_obj_t *s_run_scr;
static lv_obj_t *s_phase_label;     // 阶段名(跑步中/休息中/已暂停/自由跑)
static lv_obj_t *s_phase_left;      // 阶段剩余时间(间歇);自由跑隐藏
static lv_obj_t *s_time_label;      // 大字总时长 MM:SS
static lv_obj_t *s_steps_label;     // 步数
static lv_obj_t *s_beat_dot;        // 节拍脉冲点
static lv_obj_t *s_hint_label;      // 底部按键提示
static lv_obj_t *s_toast;           // 音量提示浮层
static lv_timer_t *s_toast_timer;   // 浮层自动隐藏
static lv_obj_t *s_battery_run;

// 完成页对象
static lv_obj_t *s_done_scr;
static lv_obj_t *s_done_title;
static lv_obj_t *s_done_stats;
static lv_obj_t *s_battery_done;

// lv_timer 句柄(应用主循环,页面切换时不销毁)
static lv_timer_t *s_tick_timer;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
// 秒 -> "MM:SS"(超 1 小时显示 HHH:MM 不现实,运动上限 120 分钟,够用)。
static void fmt_mmss(char *buf, size_t len, int64_t total_ms) {
    int total_s = (int)(total_ms / 1000);
    lv_snprintf(buf, (uint32_t)len, "%02d:%02d", total_s / 60, total_s % 60);
}

// 创建一个统一风格的标签并挂到 parent。
static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    return l;
}

// 右上角电池标签(运行时由 tick 刷新;-1 不可用时隐藏,优雅降级)。
static lv_obj_t *make_battery_label(lv_obj_t *scr) {
    lv_obj_t *l = make_label(scr, &ssj_font_20, SSJ_COL_SUB);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -10, 6);
    lv_label_set_text(l, "");
    return l;
}

static void refresh_battery(lv_obj_t *label) {
    if (!label) return;
    int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(label, "%d%%", soc);
}

// ---------------------------------------------------------------------------
// 主页
// ---------------------------------------------------------------------------
// 返回行标题;value_buf 写入当前值文本;返回 false = 该行是"开始"动作行。
static bool home_row_content(int idx, char *name, size_t nlen, char *value, size_t vlen) {
    // 行序按模式动态排列:0=模式 1=步频 之后模式专属参数,最后是"开始"。
    if (idx == 0) {
        lv_snprintf(name, (uint32_t)nlen, "模式");
        lv_snprintf(value, (uint32_t)vlen, "%s",
                    s_cfg.mode == SSJ_MODE_FREE ? "自由跑" : "间歇跑");
        return true;
    }
    if (idx == 1) {
        lv_snprintf(name, (uint32_t)nlen, "步频");
        lv_snprintf(value, (uint32_t)vlen, "%d 步/分", s_cfg.cadence);
        return true;
    }
    const bool interval = (s_cfg.mode == SSJ_MODE_INTERVAL);
    const int param_rows = interval ? 3 : 1;   // 间歇:组数/跑/歇;自由:时长
    const int last = 2 + param_rows;           // "开始"行索引
    if (idx == last) {
        lv_snprintf(name, (uint32_t)nlen, "开始");
        lv_snprintf(value, (uint32_t)vlen, "OK");
        return false;   // 动作行
    }
    if (!interval) {
        lv_snprintf(name, (uint32_t)nlen, "时长");
        lv_snprintf(value, (uint32_t)vlen, "%d 分", s_cfg.free_min);
        return true;
    }
    if (idx == 2) {
        lv_snprintf(name, (uint32_t)nlen, "组数");
        lv_snprintf(value, (uint32_t)vlen, "%d 组", s_cfg.cycles);
    } else if (idx == 3) {
        lv_snprintf(name, (uint32_t)nlen, "跑");
        lv_snprintf(value, (uint32_t)vlen, "%d 分", s_cfg.run_min);
    } else {
        lv_snprintf(name, (uint32_t)nlen, "歇");
        lv_snprintf(value, (uint32_t)vlen, "%d 分", s_cfg.rest_min);
    }
    return true;
}

// 选中行高亮重绘。
static void home_refresh(void) {
    char name[16], value[32], text[64];
    for (int i = 0; i < s_row_count; i++) {
        bool is_param = home_row_content(i, name, sizeof(name), value, sizeof(value));
        lv_snprintf(text, sizeof(text), is_param ? "%s  %s" : "%s  > %s", name, value);
        lv_label_set_text(s_row_labels[i], text);
        lv_obj_set_style_bg_color(s_rows[i],
            i == s_home_sel ? lv_color_hex(SSJ_COL_PANEL_SEL) : lv_color_hex(SSJ_COL_PANEL), 0);
        lv_obj_set_style_text_color(s_row_labels[i],
            i == s_home_sel ? lv_color_hex(SSJ_COL_ACCENT) : lv_color_hex(SSJ_COL_TEXT), 0);
    }
    refresh_battery(s_battery_home);
}

// 按当前模式重建主页行(模式切换会改变行数)。
static void home_build(void) {
    if (s_home_scr) {
        lv_obj_delete(s_home_scr);
        s_home_scr = NULL;
    }
    s_row_count = (s_cfg.mode == SSJ_MODE_INTERVAL) ? 6 : 4;
    if (s_home_sel >= s_row_count) s_home_sel = 0;

    s_home_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_home_scr, lv_color_hex(SSJ_COL_BG), 0);
    lv_obj_set_style_border_width(s_home_scr, 0, 0);

    // 标题
    lv_obj_t *title = make_label(s_home_scr, &ssj_font_20, SSJ_COL_ACCENT);
    lv_label_set_text(title, "超慢跑");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    s_battery_home = make_battery_label(s_home_scr);

    // 参数行
    for (int i = 0; i < s_row_count; i++) {
        lv_obj_t *panel = lv_obj_create(s_home_scr);
        lv_obj_remove_style_all(panel);
        lv_obj_set_size(panel, SSJ_ROW_W, SSJ_ROW_H);
        lv_obj_set_pos(panel, SSJ_ROW_X, SSJ_ROW_Y0 + i * (SSJ_ROW_H + SSJ_ROW_GAP));
        lv_obj_set_style_bg_color(panel, lv_color_hex(SSJ_COL_PANEL), 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(panel, 8, 0);

        lv_obj_t *l = make_label(panel, &ssj_font_20, SSJ_COL_TEXT);
        lv_obj_center(l);
        s_rows[i] = panel;
        s_row_labels[i] = l;
    }

    // 底部按键提示
    lv_obj_t *hint = make_label(s_home_scr, &ssj_font_20, SSJ_COL_SUB);
    lv_label_set_text(hint, "上/下 选择  OK 调节");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    home_refresh();
    lv_screen_load(s_home_scr);
}

// OK 在选中行上:参数行循环+1;动作行启动会话。
static void home_activate(void) {
    if (s_home_sel == s_row_count - 1) {   // "开始"行
        ssj_session_init(&s_sess, &s_cfg);
        ssj_session_start(&s_sess, esp_timer_get_time());
        ssj_store_save(&s_cfg);   // 保存失败不影响本次会话
        run_page_build();
        // 节拍器启动:间隔 = 60000/步频 毫秒,初始音高跑步。
        ssj_audio_metronome(60000u / (uint32_t)s_cfg.cadence, true);
        ssj_audio_tick_pitch(false);
        s_page = SSJ_PAGE_RUN;
        return;
    }

    if (s_home_sel == 0) {   // 模式循环
        s_cfg.mode = (s_cfg.mode == SSJ_MODE_FREE) ? SSJ_MODE_INTERVAL : SSJ_MODE_FREE;
        home_build();   // 行数变化,重建
        return;
    }
    if (s_home_sel == 1) {   // 步频 +2 循环
        s_cfg.cadence += 2;
        if (s_cfg.cadence > SSJ_CADENCE_MAX) s_cfg.cadence = SSJ_CADENCE_MIN;
    } else if (s_cfg.mode == SSJ_MODE_FREE) {
        s_cfg.free_min++;   // 时长 +1 循环
        if (s_cfg.free_min > SSJ_FREE_MIN_MAX) s_cfg.free_min = SSJ_FREE_MIN_MIN;
    } else if (s_home_sel == 2) {
        s_cfg.cycles++;
        if (s_cfg.cycles > SSJ_CYCLES_MAX) s_cfg.cycles = SSJ_CYCLES_MIN;
    } else if (s_home_sel == 3) {
        s_cfg.run_min++;
        if (s_cfg.run_min > SSJ_RUN_MIN_MAX) s_cfg.run_min = SSJ_RUN_MIN_MIN;
    } else {
        s_cfg.rest_min++;
        if (s_cfg.rest_min > SSJ_REST_MIN_MAX) s_cfg.rest_min = SSJ_REST_MIN_MIN;
    }
    home_refresh();
}

// ---------------------------------------------------------------------------
// 运行页
// ---------------------------------------------------------------------------
// 节拍脉冲动画:点的不透明度 255 -> 70 -> 255,一个来回 = 两个节拍间隔。
static void beat_anim_start(void) {
    if (!s_beat_dot) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_beat_dot);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, 70);
    uint32_t period_ms = 60000u / (uint32_t)s_sess.cfg.cadence;
    lv_anim_set_duration(&a, period_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_duration(&a, period_ms);
    lv_anim_start(&a);
}

static void beat_anim_stop(void) {
    lv_anim_delete(s_beat_dot, NULL);
    if (s_beat_dot) lv_obj_set_style_opa(s_beat_dot, LV_OPA_COVER, 0);
}

// 音量提示浮层:1.2s 后自动隐藏。
static void toast_timer_cb(lv_timer_t *t) {
    if (s_toast) lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(t);
    s_toast_timer = NULL;
}

static void toast_show(const char *text) {
    if (!s_toast) return;
    lv_label_set_text(s_toast, text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer) lv_timer_del(s_toast_timer);
    s_toast_timer = lv_timer_create(toast_timer_cb, 1200, NULL);
}

void run_page_build(void) {
    if (s_run_scr) lv_obj_delete(s_run_scr);
    s_run_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_run_scr, lv_color_hex(SSJ_COL_BG), 0);
    lv_obj_set_style_border_width(s_run_scr, 0, 0);

    s_battery_run = make_battery_label(s_run_scr);

    // 阶段名
    s_phase_label = make_label(s_run_scr, &ssj_font_20, SSJ_COL_ACCENT);
    lv_obj_align(s_phase_label, LV_ALIGN_TOP_MID, 0, 34);

    // 阶段剩余(仅间歇模式可见)
    s_phase_left = make_label(s_run_scr, &ssj_font_20, SSJ_COL_SUB);
    lv_obj_align(s_phase_left, LV_ALIGN_TOP_MID, 0, 62);

    // 大字总时长
    s_time_label = make_label(s_run_scr, &ssj_font_48, SSJ_COL_TEXT);
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -30);

    // 步数
    s_steps_label = make_label(s_run_scr, &ssj_font_20, SSJ_COL_SUB);
    lv_obj_align(s_steps_label, LV_ALIGN_CENTER, 0, 40);

    // 节拍脉冲点
    s_beat_dot = lv_obj_create(s_run_scr);
    lv_obj_remove_style_all(s_beat_dot);
    lv_obj_set_size(s_beat_dot, 18, 18);
    lv_obj_align(s_beat_dot, LV_ALIGN_CENTER, 0, 90);
    lv_obj_set_style_radius(s_beat_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_beat_dot, lv_color_hex(SSJ_COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_beat_dot, LV_OPA_COVER, 0);
    beat_anim_start();

    // 底部提示
    s_hint_label = make_label(s_run_scr, &ssj_font_20, SSJ_COL_SUB);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -12);

    // 音量浮层
    s_toast = make_label(s_run_scr, &ssj_font_20, SSJ_COL_TEXT);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(SSJ_COL_PANEL), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_toast, 8, 0);
    lv_obj_set_style_radius(s_toast, 8, 0);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    lv_screen_load(s_run_scr);
    run_page_refresh(true);
}

// 运行页重绘。force=true 时全部刷新;否则只在秒变化时更新时间/步数,省 SPI 带宽。
static void run_page_refresh(bool force) {
    if (!s_run_scr) return;

    static int64_t last_shown_ms = -1;
    const bool paused = (s_sess.state == SSJ_PAUSED);
    const bool interval = (s_sess.cfg.mode == SSJ_MODE_INTERVAL);

    // 阶段名与配色
    const char *phase_text;
    uint32_t phase_color;
    if (paused) {
        phase_text = "已暂停";
        phase_color = SSJ_COL_WARN;
    } else if (interval && s_sess.phase == SSJ_PHASE_REST) {
        phase_text = "休息中";
        phase_color = SSJ_COL_REST;
    } else if (interval) {
        phase_text = "跑步中";
        phase_color = SSJ_COL_ACCENT;
    } else {
        phase_text = "自由跑";
        phase_color = SSJ_COL_ACCENT;
    }
    lv_label_set_text(s_phase_label, phase_text);
    lv_obj_set_style_text_color(s_phase_label, lv_color_hex(phase_color), 0);

    // 阶段剩余
    if (interval) {
        char buf[16];
        fmt_mmss(buf, sizeof(buf), ssj_session_phase_remaining_ms(&s_sess));
        lv_label_set_text(s_phase_left, buf);
        lv_obj_remove_flag(s_phase_left, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_phase_left, LV_OBJ_FLAG_HIDDEN);
    }

    // 时间与步数:秒粒度,变了才刷
    int64_t ms = ssj_session_elapsed_ms(&s_sess);
    if (force || ms / 1000 != last_shown_ms / 1000) {
        char buf[16];
        fmt_mmss(buf, sizeof(buf), ms);
        lv_label_set_text(s_time_label, buf);
        lv_label_set_text_fmt(s_steps_label, "%d 步", (int)ssj_session_steps(&s_sess));
        last_shown_ms = ms;
    }

    // 提示文案随状态切换
    lv_label_set_text(s_hint_label,
                      paused ? "OK 继续 · 长按结束" : "OK 暂停 · 长按结束");

    // 节拍点:暂停时停住;恢复时重启动画
    if (paused) {
        beat_anim_stop();
        lv_obj_set_style_bg_color(s_beat_dot, lv_color_hex(SSJ_COL_WARN), 0);
    } else {
        lv_obj_set_style_bg_color(s_beat_dot, lv_color_hex(phase_color), 0);
        if (lv_anim_count_running() == 0) beat_anim_start();
    }

    refresh_battery(s_battery_run);
}

// ---------------------------------------------------------------------------
// 完成页
// ---------------------------------------------------------------------------
static void done_page_build(void) {
    if (s_done_scr) lv_obj_delete(s_done_scr);
    s_done_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_done_scr, lv_color_hex(SSJ_COL_BG), 0);
    lv_obj_set_style_border_width(s_done_scr, 0, 0);

    s_battery_done = make_battery_label(s_done_scr);

    s_done_title = make_label(s_done_scr, &ssj_font_20,
                              ssj_session_target_reached(&s_sess) ? SSJ_COL_ACCENT : SSJ_COL_SUB);
    lv_label_set_text(s_done_title,
                      ssj_session_target_reached(&s_sess) ? "完成!" : "已结束");
    lv_obj_align(s_done_title, LV_ALIGN_TOP_MID, 0, 60);

    s_done_stats = make_label(s_done_scr, &ssj_font_20, SSJ_COL_TEXT);
    char tbuf[16];
    fmt_mmss(tbuf, sizeof(tbuf), ssj_session_elapsed_ms(&s_sess));
    lv_label_set_text_fmt(s_done_stats, "总时长  %s\n步数  %d",
                          tbuf, (int)ssj_session_steps(&s_sess));
    lv_obj_set_style_text_align(s_done_stats, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_done_stats, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *hint = make_label(s_done_scr, &ssj_font_20, SSJ_COL_SUB);
    lv_label_set_text(hint, "OK 返回");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    refresh_battery(s_battery_done);
    lv_screen_load(s_done_scr);
}

// ---------------------------------------------------------------------------
// lv_timer 主循环
// ---------------------------------------------------------------------------
static void app_tick(lv_timer_t *timer) {
    (void)timer;
    if (s_page != SSJ_PAGE_RUN) return;

    if (s_sess.state == SSJ_RUNNING) {
        int ev = ssj_session_tick(&s_sess, esp_timer_get_time());
        if (ev & SSJ_EV_PHASE_REST) {
            ssj_audio_tick_pitch(true);
            ssj_audio_play(SSJ_SND_PHASE_REST);
        }
        if (ev & SSJ_EV_PHASE_RUN) {
            ssj_audio_tick_pitch(false);
            ssj_audio_play(SSJ_SND_PHASE_RUN);
        }
        if (ev & SSJ_EV_DONE) {
            ssj_audio_metronome(0, false);
            ssj_audio_play(SSJ_SND_DONE);
            beat_anim_stop();
            done_page_build();
            s_page = SSJ_PAGE_DONE;
            return;
        }
    }
    run_page_refresh(false);
}

// ---------------------------------------------------------------------------
// 按键处理(调用方已持有 LVGL 锁)
// ---------------------------------------------------------------------------
void ssj_app_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_RELEASE) return;   // 抬起事件不参与交互

    switch (s_page) {
        case SSJ_PAGE_HOME:
            if (ev != BSP_BTN_CLICK) return;   // 主页不响应长按
            if (btn == BSP_BTN_UP) {
                s_home_sel = (s_home_sel + s_row_count - 1) % s_row_count;
                home_refresh();
            } else if (btn == BSP_BTN_DOWN) {
                s_home_sel = (s_home_sel + 1) % s_row_count;
                home_refresh();
            } else if (btn == BSP_BTN_OK) {
                home_activate();
            }
            break;

        case SSJ_PAGE_RUN:
            if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
                // 长按结束:收账并进入完成页
                ssj_session_finish(&s_sess);
                ssj_audio_metronome(0, false);
                ssj_audio_play(SSJ_SND_DONE);
                beat_anim_stop();
                done_page_build();
                s_page = SSJ_PAGE_DONE;
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                if (s_sess.state == SSJ_RUNNING) {
                    ssj_session_pause(&s_sess, esp_timer_get_time());
                    ssj_audio_metronome(0, false);
                } else if (s_sess.state == SSJ_PAUSED) {
                    ssj_session_resume(&s_sess, esp_timer_get_time());
                    ssj_audio_metronome(60000u / (uint32_t)s_sess.cfg.cadence, true);
                    ssj_audio_tick_pitch(s_sess.cfg.mode == SSJ_MODE_INTERVAL
                                             && s_sess.phase == SSJ_PHASE_REST);
                }
                run_page_refresh(true);
            } else if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
                // 运行/暂停中 UP/DOWN 调音量
                const int step = (btn == BSP_BTN_UP) ? 10 : -10;
                int v = (int)s_volume + step;
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                s_volume = (uint8_t)v;
                ssj_audio_set_volume(s_volume);
                char buf[24];
                lv_snprintf(buf, sizeof(buf), "音量 %d%%", s_volume);
                toast_show(buf);
            }
            break;

        case SSJ_PAGE_DONE:
            if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
                home_build();
                s_page = SSJ_PAGE_HOME;
            }
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// 应用入口
// ---------------------------------------------------------------------------
void ssj_app_init(void) {
    // 调用前提:bsp_display_init/bsp_lvgl_init 已成功,且本函数在 LVGL 锁内执行。
    s_page = SSJ_PAGE_HOME;
    s_home_sel = 0;

    if (!ssj_store_load(&s_cfg)) {
        ESP_LOGI(TAG, "使用默认超慢跑配置(180 步/分)");
    }

    // 音频失败不阻塞:应用无声运行(节拍可视化仍在)。
    if (!ssj_audio_start()) {
        ESP_LOGW(TAG, "音频不可用,无声运行");
    }

    // 会话计时主循环:100ms 周期,在 LVGL 任务上下文执行,无需持锁。
    s_tick_timer = lv_timer_create(app_tick, 100, NULL);

    home_build();
}
