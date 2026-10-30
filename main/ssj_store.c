// main/ssj_store.c —— 超慢跑配置持久化(NVS)实现。
#include "ssj_store.h"

#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "ssj_store";

// NVS 命名空间与键名。键名 <= 15 字符(NVS 约束)。
#define SSJ_NSV_NAMESPACE "ssj"
#define SSJ_KEY_MODE "mode"
#define SSJ_KEY_CADENCE "cadence"
#define SSJ_KEY_FREE_MIN "free_min"
#define SSJ_KEY_CYCLES "cycles"
#define SSJ_KEY_RUN_MIN "run_min"
#define SSJ_KEY_REST_MIN "rest_min"
#define SSJ_KEY_MUSIC "music"       // 新键:旧固件没有,缺失时用默认值(不整体回落)
#define SSJ_KEY_FLASH "flash"
#define SSJ_KEY_VOLUME "volume"
#define SSJ_KEY_HR "hr"             // 心率开关
#define SSJ_KEY_HR_MAX "hr_max"     // 最大心率(区间着色用)

// 出厂默认:经典超慢跑参数(180 步/分,自由跑 15 分钟,伴跑音乐开,闪屏柔,心率关)。
static ssj_config_t defaults(void) {
    ssj_config_t c = {0};
    c.mode = SSJ_MODE_FREE;
    c.cadence = SSJ_CADENCE_DEFAULT;
    c.free_min = SSJ_FREE_MIN_DEFAULT;
    c.cycles = SSJ_CYCLES_DEFAULT;
    c.run_min = SSJ_RUN_MIN_DEFAULT;
    c.rest_min = SSJ_REST_MIN_DEFAULT;
    c.music = true;
    c.flash = SSJ_FLASH_DEFAULT;
    c.volume = SSJ_VOLUME_DEFAULT;
    c.hr_enabled = false;                      // 省电:想用心率在设置页打开
    c.hr_max = SSJ_HR_MAX_DEFAULT;
    return c;
}

bool ssj_store_load(ssj_config_t *cfg) {
    if (!cfg) return false;
    *cfg = defaults();

    nvs_handle_t h;
    esp_err_t err = nvs_open(SSJ_NSV_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        // 首次启动命名空间不存在属正常,不刷错误日志。
        ESP_LOGI(TAG, "NVS 无已存配置,使用默认(%s)", esp_err_to_name(err));
        return false;
    }

    // 逐键读取;任一键缺失都视为"没有完整配置",整体回落默认,避免半新半旧。
    uint8_t mode = 0;
    uint16_t cadence = 0, free_min = 0, cycles = 0, run_min = 0, rest_min = 0;
    bool complete = true;
    complete &= (nvs_get_u8(h, SSJ_KEY_MODE, &mode) == ESP_OK);
    complete &= (nvs_get_u16(h, SSJ_KEY_CADENCE, &cadence) == ESP_OK);
    complete &= (nvs_get_u16(h, SSJ_KEY_FREE_MIN, &free_min) == ESP_OK);
    complete &= (nvs_get_u16(h, SSJ_KEY_CYCLES, &cycles) == ESP_OK);
    complete &= (nvs_get_u16(h, SSJ_KEY_RUN_MIN, &run_min) == ESP_OK);
    complete &= (nvs_get_u16(h, SSJ_KEY_REST_MIN, &rest_min) == ESP_OK);

    if (complete) {
        cfg->mode = (ssj_mode_t)mode;
        cfg->cadence = cadence;
        cfg->free_min = free_min;
        cfg->cycles = cycles;
        cfg->run_min = run_min;
        cfg->rest_min = rest_min;
    } else {
        // 基础 6 键不全视为"没有旧版完整配置",其余仍用默认值。
    }

    // 新增键:缺失时保留默认值,不因为升级固件就把用户参数清空。
    uint8_t music = 0, flash = 0, volume = 0, hr = 0;
    uint16_t hr_max = 0;
    if (nvs_get_u8(h, SSJ_KEY_MUSIC, &music) == ESP_OK) cfg->music = (music != 0);
    if (nvs_get_u8(h, SSJ_KEY_FLASH, &flash) == ESP_OK) cfg->flash = flash;
    if (nvs_get_u8(h, SSJ_KEY_VOLUME, &volume) == ESP_OK) cfg->volume = volume;
    if (nvs_get_u8(h, SSJ_KEY_HR, &hr) == ESP_OK) cfg->hr_enabled = (hr != 0);
    if (nvs_get_u16(h, SSJ_KEY_HR_MAX, &hr_max) == ESP_OK) cfg->hr_max = hr_max;
    nvs_close(h);

    ssj_config_clamp(cfg);
    return complete;
}

bool ssj_store_save(const ssj_config_t *cfg) {
    if (!cfg) return false;
    // 保存前先收敛,防止把越界值写进 NVS。
    ssj_config_t safe = *cfg;
    ssj_config_clamp(&safe);

    nvs_handle_t h;
    esp_err_t err = nvs_open(SSJ_NSV_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_u8(h, SSJ_KEY_MODE, (uint8_t)safe.mode);
    err |= nvs_set_u16(h, SSJ_KEY_CADENCE, (uint16_t)safe.cadence);
    err |= nvs_set_u16(h, SSJ_KEY_FREE_MIN, (uint16_t)safe.free_min);
    err |= nvs_set_u16(h, SSJ_KEY_CYCLES, (uint16_t)safe.cycles);
    err |= nvs_set_u16(h, SSJ_KEY_RUN_MIN, (uint16_t)safe.run_min);
    err |= nvs_set_u16(h, SSJ_KEY_REST_MIN, (uint16_t)safe.rest_min);
    err |= nvs_set_u8(h, SSJ_KEY_MUSIC, safe.music ? 1 : 0);
    err |= nvs_set_u8(h, SSJ_KEY_FLASH, safe.flash);
    err |= nvs_set_u8(h, SSJ_KEY_VOLUME, safe.volume);
    err |= nvs_set_u8(h, SSJ_KEY_HR, safe.hr_enabled ? 1 : 0);
    err |= nvs_set_u16(h, SSJ_KEY_HR_MAX, (uint16_t)safe.hr_max);
    err |= nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 写入失败: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
