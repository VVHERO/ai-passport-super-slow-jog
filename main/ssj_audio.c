// main/ssj_audio.c —— 超慢跑节拍音任务实现。
#include "ssj_audio.h"

#include "bsp_audio.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>

static const char *TAG = "ssj_audio";

// ---------------------------------------------------------------------------
// PCM 常量。16kHz/16bit/单声道与 bsp_audio_set_format 保持一致。
// 节拍 42ms = 672 采样 = 1344 字节,静态缓冲,避免运行期分配。
// ---------------------------------------------------------------------------
#define SSJ_PCM_HZ 16000
#define SSJ_TICK_MS 42
#define SSJ_TICK_SAMPLES (SSJ_PCM_HZ * SSJ_TICK_MS / 1000)   // 672

// 控制命令队列深度:节拍事件不进队列(任务自调度),队列只走低频控制命令,4 足够。
#define SSJ_AUDIO_QUEUE_LEN 4

typedef enum {
    SSJ_CMD_METRONOME = 0,  // 开关节拍器
    SSJ_CMD_PITCH,          // 切换节拍音高
    SSJ_CMD_SOUND,          // 播放一次性提示音
} ssj_cmd_type_t;

typedef struct {
    ssj_cmd_type_t type;
    uint32_t a;   // METRONOME: period_ms;PITCH: rest_phase(0/1);SOUND: ssj_sound_t
    uint32_t b;   // METRONOME: on(0/1)
} ssj_cmd_t;

static QueueHandle_t s_queue;         // 控制命令队列(main/UI 任务 -> 音频任务)
static TaskHandle_t s_task;           // 音频任务句柄
static volatile bool s_running;       // 任务退出标志(单写多读,volatile 足够)

// 节拍正弦查找表:一个周期整 64 点,v[i] = round(1000*sin(2*pi*i/64)),
// 振幅 ±1000(合成时再按 -6dBFS 缩放)。预计算避免运行期 sin() 浮点开销
// (ESP32-C3 无 FPU)。必须恰好 64 个元素,配合下方 &63 取模。
static const int16_t k_sin64[64] = {
       0,   98,  195,  290,  383,  471,  556,  634,  707,  773,  831,  882,  924,  957,  981,  995,
    1000,  995,  981,  957,  924,  882,  831,  773,  707,  634,  556,  471,  383,  290,  195,   98,
       0,  -98, -195, -290, -383, -471, -556, -634, -707, -773, -831, -882, -924, -957, -981, -995,
   -1000, -995, -981, -957, -924, -882, -831, -773, -707, -634, -556, -471, -383, -290, -195,  -98,
};

// 节拍 PCM 缓冲(每次发声重新填充;任务私有,无需加锁)。
static int16_t s_tick_pcm[SSJ_TICK_SAMPLES];
// 提示音缓冲:最长单音 400ms(完成音 240ms + 余量),避免长音被截断。
static int16_t s_melody_pcm[SSJ_PCM_HZ * 4 / 10];

// ---------------------------------------------------------------------------
// PCM 合成
// ---------------------------------------------------------------------------
// 把一段正弦写入 out:freq_hz + 时长 ms,4ms 淡入/淡出防爆音。
// 返回实际写入采样数。仅音频任务调用。
static int synth_tone(int16_t *out, int max_samples, int freq_hz, int dur_ms) {
    int samples = SSJ_PCM_HZ * dur_ms / 1000;
    if (samples > max_samples) samples = max_samples;
    // 定点 16.16 相位步进:step = freq / 16000 * 64 * 65536
    const uint32_t step_q16 = (uint32_t)(((uint64_t)freq_hz * 64 * 65536) / SSJ_PCM_HZ);
    const int fade = SSJ_PCM_HZ * 4 / 1000;   // 4ms 淡入出
    uint32_t phase_q16 = 0;
    for (int i = 0; i < samples; i++) {
        int32_t v = k_sin64[(phase_q16 >> 16) & 63] * 16000 / 1000;   // 峰值 16000 ≈ -6dBFS
        if (i < fade) {
            v = v * i / fade;
        } else if (i > samples - fade) {
            v = v * (samples - i) / fade;
        }
        out[i] = (int16_t)v;
        phase_q16 += step_q16;
    }
    return samples;
}

// 一次性提示音旋律:{频率 Hz, 时长 ms},以 {0,0} 结尾;音间静音 30ms。仅音频任务调用。
static void play_melody(const int (*notes)[2]) {
    for (int n = 0; notes[n][0] != 0; n++) {
        int samples = synth_tone(s_melody_pcm, (int)(sizeof(s_melody_pcm) / sizeof(s_melody_pcm[0])),
                                 notes[n][0], notes[n][1]);
        esp_err_t e = bsp_audio_write(s_melody_pcm, samples * sizeof(int16_t));
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "旋律写入失败: %s", esp_err_to_name(e));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// 阶段切换/完成提示音定义。三连音提示阶段切换,上行双音提示完成。
static const int k_melody_rest[4][2] = {{640, 90}, {640, 90}, {640, 90}, {0, 0}};
static const int k_melody_run[4][2] = {{1000, 90}, {1000, 90}, {1000, 90}, {0, 0}};
static const int k_melody_done[3][2] = {{880, 120}, {1320, 240}, {0, 0}};

// ---------------------------------------------------------------------------
// 任务主体
// ---------------------------------------------------------------------------
static void audio_task(void *arg) {
    (void)arg;
    // 节拍器状态(任务私有):
    bool metro_on = false;
    bool rest_phase = false;
    uint32_t period_ms = 333;   // 180 步/分 ≈ 333ms,开节拍器时会被命令覆盖
    int64_t next_beat_us = 0;

    for (;;) {
        ssj_cmd_t cmd;
        if (!metro_on) {
            // 节拍器关着:阻塞等命令,零 CPU 占用。
            if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        } else {
            int64_t remain_us = next_beat_us - esp_timer_get_time();
            if (remain_us <= 0) {
                // 到点:发拍。按绝对时刻累加,唤醒抖动不累积。
                int samples = synth_tone(s_tick_pcm, SSJ_TICK_SAMPLES,
                                         rest_phase ? 640 : 1000, SSJ_TICK_MS);
                esp_err_t e = bsp_audio_write(s_tick_pcm, samples * sizeof(int16_t));
                if (e != ESP_OK) {
                    ESP_LOGW(TAG, "节拍写入失败: %s", esp_err_to_name(e));
                }
                next_beat_us += (int64_t)period_ms * 1000;
                // 落后太多(如刚从长暂停恢复)时重新对表,不追发历史节拍。
                if (next_beat_us < esp_timer_get_time()) {
                    next_beat_us = esp_timer_get_time() + (int64_t)period_ms * 1000;
                }
                continue;
            }
            // 没到点:限时等命令;超时返回后循环顶部自然发拍。
            TickType_t wait_ticks = (TickType_t)(remain_us / 1000 / portTICK_PERIOD_MS);
            if (wait_ticks == 0) wait_ticks = 1;
            if (xQueueReceive(s_queue, &cmd, wait_ticks) != pdTRUE) continue;
        }

        // 处理控制命令
        switch (cmd.type) {
            case SSJ_CMD_METRONOME:
                period_ms = cmd.a;
                metro_on = (cmd.b != 0);
                if (metro_on) {
                    next_beat_us = esp_timer_get_time() + (int64_t)period_ms * 1000;
                }
                break;
            case SSJ_CMD_PITCH:
                rest_phase = (cmd.a != 0);
                break;
            case SSJ_CMD_SOUND:
                switch ((ssj_sound_t)cmd.a) {
                    case SSJ_SND_PHASE_REST: play_melody(k_melody_rest); break;
                    case SSJ_SND_PHASE_RUN: play_melody(k_melody_run); break;
                    case SSJ_SND_DONE: play_melody(k_melody_done); break;
                    default: break;
                }
                break;
            default:
                break;
        }
        if (!s_running) break;
    }
    ESP_LOGI(TAG, "音频任务退出");
    s_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
bool ssj_audio_start(void) {
    if (s_task) return true;   // 幂等

    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败,无声运行");
        return false;
    }
    if (bsp_audio_set_format(SSJ_PCM_HZ, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "音频格式设置失败,无声运行");
        return false;
    }
    bsp_audio_set_volume(60);   // 默认 60%,按键可调

    s_queue = xQueueCreate(SSJ_AUDIO_QUEUE_LEN, sizeof(ssj_cmd_t));
    if (!s_queue) return false;
    s_running = true;
    if (xTaskCreate(audio_task, "ssj_audio", 4096, NULL, 5, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        s_running = false;
        return false;
    }
    return true;
}

void ssj_audio_stop(void) {
    s_running = false;
    // 发一条任意命令唤醒阻塞的任务,让它检查 s_running 后自行退出。
    // 不用 vTaskDelete:避免删除持有 I2S 临界区的任务。
    if (s_queue && s_task) {
        ssj_cmd_t cmd = {.type = SSJ_CMD_METRONOME, .a = 0, .b = 0};
        xQueueSend(s_queue, &cmd, 0);
    }
}

void ssj_audio_metronome(uint32_t period_ms, bool on) {
    if (!s_queue) return;
    ssj_cmd_t cmd = {.type = SSJ_CMD_METRONOME, .a = period_ms, .b = on ? 1 : 0};
    xQueueSend(s_queue, &cmd, 0);
}

void ssj_audio_tick_pitch(bool rest_phase) {
    if (!s_queue) return;
    ssj_cmd_t cmd = {.type = SSJ_CMD_PITCH, .a = rest_phase ? 1 : 0};
    xQueueSend(s_queue, &cmd, 0);
}

void ssj_audio_play(ssj_sound_t snd) {
    if (!s_queue) return;
    ssj_cmd_t cmd = {.type = SSJ_CMD_SOUND, .a = (uint32_t)snd};
    xQueueSend(s_queue, &cmd, 0);
}

void ssj_audio_set_volume(uint8_t percent) {
    bsp_audio_set_volume(percent);
}
