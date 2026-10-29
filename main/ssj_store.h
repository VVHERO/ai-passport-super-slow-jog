// main/ssj_store.h —— 超慢跑配置持久化(NVS)。
// 配置量很小(几个标量),用独立键存储而非 blob:便于未来加字段且兼容旧数据。
// 读不到(首次启动/NVS 损坏)时返回出厂默认,由 ssj_config_clamp 兜底。
#pragma once

#include "ssj_logic.h"

// 从 NVS 命名空间 "ssj" 读取配置。返回 true = 读到完整合法配置;false = 使用了默认值。
// 无论返回值,出口配置保证已收敛到合法范围。
bool ssj_store_load(ssj_config_t *cfg);

// 保存配置到 NVS(含 commit)。返回 true = 保存成功。
// 保存失败只影响下次开机的记忆,不影响本次会话,调用方无需中断流程。
bool ssj_store_save(const ssj_config_t *cfg);
