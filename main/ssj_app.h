// main/ssj_app.h —— 超慢跑应用入口(页面、按键、会话编排)。
#pragma once

#include "bsp_button.h"

#include <stdbool.h>

// 构建主页并启动应用(lv_timer 驱动会话计时)。必须在 LVGL 初始化成功后、
// 持有 bsp_lvgl_lock() 的上下文里调用一次。内部完成配置加载与音频启动。
void ssj_app_init(void);

// 按键事件入口。调用方必须持有 bsp_lvgl_lock()(事件在 LVGL 上下文里处理)。
// 回调语义:UP/DOWN 短按移动选择或调音量;OK 短按改值/开始/暂停/继续/返回;
// OK 长按结束会话。RELEASE 事件被忽略。
void ssj_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);
