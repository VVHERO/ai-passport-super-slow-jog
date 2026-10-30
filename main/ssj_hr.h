// main/ssj_hr.h —— 从 BLE 心率传感器(Amazfit Balance 的「心率推送」等)读取实时心率。
//
// 协议:标准 BLE 心率服务(Heart Rate Service 0x180D)的
// Heart Rate Measurement(0x2A37)通知,1 Hz。
// 设备角色:CENTRAL(GATT 客户端)—— 我们主动扫描、连接、订阅。
//
// 线程模型:NimBLE host 任务跑 GAP/GATT 回调,它们只写 volatile 快照;
// 本模块另有一个控制任务负责扫描/重连状态机。UI 只在 LVGL 上下文里读快照,
// 不做任何 BLE 调用。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SSJ_HR_OFF = 0,    // 关闭(默认),协议栈未启动或已停扫
    SSJ_HR_STARTING,   // BLE 协议栈启动中
    SSJ_HR_SEARCHING,  // 扫描中 / 等待重连
    SSJ_HR_CONNECTED,  // 已连接(心率数据可能尚未到达)
} ssj_hr_state_t;

// 打开/关闭心率接收。首次打开时才初始化 NimBLE(之后复用,不重复初始化)。
// 关闭会断开连接并停止扫描,协议栈保持就绪,便于再次打开。
void ssj_hr_set_enabled(bool on);

// 当前状态(供 UI 显示「搜索中 / 已连接」)。
ssj_hr_state_t ssj_hr_state(void);

// 最近一次有效心率。返回 false = 从未收到过,或已超过 stale_ms 没更新。
bool ssj_hr_latest(int *bpm, int64_t *age_ms);
