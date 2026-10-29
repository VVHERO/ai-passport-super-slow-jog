// main/ssj_audio.h —— 超慢跑节拍音/提示音任务。
//
// 设计:独立 FreeRTOS 任务持有音频输出;UI/LVGL 通过命令队列与它通信,
// 绝不直接在 LVGL 上下文里做 I2S 写入(阻塞)。节拍按绝对时刻调度
// (esp_timer_get_time()),不受任务唤醒抖动累积影响;暂停/恢复由 UI 发命令。
//
// 节拍音为程序合成短促正弦(无音频素材,零 Flash 占用):
//   跑步阶段 1.0 kHz / 休息阶段 0.64 kHz,42ms 带淡入淡出防爆音。
//   阶段切换:三连短音;完成:上行双音。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 事件提示音类型(UI 在阶段切换/完成时投递)。
typedef enum {
    SSJ_SND_PHASE_REST = 0,  // 进入休息:低音三连
    SSJ_SND_PHASE_RUN,       // 进入跑步:高音三连
    SSJ_SND_DONE,            // 会话完成:上行双音
} ssj_sound_t;

// 启动音频任务(幂等)。内部调用 bsp_audio_init() + bsp_audio_set_format(16k/16bit/mono)。
// 返回 false = 初始化失败(硬件缺失等),任务不创建;UI 侧应允许无声降级。
bool ssj_audio_start(void);

// 停止音频任务并释放(会话页销毁时调用;非必须,固件常驻也可不复用)。
void ssj_audio_stop(void);

// 开始/停止节拍器。period_ms = 节拍间隔(60000/步频)。
void ssj_audio_metronome(uint32_t period_ms, bool on);

// 节拍音高切换(跑步/休息阶段变化时调用;节拍器开着才有意义)。
void ssj_audio_tick_pitch(bool rest_phase);

// 播放一次性提示音(异步投递,队列满则丢弃,不阻塞调用方)。
void ssj_audio_play(ssj_sound_t snd);

// 输出音量 0..100。立即生效,内部转发 bsp_audio_set_volume。
void ssj_audio_set_volume(uint8_t percent);
