<p align="right">
  <strong>简体中文</strong> · <a href="README.en.md">English</a>
</p>

# 超慢跑 · Super Slow Jog

把 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) 变成随身的节拍陪跑员。

180 步/分是超慢跑最舒服的节奏 —— 小步幅、低心率、还能边跑边说话。这台小机器只做一件事：把拍子稳稳打在你耳边。屏幕上一颗绿点跟着节拍亮灭，耳朵听、眼睛看，脚步自然就合上了。

打开音乐后，它还会整段播放 180 步/分的伴跑音乐，重拍与节拍提示音、屏幕明暗严格同步：音乐一拍，屏就暗一下，你甚至可以不戴耳机，只看屏幕上的明暗节奏跑。

<img src="docs/images/cover.jpg" width="360" alt="超慢跑封面">

## 两种跑法

| 模式 | 参数 | 说明 |
|---|---|---|
| 自由跑 | 时长 1–120 分钟 | 到点自动收工，不用一直盯着表 |
| 间歇跑 | 组数 1–20 / 跑 1–30 分 / 歇 1–10 分 | 跑和歇用不同音高区分，切换先响三声提示 |

步频 **120–200 步/分** 可调，每次 +2，到上限回绕到下限。所有参数存在 NVS，关机、断电都不丢。

## 伴跑音乐

- 内置 400 秒 180 步/分伴跑音乐（33 分钟原曲中截取的一段连续混音，首尾无缝循环）
- 音乐开着时步频固定 180 步/分（音乐速度即步频），主页不再显示「步频」行
- 暂停时音乐一起停，继续时从暂停处接着播，不会从头重来
- 节拍提示音轻轻叠在音乐重拍上，想安静跑可以把音量调到 0
- 音乐与节拍共用同一条音频通路，相位天然对齐，不会出现「人和鼓差半拍」

## 随拍闪屏

设置里可切 `关 / 柔 / 强` 三档：每来一拍，整屏背光短暂压暗（柔档 45% 亮度、强档 8% 亮度，约 110 ms 后恢复），跑步时用余光就能跟上节奏。默认「柔」，想更醒目就切「强」。

## 界面

| 运行页 | 主页 |
|---|---|
| <img src="docs/images/run.jpg" width="300"> | <img src="docs/images/home.jpg" width="300"> |

运行页从上到下：当前阶段（跑步中 / 休息中 / 已暂停）、阶段剩余时间（仅间歇跑）、本次总时长 `分:秒`、已走步数、跟着节拍亮灭的圆点（跑步绿 / 休息蓝 / 暂停橙且停住）、当前可用按键提示。右上角是电量百分比，读不到电量时这一角留空，不影响使用。

## 按键

**主页**

- 上 / 下：在参数行之间移动，选中行底色变深、文字变绿
- OK（在参数行上）：把当前值往上调一档，到顶循环回底
- OK（在「开始」行上）：立刻开始
- OK 长按（约半秒）：进设置页

**设置页**（音乐 / 闪屏 / 音量 / 返回）

- 上 / 下移动，OK 切换当前项：音乐开关、闪屏 `关 → 柔 → 强`、音量 0–100% 步进 10
- OK 在「返回」上、或任意位置长按 OK：回主页并保存

**运行中**

- OK 短按：暂停 / 继续，暂停期间计时不增长、音乐一起停
- OK 长按（约半秒）：结束本次训练，进入成绩页
- 上 / 下：音量 ±10%，屏幕浮出百分比，一秒多后消失

**成绩页**：标题显示「完成!」或「已结束」，下面是总时长与步数，按 OK 回主页并保留设置。

## 声音

- 22.05 kHz / 16 bit / 单声道 PCM 输出，节拍音全部查表合成
- 跑步阶段拍子音高，休息阶段偏低
- 间歇模式切换阶段前先响三声提示音，再换新阶段拍子
- 全部练完响一段上扬双音，自动跳到成绩页
- 每拍带 4 ms 淡入淡出，避免爆音
- 节拍按绝对时间调度，长时间跑不会累积漂移
- 伴跑音乐为 4 bit IMA ADPCM 内嵌素材：解码只是查表和加法，不占 CPU，
  节拍音直接混进同一段 PCM，因此音乐重拍、节拍声、屏幕闪动三者同源同步

## 编译

需要 ESP-IDF **v5.5.3**，目标芯片 `esp32c3`。

```bash
. $IDF_PATH/export.sh          # Windows: %IDF_PATH%\export.bat
idf.py set-target esp32c3
idf.py build
```

`LVGL 9.5` 与其它依赖由 `dependencies.lock` 锁定，首次构建时自动拉取。

伴跑音乐素材不入库（版权归素材提供者），全新克隆后先生成一次，再构建：

```bash
python tools/pack_music.py "<180 BPM 源视频>" assets/music/ssj_music.adp \
  --start 428 --duration 400 --peak 0.72 --decode-check
```

缺少这个文件时 `main/CMakeLists.txt` 会在配置阶段直接报错并打印上面这条命令。

### 合并固件（从 0x0 刷写）

```bash
idf.py merge-bin -o FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build
```

`verify_firmware.py` 会检查镜像分区布局与大小是否符合设备要求。合并镜像直接从 **0x0** 刷入，覆盖 bootloader + 分区表 + 应用。

```bash
esptool.py --chip esp32c3 -p <PORT> write_flash 0x0 FoloToy-AI-Passport-full.bin
```

## 一键校验

```bash
./tools/validate.sh            # 静态检查 + 固件构建
./tools/validate.sh --static   # 只跑静态检查与主机测试
```

静态检查包含：

- `tools/check_repo.py` —— 仓库结构规则
- 纯 C 状态机主机测试（`tests/test_ssj_logic.c`），7 个用例覆盖参数钳制、会话生命周期、阶段切换、步数推算
- 中文字形覆盖测试（`tests/test_ssj_font_coverage.py`），确保 UI 里出现的每个汉字都在子集字体里

## 代码结构

```
main/
  ssj_logic.{c,h}    纯 C 状态机，不依赖硬件，可主机测试
  ssj_app.{c,h}      LVGL 界面（主页 / 运行 / 成绩 / 设置），节拍脉冲动画
  ssj_audio.{c,h}    节拍合成 + 音乐流播放 + 随拍闪屏，命令队列驱动
  ssj_store.{c,h}    NVS 参数持久化
  main.c             启动、按键队列、LVGL 锁
assets/fonts/        思源黑体子集（SIL OFL 1.1）+ 生成脚本
assets/music/        伴跑音乐 IMA ADPCM 素材（由 tools/pack_music.py 生成）
tests/               主机测试
tools/               构建、校验、归档、音乐打包
```

状态机与界面、音频、存储完全解耦：`ssj_logic.c` 只用标准 C，主机上就能编译运行，因此时序逻辑的回归不依赖真机。

## 硬件

ESP32-C3（8 MB Flash，无 PSRAM）、240×320 屏、3 个 ADC 按键、ES8311 音频编解码、CW2017 电量计。控制台走 USB-Serial-JTAG；UART0 的 TX 与背光共用 GPIO21，因此固件不使用 UART0。

## 协议与致谢

本项目基于 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)，遵循 **MIT License**，原始版权归 FoloToy 所有，见 [LICENSE](LICENSE)。

内置字体为思源黑体（Source Han Sans SC）子集，遵循 SIL Open Font License 1.1。

内置伴跑音乐由本玩法的创作者提供，版权归创作者所有，仅随本玩法固件分发，不单独授权再利用；重新生成方法见 [assets/README.zh_CN.md](assets/README.zh_CN.md)。

LED 脉动、节拍合成、界面排版均为本项目自行实现，未复用示例工程代码。
