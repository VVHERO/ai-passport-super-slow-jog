// main/ssj_audio.h —— 超慢跑音频任务:节拍提示音 + 180 步频伴跑音乐 + 随拍闪屏。
//
// 设计:独立 FreeRTOS 任务持有音频输出;UI/LVGL 通过命令队列与它通信,
// 绝不直接在 LVGL 上下文里做 I2S 写入(阻塞)。节拍按绝对时刻调度
// (esp_timer_get_time()),不受任务唤醒抖动累积影响;暂停/恢复由 UI 发命令。
//
// 两种发声模式:
//   1) 只有节拍器(音乐关):每个节拍合成 42ms 短音,跑步 1.0 kHz / 休息 0.64 kHz,
//      阶段切换三连音、完成上行双音。零素材占用。
//   2) 伴跑音乐(音乐开):循环播放内嵌的 400 秒 180 步频素材(22.05 kHz 单声道
//      IMA ADPCM,见 tools/pack_music.py),节拍声按音乐重拍叠加在块首。
//      音乐速度即步频,因此开音乐时会话步频固定为 180。
//
// 闪屏:每个节拍把背光短暂压暗(0=关 / 1=柔 45% / 2=强 8%),与节拍/音乐重拍同步。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 事件提示音类型(UI 在阶段切换/完成时投递)。
typedef enum {
    SSJ_SND_PHASE_REST = 0,  // 进入休息:低音三连
    SSJ_SND_PHASE_RUN,       // 进入跑步:高音三连
    SSJ_SND_DONE,            // 会话完成:上行双音
} ssj_sound_t;

// 启动音频任务(幂等)。内部调用 bsp_audio_init() + bsp_audio_set_format(22.05k/16bit/mono)。
// 返回 false = 初始化失败(硬件缺失等),任务不创建;UI 侧应允许无声降级。
bool ssj_audio_start(void);

// 停止音频任务并释放(会话页销毁时调用;非必须,固件常驻也可不复用)。
void ssj_audio_stop(void);

// 开始/停止节拍器。period_ms = 节拍间隔(60000/步频)。
// 音乐播放时该周期只用于关掉音乐后的节拍,不改变音乐速度。
void ssj_audio_metronome(uint32_t period_ms, bool on);

// 节拍音高切换(跑步/休息阶段变化时调用;节拍器开着才有意义)。
void ssj_audio_tick_pitch(bool rest_phase);

// 播放一次性提示音(异步投递,队列满则丢弃,不阻塞调用方)。
void ssj_audio_play(ssj_sound_t snd);

// 伴跑音乐开关。restart=true 时从素材开头播放(新会话),false 时从上次位置续播
// (暂停后继续)。音乐播放期间节拍声以音乐重拍为准。
void ssj_audio_set_music(bool on, bool restart);

// 随拍闪屏档位:0=关,1=柔(45% 亮度),2=强(8% 亮度)。0 会立即把背光恢复满亮。
void ssj_audio_set_flash(uint8_t level);

// 输出音量 0..100。立即生效,内部转发 bsp_audio_set_volume。
void ssj_audio_set_volume(uint8_t percent);
