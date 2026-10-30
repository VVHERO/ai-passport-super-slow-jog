// main/ssj_hr.c —— BLE 心率(CENTRAL / HRS 客户端)实现。
//
// 状态机(控制任务每 250ms 走一格):
//   关闭 ──enable──> 启动协议栈 ──sync──> 扫描(8s) ──发现 0x180D──> 连接
//   连接 ──订阅 0x2A37──> 收心率(1Hz) ──断线/超时──> 3s 后重扫
//   扫描 8s 没找到 ──> 静默 25s ──> 再扫(省电,不至于一直满负荷扫)
//
// 关键取舍:
//   - 只认标准 HRS,不碰 Zepp 私有协议:Amazfit 的「心率推送」就是标准 HRS。
//   - 扫描按广告里的 0x180D 过滤;手表地址是静态随机地址,可能变,所以绝不硬编码。
//   - 发现候选后必须先 ble_gap_disc_cancel 再 ble_gap_connect(NimBLE 不允许边扫边连)。
#include "ssj_hr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

static const char *TAG = "ssj_hr";

#define SSJ_HR_SERVICE_UUID 0x180D
#define SSJ_HR_MEAS_UUID 0x2A37

#define SSJ_HR_SCAN_WINDOW_MS 8000     // 一次扫描多久
#define SSJ_HR_SCAN_IDLE_MS 25000      // 两轮扫描之间静默多久(省电)
#define SSJ_HR_RECONNECT_MS 3000       // 断线后多久重连
#define SSJ_HR_STALE_MS 15000          // 多久没数据就认为链路挂了
#define SSJ_HR_TICK_MS 250

static volatile bool s_enabled;
static volatile ssj_hr_state_t s_state = SSJ_HR_OFF;
static volatile bool s_synced;
static volatile bool s_scanning;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t s_hr_val_handle;
static volatile int s_bpm;
static volatile int64_t s_bpm_us;
static volatile bool s_candidate;
static volatile bool s_hr_svc_found;
static volatile int64_t s_next_action_us;
static volatile int64_t s_scan_started_us;

static ble_addr_t s_peer;
static uint8_t s_own_addr_type;
static bool s_inited;
static TaskHandle_t s_task;

static int gap_event(struct ble_gap_event *event, void *arg);
static bool hr_init_once(void);

// 广告名里出现这些字样也当作候选(有的手表只在开启推送后才在广告里带 0x180D,
// 先连上去看看有没有心率服务,没有就断开——代价只是几秒)。
static bool name_looks_like_watch(const uint8_t *name, uint8_t len) {
    static const char *keys[] = {"amazfit", "zepp", "hyperate"};
    if (!name || len == 0) return false;
    char lower[32];
    const uint8_t n = len < sizeof(lower) - 1 ? len : (uint8_t)(sizeof(lower) - 1);
    for (uint8_t i = 0; i < n; i++) {
        char c = (char)name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[n] = '\0';
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        if (strstr(lower, keys[k]) != NULL) return true;
    }
    return false;
}

// Amazfit/Zepp 的广告包常常被厂商数据塞满 31 字节,既没有 0x180D 也没有名字
// (实测 Amazfit Balance 就是 `02 01 02 | 1b ff 57 01 ...`,公司 ID 0x0157 = 安徽华米)。
// 所以「华米公司 ID」也是候选条件:连上去再看有没有心率服务。
#define SSJ_HR_HUAMI_COMPANY_ID 0x0157

static bool mfg_looks_like_huami(const struct ble_hs_adv_fields *f) {
    if (!f->mfg_data || f->mfg_data_len < 2) return false;
    const uint16_t company = (uint16_t)f->mfg_data[0] | ((uint16_t)f->mfg_data[1] << 8);
    return company == SSJ_HR_HUAMI_COMPANY_ID;
}

// 连上去发现没有心率服务的设备,记进"暂时别再试"的名单,避免连着扫到它反复重连。
#define SSJ_HR_BAD_MAX 4
#define SSJ_HR_BAD_COOLDOWN_US (120LL * 1000 * 1000)
static ble_addr_t s_bad[SSJ_HR_BAD_MAX];
static int64_t s_bad_us[SSJ_HR_BAD_MAX];
static int s_bad_next;

static bool peer_in_bad_list(const ble_addr_t *addr) {
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < SSJ_HR_BAD_MAX; i++) {
        if (s_bad_us[i] == 0) continue;
        if (now - s_bad_us[i] > SSJ_HR_BAD_COOLDOWN_US) continue;
        if (s_bad[i].type == addr->type && memcmp(s_bad[i].val, addr->val, 6) == 0) return true;
    }
    return false;
}

static void peer_mark_bad(const ble_addr_t *addr) {
    s_bad[s_bad_next] = *addr;
    s_bad_us[s_bad_next] = esp_timer_get_time();
    s_bad_next = (s_bad_next + 1) % SSJ_HR_BAD_MAX;
}

// ---------------------------------------------------------------------------
// 扫描 / 连接
// ---------------------------------------------------------------------------
static void start_scan(void) {
    struct ble_gap_disc_params params = {0};
    // ⚠ 必须给明确的 scan interval/window。ESP32 开了扩展广播(BLE_EXT_ADV)时,
    // ble_gap_disc() 走 ext_disc 分支且【不填默认值】;传 0/0 会让控制器几乎不扫描
    // (只见启动瞬间的几个包,之后静默,也不会有 DISC_COMPLETE)。
    // 60ms/60ms = 连续扫描;只在一轮 8 秒里开,整体功耗可控。
    params.itvl = 0x0060;
    params.window = 0x0060;
    params.filter_policy = 0;
    params.limited = 0;
    params.passive = 0;
    params.filter_duplicates = 1;
    s_candidate = false;
    int rc = ble_gap_disc(s_own_addr_type, SSJ_HR_SCAN_WINDOW_MS, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "开始扫描失败: %d", rc);
        s_next_action_us = esp_timer_get_time() + (int64_t)SSJ_HR_RECONNECT_MS * 1000;
        return;
    }
    s_scan_started_us = esp_timer_get_time();
    s_scanning = true;
    s_state = SSJ_HR_SEARCHING;
    ESP_LOGI(TAG, "开始扫描心率设备");
}

static void start_connect(void) {
    // 连接参数:400–500ms 间隔,8s 监督超时。心率 1Hz 通知,没必要更快。
    struct ble_gap_conn_params cp = {0};
    cp.scan_itvl = 0x0010;
    cp.scan_window = 0x0010;
    cp.itvl_min = 0x0190;
    cp.itvl_max = 0x01F4;
    cp.latency = 0;
    cp.supervision_timeout = 0x0320;   // 800 * 10ms = 8s
    cp.min_ce_len = 0;
    cp.max_ce_len = 0;

    ESP_LOGI(TAG, "连接 %02x:%02x:%02x:%02x:%02x:%02x (type=%u)",
             s_peer.val[5], s_peer.val[4], s_peer.val[3],
             s_peer.val[2], s_peer.val[1], s_peer.val[0], (unsigned)s_peer.type);
    int rc = ble_gap_connect(s_own_addr_type, &s_peer, 10000, &cp, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "连接失败: %d,稍后重扫", rc);
        s_next_action_us = esp_timer_get_time() + (int64_t)SSJ_HR_RECONNECT_MS * 1000;
    } else {
        // 连接正在建立(最多 10s):这段时间别再去开扫描,否则 ble_gap_disc 返回 EBUSY。
        s_next_action_us = esp_timer_get_time() + 10LL * 1000 * 1000;
    }
}

// ---------------------------------------------------------------------------
// GATT 发现与订阅
// ---------------------------------------------------------------------------
static int on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg) {
    (void)arg;
    if (error->status == 0 && chr) {
        if (ble_uuid_u16(&chr->uuid.u) == SSJ_HR_MEAS_UUID) {
            s_hr_val_handle = chr->val_handle;
            // CCCD = 值句柄 + 1;写 0x0001 打开通知。
            uint8_t cccd[2] = {0x01, 0x00};
            int rc = ble_gattc_write_flat(conn_handle, chr->val_handle + 1, cccd, sizeof(cccd),
                                          NULL, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "订阅 0x2A37 失败: %d", rc);
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            } else {
                ESP_LOGI(TAG, "已订阅心率通知");
            }
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_hr_val_handle == 0) {
            ESP_LOGW(TAG, "对端没有 0x2A37,断开重扫");
            peer_mark_bad(&s_peer);
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

static int on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg) {
    (void)arg;
    if (error->status == 0 && service) {
        s_hr_svc_found = true;
        int rc = ble_gattc_disc_all_chrs(conn_handle, service->start_handle,
                                         service->end_handle, on_chr_disc, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "特征发现失败: %d", rc);
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        // ⚠ EDONE 是"发现流程结束",不是"没找到":找到服务后同样会收到 EDONE。
        // 只有从头到尾一个服务都没匹配上,才算对端没有心率服务。
        if (!s_hr_svc_found) {
            ESP_LOGW(TAG, "对端没有心率服务,断开重扫");
            peer_mark_bad(&s_peer);
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 心率数据解析:flags 的 bit0 决定 8/16 位;本例手表是 8 位。
// ---------------------------------------------------------------------------
static void on_hr_measurement(const struct os_mbuf *om) {
    uint8_t buf[8];
    uint16_t len = OS_MBUF_PKTLEN(om);
    if (len < 2) return;
    if (len > sizeof(buf)) len = sizeof(buf);
    if (os_mbuf_copydata(om, 0, len, buf) != 0) return;

    const uint8_t flags = buf[0];
    int bpm;
    if (flags & 0x01) {
        if (len < 3) return;
        bpm = (int)(buf[1] | ((uint16_t)buf[2] << 8));
    } else {
        bpm = (int)buf[1];
    }
    if (bpm <= 0 || bpm > 250) return;   // 明显不合法的值直接丢
    s_bpm = bpm;
    s_bpm_us = esp_timer_get_time();
}

// ---------------------------------------------------------------------------
// GAP 事件
// ---------------------------------------------------------------------------
static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) break;
            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
                break;
            }
            bool has_hrs_uuid = false;
            for (int i = 0; i < fields.num_uuids16; i++) {
                if (ble_uuid_u16(&fields.uuids16[i].u) == SSJ_HR_SERVICE_UUID) has_hrs_uuid = true;
            }
            bool hit = has_hrs_uuid;
            if (!hit) {
                // 广告里没有 0x180D 时,按名字/厂商 ID 猜:连上去再看有没有心率服务。
                // 猜错的设备会进 2 分钟的冷却名单,不会反复重连。
                const bool guessed = name_looks_like_watch(fields.name, fields.name_len) ||
                                     (mfg_looks_like_huami(&fields) && event->disc.rssi >= -80);
                if (guessed && peer_in_bad_list(&event->disc.addr)) hit = false;
                else hit = guessed;
            }
#ifdef SSJ_SELFTEST
            static int dbg_reports;
            if (!hit && dbg_reports < 40 && event->disc.rssi >= -75) {
                dbg_reports++;
                char hex[3 * 17];
                const int n = event->disc.length_data < 16 ? event->disc.length_data : 16;
                for (int i = 0; i < n; i++) {
                    static const char *hexd = "0123456789abcdef";
                    hex[i * 3 + 0] = hexd[(event->disc.data[i] >> 4) & 0xF];
                    hex[i * 3 + 1] = hexd[event->disc.data[i] & 0xF];
                    hex[i * 3 + 2] = ' ';
                }
                hex[n * 3] = '\0';
                ESP_LOGI(TAG, "SELFTEST rpt %02x:%02x:%02x:%02x:%02x:%02x ev=%d rssi=%d len=%d u16=%d raw=%s",
                         event->disc.addr.val[5], event->disc.addr.val[4], event->disc.addr.val[3],
                         event->disc.addr.val[2], event->disc.addr.val[1], event->disc.addr.val[0],
                         (int)event->disc.event_type, (int)event->disc.rssi,
                         (int)event->disc.length_data, (int)fields.num_uuids16, hex);
            }
#endif
            if (!hit) break;
            s_peer = event->disc.addr;
            s_candidate = true;
            break;
        }
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn = event->connect.conn_handle;
                s_state = SSJ_HR_CONNECTED;
                s_hr_val_handle = 0;
                s_hr_svc_found = false;
                ESP_LOGI(TAG, "已连接心率设备,开始发现服务");
                int rc = ble_gattc_disc_svc_by_uuid(s_conn, BLE_UUID16_DECLARE(SSJ_HR_SERVICE_UUID),
                                                    on_svc_disc, NULL);
                if (rc != 0) {
                    ESP_LOGW(TAG, "服务发现失败: %d", rc);
                    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
                }
            } else {
                ESP_LOGW(TAG, "连接未成功: %d", event->connect.status);
                s_conn = BLE_HS_CONN_HANDLE_NONE;
                s_state = s_enabled ? SSJ_HR_SEARCHING : SSJ_HR_OFF;
                s_next_action_us = esp_timer_get_time() + (int64_t)SSJ_HR_RECONNECT_MS * 1000;
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "心率设备断开: reason=%d", event->disconnect.reason);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_hr_val_handle = 0;
            s_state = s_enabled ? SSJ_HR_SEARCHING : SSJ_HR_OFF;
            s_next_action_us = esp_timer_get_time() + (int64_t)SSJ_HR_RECONNECT_MS * 1000;
            break;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            s_scanning = false;
#ifdef SSJ_SELFTEST
            ESP_LOGI(TAG, "SELFTEST 扫描结束 reason=%d", event->disc_complete.reason);
#endif
            if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
                s_state = s_enabled ? SSJ_HR_SEARCHING : SSJ_HR_OFF;
                if (!s_candidate) {
                    // 这一轮没找到:静默一段时间再扫,别一直满负荷扫。
                    s_next_action_us = esp_timer_get_time() + (int64_t)SSJ_HR_SCAN_IDLE_MS * 1000;
                }
            }
            break;
        case BLE_GAP_EVENT_NOTIFY_RX:
            if (event->notify_rx.attr_handle == s_hr_val_handle) {
                on_hr_measurement(event->notify_rx.om);
            }
            break;
        default:
            break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 控制任务
// ---------------------------------------------------------------------------
static void hr_control_task(void *arg) {
    (void)arg;
    if (!hr_init_once()) {
        s_enabled = false;
        s_state = SSJ_HR_OFF;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SSJ_HR_TICK_MS));
        const int64_t now = esp_timer_get_time();
#ifdef SSJ_SELFTEST
        {
            static int64_t last_hb;
            if (now - last_hb >= 10LL * 1000 * 1000) {
                last_hb = now;
                ESP_LOGI(TAG, "SELFTEST hb en=%d sync=%d scan=%d conn=%u cand=%d next=%lldms",
                         (int)s_enabled, (int)s_synced, (int)s_scanning, (unsigned)s_conn,
                         (int)s_candidate,
                         (long long)((s_next_action_us - now) / 1000));
            }
        }
#endif

        if (!s_enabled) {
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            }
            if (s_scanning) {
                ble_gap_disc_cancel();
                s_scanning = false;
            }
            continue;
        }
        if (!s_synced) continue;

        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            // 连着但长时间没数据(手表停止广播/超出范围):断开重来。
            if (s_bpm_us != 0 && now - s_bpm_us > (int64_t)SSJ_HR_STALE_MS * 1000) {
                ESP_LOGW(TAG, "心率数据超时,断开重连");
                ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            }
            continue;
        }

        if (s_candidate) {
            if (s_scanning) {
                // NimBLE 不允许边扫边连。这里不依赖 DISC_COMPLETE(实测取消扫描后
                // 这条事件不一定回来),直接把状态改成"已停扫",下一 tick 就连接;
                // 万一控制器还没停完,ble_gap_connect 会返回 EBUSY,按 3 秒退避重试。
                ble_gap_disc_cancel();
                s_scanning = false;
                s_next_action_us = now + 100LL * 1000;
                continue;
            }
            if (now >= s_next_action_us) {
                s_candidate = false;
                start_connect();
            }
            continue;
        }
        if (!s_scanning && now >= s_next_action_us) {
            start_scan();
        } else if (s_scanning && now - s_scan_started_us > (int64_t)(SSJ_HR_SCAN_WINDOW_MS + 5000) * 1000) {
            // 兜底:正常应该在扫描窗口到点时收到 DISC_COMPLETE;万一没收到,
            // 别让状态卡在"扫描中"永远不重试。
            ESP_LOGW(TAG, "扫描超时未结束,强制取消后重试");
            ble_gap_disc_cancel();
            s_scanning = false;
            s_next_action_us = now + (int64_t)SSJ_HR_RECONNECT_MS * 1000;
        }
    }
}

// ---------------------------------------------------------------------------
// 协议栈初始化与生命周期
// ---------------------------------------------------------------------------
// NimBLE host 任务入口:跑协议栈事件循环,退出后自删。
// (不能给 nimble_port_freertos_init 传 NULL,见 hr_init_once 的注释。)
static void hr_host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    vTaskDelete(NULL);
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE 复位: %d", reason);
    s_synced = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_scanning = false;
    s_state = s_enabled ? SSJ_HR_STARTING : SSJ_HR_OFF;
}

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "取本机 BLE 地址失败: %d", rc);
        return;
    }
    s_synced = true;
    s_state = s_enabled ? SSJ_HR_SEARCHING : SSJ_HR_OFF;
    s_next_action_us = esp_timer_get_time();
    ESP_LOGI(TAG, "NimBLE 就绪(addr_type=%u)", (unsigned)s_own_addr_type);
}

static bool hr_init_once(void) {
    if (s_inited) return true;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return false;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    // 注意:必须传真正的 host 任务函数。IDF 的 nimble_port_freertos_init() 直接把
    // 参数交给 xTaskCreatePinnedToCore 当入口,传 NULL 会在任务第一次被调度时
    // 跳转到地址 0(表现为 Instruction access fault 反复重启)。
    nimble_port_freertos_init(hr_host_task);
    s_inited = true;
    ESP_LOGI(TAG, "BLE 心率模块已初始化");
    return true;
}

void ssj_hr_set_enabled(bool on) {
    if (on) {
        s_enabled = true;
        s_next_action_us = esp_timer_get_time();
        if (!s_task) {
            // 协议栈初始化放在控制任务里做:调用方通常是持有 LVGL 锁的 UI 上下文,
            // 不能在这里阻塞几百毫秒。
            if (xTaskCreate(hr_control_task, "ssj_hr", 3584, NULL, 3, &s_task) != pdPASS) {
                ESP_LOGE(TAG, "心率任务创建失败");
                s_enabled = false;
                s_state = SSJ_HR_OFF;
                return;
            }
        }
        s_state = s_inited && s_synced ? SSJ_HR_SEARCHING : SSJ_HR_STARTING;
        ESP_LOGI(TAG, "心率接收:开");
    } else {
        s_enabled = false;
        s_state = SSJ_HR_OFF;
        s_bpm = 0;
        s_bpm_us = 0;
        ESP_LOGI(TAG, "心率接收:关");
    }
}

ssj_hr_state_t ssj_hr_state(void) {
    return s_state;
}

bool ssj_hr_latest(int *bpm, int64_t *age_ms) {
    const int64_t stamp = s_bpm_us;
    if (stamp == 0) return false;
    const int64_t age_us = esp_timer_get_time() - stamp;
    if (age_us > (int64_t)SSJ_HR_STALE_MS * 1000) return false;
    if (bpm) *bpm = s_bpm;
    if (age_ms) *age_ms = age_us / 1000;
    return true;
}
