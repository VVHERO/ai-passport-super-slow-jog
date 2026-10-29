// main/ssj_audio.c —— 超慢跑音频任务:节拍音 + 伴跑音乐 + 随拍闪屏。
//
// 采样格式:22.05 kHz / 16bit / 单声道。音乐素材与节拍音共用同一个 I2S 输出,
// 因此音乐播放时节拍声是"混"进音乐块里的,不需要第二路 DMA。
#include "ssj_audio.h"

#include "bsp_audio.h"
#include "bsp_display.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "ssj_audio";

// 内嵌音乐素材(main/CMakeLists.txt 的 EMBED_FILES 生成这两个符号)。
extern const uint8_t ssj_music_adp_start[] asm("_binary_ssj_music_adp_start");
extern const uint8_t ssj_music_adp_end[] asm("_binary_ssj_music_adp_end");

// ---------------------------------------------------------------------------
// 素材格式常量。必须与 tools/pack_music.py 的 SAMPLE_RATE / BLOCK_SAMPLES 一致。
//
// 2450 采样 = 111.1ms。之所以不是整毫秒,是因为 180 步/分 = 每秒 3 拍,
// 2450 = 7350/3 → 一拍正好 3 个块,节拍音可以永远落在块首,运行期不做样本定位。
// ---------------------------------------------------------------------------
#define SSJ_PCM_HZ 22050
#define SSJ_MUSIC_BLOCK_SAMPLES 2450
#define SSJ_MUSIC_BLOCK_BYTES 1229           // 4 字节块头 + 2450/2 个 4bit 采样
#define SSJ_MUSIC_BEAT_BLOCKS 3              // 每 3 块 = 1 拍(333.3 ms)
#define SSJ_MUSIC_MAX_BLOCKS 4096

// 节拍短音:42ms,带淡入淡出防爆音。
#define SSJ_TICK_MS 42
#define SSJ_TICK_SAMPLES (SSJ_PCM_HZ * SSJ_TICK_MS / 1000)   // 926

// 闪屏:写入一块后音频还要经过 DMA 缓冲才真正出声(6 x 240 帧 ≈ 65ms @22.05kHz),
// 所以闪屏相对写入时刻延后同样时间,画面才和耳朵对齐。
#define SSJ_FLASH_DELAY_US 60000
#define SSJ_FLASH_DIM_US 110000

// 控制命令队列深度:音乐播放时每块(111ms)轮询一次,低频命令足够。
#define SSJ_AUDIO_QUEUE_LEN 6

typedef enum {
    SSJ_CMD_METRONOME = 0,  // 开关节拍器
    SSJ_CMD_PITCH,          // 切换节拍音高
    SSJ_CMD_SOUND,          // 播放一次性提示音
    SSJ_CMD_MUSIC,          // 开关伴跑音乐
    SSJ_CMD_FLASH,          // 闪屏档位
} ssj_cmd_type_t;

typedef struct {
    ssj_cmd_type_t type;
    uint32_t a;   // METRONOME: period_ms;PITCH: rest_phase;SOUND: ssj_sound_t;
                  // MUSIC: on;FLASH: 档位
    uint32_t b;   // METRONOME: on;MUSIC: restart
} ssj_cmd_t;

static QueueHandle_t s_queue;         // 控制命令队列(UI 任务 -> 音频任务)
static TaskHandle_t s_task;           // 音频任务句柄
static volatile bool s_running;       // 任务退出标志(单写多读,volatile 足够)

// 音乐素材块数与采样缓冲(任务私有)。
static uint32_t s_music_blocks;
static int16_t s_mix[SSJ_MUSIC_BLOCK_SAMPLES];   // 一块解码结果(兼作混音缓冲)
static int16_t s_tick_pcm[SSJ_TICK_SAMPLES];     // 节拍短音
static int16_t s_melody_pcm[SSJ_PCM_HZ * 4 / 10];// 提示音单音缓冲

// 闪屏状态(音频任务写档位,esp_timer 任务做背光切换)。
static esp_timer_handle_t s_flash_dim_timer;
static esp_timer_handle_t s_flash_restore_timer;
static volatile uint8_t s_flash_level;    // 0=关 1=柔 2=强
static volatile bool s_flash_pending;     // 本轮闪烁是否已排程

// 档位 -> 压暗百分比(0 档不用)。
static const uint8_t k_flash_dim[3] = {0, 45, 8};

#ifdef SSJ_SELFTEST
// 自检固件专用:记录拍点与闪屏的实际时刻,便于串口核对节拍周期与相位。
static volatile int64_t s_st_t0;
static volatile int s_st_beat;

// 自检固件专用:用板载麦克风回采,证明音乐真的从喇叭出来了(声学回路)。
// 只有 SSJ_SELFTEST 构建才创建这个任务,发布固件不含。
static void selftest_mic_task(void *arg) {
    (void)arg;
    static int16_t buf[512];
    for (;;) {
        esp_err_t e = bsp_audio_read(buf, sizeof(buf));
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "SELFTEST mic read failed: %s", esp_err_to_name(e));
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        int32_t energy = 0;
        int32_t peak = 0;
        for (int i = 0; i < 256; i++) {
            const int32_t v = buf[i];
            energy += (v * v) / 256;
            if (v > peak) peak = v;
            if (-v > peak) peak = -v;
        }
        ESP_LOGI(TAG, "SELFTEST mic energy=%ld peak=%ld", (long)energy, (long)peak);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#endif

// 节拍正弦查找表:一个周期整 64 点,v[i] = round(1000*sin(2*pi*i/64)),
// 振幅 ±1000(合成时再按 -6dBFS 缩放)。预计算避免运行期 sin() 浮点开销
// (ESP32-C3 无 FPU)。必须恰好 64 个元素,配合下方 &63 取模。
static const int16_t k_sin64[64] = {
       0,   98,  195,  290,  383,  471,  556,  634,  707,  773,  831,  882,  924,  957,  981,  995,
    1000,  995,  981,  957,  924,  882,  831,  773,  707,  634,  556,  471,  383,  290,  195,   98,
       0,  -98, -195, -290, -383, -471, -556, -634, -707, -773, -831, -882, -924, -957, -981, -995,
   -1000, -995, -981, -957, -924, -882, -831, -773, -707, -634, -556, -471, -383, -290, -195,  -98,
};

// IMA ADPCM 步长表与索引增量表(与 tools/pack_music.py 完全一致)。
static const int16_t k_step[89] = {
       7,    8,    9,   10,   11,   12,   13,   14,   16,   17,   19,   21,
      23,   25,   28,   31,   34,   37,   41,   45,   50,   55,   60,   66,
      73,   80,   88,   97,  107,  118,  130,  143,  157,  173,  190,  209,
     230,  253,  279,  307,  337,  371,  408,  449,  494,  544,  598,  658,
     724,  796,  876,  963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t k_index_adj[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

static inline int16_t sat16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// ---------------------------------------------------------------------------
// 音乐解码:一块 1229 字节 -> 2450 个 int16。块自带预测器/步长索引,可从任意块开始。
// ---------------------------------------------------------------------------
static void decode_block(const uint8_t *in, int16_t *out) {
    int32_t pred = (int16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
    int index = in[2];
    if (index > 88) index = 88;
    const uint8_t *nib = in + 4;
    int n = 0;
    for (int i = 0; i < SSJ_MUSIC_BLOCK_SAMPLES / 2; i++) {
        const uint8_t byte = nib[i];
        for (int half = 0; half < 2; half++) {
            const uint8_t code = half == 0 ? (byte & 0x0F) : (byte >> 4);
            const int32_t step = k_step[index];
            int32_t diff = step >> 3;
            if (code & 4) diff += step;
            if (code & 2) diff += step >> 1;
            if (code & 1) diff += step >> 2;
            pred = (code & 8) ? pred - diff : pred + diff;
            pred = sat16(pred);
            index += k_index_adj[code & 7];
            if (index < 0) index = 0;
            if (index > 88) index = 88;
            out[n++] = (int16_t)pred;
        }
    }
}

// ---------------------------------------------------------------------------
// PCM 合成
// ---------------------------------------------------------------------------
// 把一段正弦写入 out:freq_hz + 时长 ms,4ms 淡入/淡出防爆音。
// 返回实际写入采样数。仅音频任务调用。
static int synth_tone(int16_t *out, int max_samples, int freq_hz, int dur_ms) {
    int samples = SSJ_PCM_HZ * dur_ms / 1000;
    if (samples > max_samples) samples = max_samples;
    // 定点 16.16 相位步进:step = freq / 22050 * 64 * 65536
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

// 把节拍短音叠加到音乐块开头(音乐块首即拍点)。越界饱和,避免破音。
// 音乐模式下节拍音降到 -12dBFS(合成峰值 8000):音乐本身已有重拍,提示音只需要
// 能听出来,同时给音乐留出不削波的余量(素材峰值已归一化到 0.72 FS)。
static void mix_tick(int16_t *block, int block_samples, bool rest_phase) {
    const int samples = synth_tone(s_tick_pcm, SSJ_TICK_SAMPLES, rest_phase ? 640 : 1000, SSJ_TICK_MS);
    for (int i = 0; i < samples && i < block_samples; i++) {
        block[i] = sat16((int32_t)block[i] + s_tick_pcm[i] / 2);
    }
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
// 随拍闪屏:写入拍点块后延时压暗背光,110ms 后恢复。
// ---------------------------------------------------------------------------
static void flash_dim_cb(void *arg) {
    (void)arg;
    if (!s_flash_pending) return;
    bsp_display_backlight(k_flash_dim[s_flash_level]);
    esp_timer_stop(s_flash_restore_timer);
    esp_timer_start_once(s_flash_restore_timer, SSJ_FLASH_DIM_US);
#ifdef SSJ_SELFTEST
    ESP_LOGI(TAG, "SELFTEST flash dim  t=%lldms lvl=%u",
             (long long)((esp_timer_get_time() - s_st_t0) / 1000), (unsigned)s_flash_level);
#endif
}

static void flash_restore_cb(void *arg) {
    (void)arg;
    bsp_display_backlight(100);
    s_flash_pending = false;
}

static void flash_trigger(void) {
    if (s_flash_level == 0 || s_flash_pending) return;
    if (!s_flash_dim_timer) return;
    s_flash_pending = true;
    esp_timer_stop(s_flash_dim_timer);
    esp_timer_start_once(s_flash_dim_timer, SSJ_FLASH_DELAY_US);
}

// 取消进行中的闪烁并恢复满亮(暂停/结束/关闭闪屏时调用)。
static void flash_cancel(void) {
    s_flash_pending = false;
    if (s_flash_dim_timer) esp_timer_stop(s_flash_dim_timer);
    if (s_flash_restore_timer) esp_timer_stop(s_flash_restore_timer);
    bsp_display_backlight(100);
}

// ---------------------------------------------------------------------------
// 任务主体
// ---------------------------------------------------------------------------
static void handle_cmd(const ssj_cmd_t *cmd, bool *metro_on, uint32_t *period_ms,
                       bool *rest_phase, bool *music_on, uint32_t *block,
                       int64_t *next_beat_us) {
    switch (cmd->type) {
        case SSJ_CMD_METRONOME:
            *period_ms = cmd->a;
            *metro_on = (cmd->b != 0);
            if (*metro_on) {
                *next_beat_us = esp_timer_get_time() + (int64_t)*period_ms * 1000;
            } else {
                flash_cancel();
            }
            break;
        case SSJ_CMD_PITCH:
            *rest_phase = (cmd->a != 0);
            break;
        case SSJ_CMD_SOUND:
            switch ((ssj_sound_t)cmd->a) {
                case SSJ_SND_PHASE_REST: play_melody(k_melody_rest); break;
                case SSJ_SND_PHASE_RUN: play_melody(k_melody_run); break;
                case SSJ_SND_DONE: play_melody(k_melody_done); break;
                default: break;
            }
            break;
        case SSJ_CMD_MUSIC:
            if (cmd->a) {
                if (cmd->b || !*music_on) *block = 0;   // 新会话从素材开头
                *music_on = true;
            } else {
                *music_on = false;
                flash_cancel();
                // 关音乐但节拍器还在(自动关音乐不存在,这里只做兜底):回到定时器排拍。
                if (*metro_on) *next_beat_us = esp_timer_get_time() + (int64_t)*period_ms * 1000;
            }
            break;
        case SSJ_CMD_FLASH:
            s_flash_level = (uint8_t)(cmd->a > 2 ? 2 : cmd->a);
            if (s_flash_level == 0) flash_cancel();
            break;
        default:
            break;
    }
}

static void audio_task(void *arg) {
    (void)arg;
    bool metro_on = false;
    bool rest_phase = false;
    bool music_on = false;
    uint32_t period_ms = 333;   // 180 步/分 ≈ 333ms
    uint32_t block = 0;
    int64_t next_beat_us = 0;

    for (;;) {
        ssj_cmd_t cmd;

        if (music_on) {
            // ---- 音乐流:每块 111ms,块首即拍点(每 3 块一拍) ----
            const bool beat = (block % SSJ_MUSIC_BEAT_BLOCKS) == 0;
            decode_block(ssj_music_adp_start + (size_t)block * SSJ_MUSIC_BLOCK_BYTES, s_mix);
            if (beat && metro_on) mix_tick(s_mix, SSJ_MUSIC_BLOCK_SAMPLES, rest_phase);
            esp_err_t e = bsp_audio_write(s_mix, SSJ_MUSIC_BLOCK_SAMPLES * sizeof(int16_t));
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "音乐写入失败: %s", esp_err_to_name(e));
                vTaskDelay(pdMS_TO_TICKS(SSJ_MUSIC_BLOCK_SAMPLES * 1000 / SSJ_PCM_HZ));
            }
            if (beat) flash_trigger();
#ifdef SSJ_SELFTEST
            if (beat) {
                if (s_st_t0 == 0) s_st_t0 = esp_timer_get_time();
                ESP_LOGI(TAG, "SELFTEST beat %d block=%u t=%lldms",
                         s_st_beat, (unsigned)block,
                         (long long)((esp_timer_get_time() - s_st_t0) / 1000));
                s_st_beat++;
            }
#endif
            block++;
            if (block >= s_music_blocks) block = 0;
            // 排空命令队列:暂停/结束最迟在一个块(111ms)内生效。
            // (这一轮写入已经进 DMA,所以实际静音还要等缓冲区放完,约 60ms。)
            while (xQueueReceive(s_queue, &cmd, 0) == pdTRUE) {
                handle_cmd(&cmd, &metro_on, &period_ms, &rest_phase, &music_on, &block,
                           &next_beat_us);
                if (!music_on) break;
            }
        } else if (metro_on) {
            // ---- 纯节拍器:按绝对时刻调度,唤醒抖动不累积 ----
            const int64_t now_us = esp_timer_get_time();
            const int64_t remain_us = next_beat_us - now_us;
            if (remain_us <= 0) {
                int samples = synth_tone(s_tick_pcm, SSJ_TICK_SAMPLES,
                                         rest_phase ? 640 : 1000, SSJ_TICK_MS);
                esp_err_t e = bsp_audio_write(s_tick_pcm, samples * sizeof(int16_t));
                if (e != ESP_OK) {
                    ESP_LOGW(TAG, "节拍写入失败: %s", esp_err_to_name(e));
                }
                flash_trigger();
                next_beat_us += (int64_t)period_ms * 1000;
                // 落后太多(如刚从长暂停恢复)时重新对表,不追发历史节拍。
                if (next_beat_us < esp_timer_get_time()) {
                    next_beat_us = esp_timer_get_time() + (int64_t)period_ms * 1000;
                }
                if (!s_running) break;
                continue;
            }
            TickType_t wait_ticks = (TickType_t)(remain_us / 1000 / portTICK_PERIOD_MS);
            if (wait_ticks == 0) wait_ticks = 1;
            if (xQueueReceive(s_queue, &cmd, wait_ticks) == pdTRUE) {
                handle_cmd(&cmd, &metro_on, &period_ms, &rest_phase, &music_on, &block,
                           &next_beat_us);
            }
        } else {
            // ---- 空闲:阻塞等命令,零 CPU 占用 ----
            if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
            handle_cmd(&cmd, &metro_on, &period_ms, &rest_phase, &music_on, &block,
                       &next_beat_us);
        }
        if (!s_running) break;
    }

    flash_cancel();
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
    bsp_audio_set_volume(60);   // 默认 60%,UI 载入配置后会覆盖

    // 音乐块数由素材长度推算,防止固件与素材版本不一致时越界。
    const size_t music_bytes = (size_t)(ssj_music_adp_end - ssj_music_adp_start);
    s_music_blocks = (uint32_t)(music_bytes / SSJ_MUSIC_BLOCK_BYTES);
    if (s_music_blocks == 0) {
        ESP_LOGW(TAG, "音乐素材为空,本次只播节拍音");
    } else if (s_music_blocks > SSJ_MUSIC_MAX_BLOCKS) {
        s_music_blocks = SSJ_MUSIC_MAX_BLOCKS;
    }
    ESP_LOGI(TAG, "伴跑音乐素材 %u 字节 / %u 块 (%.1f 秒)",
             (unsigned)music_bytes, (unsigned)s_music_blocks,
             (double)s_music_blocks * SSJ_MUSIC_BLOCK_SAMPLES / SSJ_PCM_HZ);

    esp_timer_create_args_t targs = {.callback = flash_dim_cb, .name = "ssj_flash_dim"};
    if (esp_timer_create(&targs, &s_flash_dim_timer) != ESP_OK) s_flash_dim_timer = NULL;
    targs.callback = flash_restore_cb;
    targs.name = "ssj_flash_restore";
    if (esp_timer_create(&targs, &s_flash_restore_timer) != ESP_OK) s_flash_restore_timer = NULL;

    s_queue = xQueueCreate(SSJ_AUDIO_QUEUE_LEN, sizeof(ssj_cmd_t));
    if (!s_queue) return false;
    s_running = true;
    // 音乐流每 111ms 解码一块 2450 采样,栈需求比纯节拍略高。
    if (xTaskCreate(audio_task, "ssj_audio", 5120, NULL, 6, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        s_running = false;
        return false;
    }
#ifdef SSJ_SELFTEST
    {
        TaskHandle_t mic = NULL;
        if (xTaskCreate(selftest_mic_task, "ssj_st_mic", 3072, NULL, 2, &mic) != pdPASS) {
            ESP_LOGW(TAG, "SELFTEST 麦克风回采任务创建失败");
        }
    }
#endif
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

void ssj_audio_set_music(bool on, bool restart) {
    if (!s_queue) return;
    ssj_cmd_t cmd = {.type = SSJ_CMD_MUSIC, .a = on ? 1 : 0, .b = restart ? 1 : 0};
    xQueueSend(s_queue, &cmd, 0);
}

void ssj_audio_set_flash(uint8_t level) {
    if (!s_queue) return;
    ssj_cmd_t cmd = {.type = SSJ_CMD_FLASH, .a = level};
    xQueueSend(s_queue, &cmd, 0);
}

void ssj_audio_set_volume(uint8_t percent) {
    bsp_audio_set_volume(percent);
}
