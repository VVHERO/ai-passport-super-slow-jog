# 伴跑音乐:400 秒 180 步频素材(22.05 kHz 单声道 IMA ADPCM,约 4.2 MB),
# 由 tools/pack_music.py 生成;编进 app 的 .rodata.embedded,
# 播放时直接从 Flash 映射读取,不占 RAM。
#
# 素材本身含创作者的音乐,不纳入版本管理(见 assets/README.zh_CN.md):
# 全新克隆后必须先用 tools/pack_music.py 生成,否则这里直接给出明确报错,
# 而不是让 CMake 报一句 file not found。
set(SSJ_MUSIC_ADP "${CMAKE_CURRENT_LIST_DIR}/../assets/music/ssj_music.adp")
if(NOT EXISTS "${SSJ_MUSIC_ADP}")
    message(FATAL_ERROR
        "缺少伴跑音乐素材: ${SSJ_MUSIC_ADP}\n"
        "该文件不入库(版权归素材提供者)。请先生成:\n"
        "  python tools/pack_music.py \"<180 BPM 源视频>\" assets/music/ssj_music.adp "
        "--start 428 --duration 400 --peak 0.72 --decode-check")
endif()

idf_component_register(
    SRCS "main.c"
         "ssj_logic.c"
         "ssj_store.c"
         "ssj_audio.c"
         "ssj_app.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
    EMBED_FILES "${SSJ_MUSIC_ADP}"
)

# 超慢跑应用字体子集(assets/fonts 生成,见 docs/development/engineering/lvgl-chinese-fonts.zh_CN.md)。
# 由 lv_font_conv 从思源黑体生成,源文件纳入版本管理。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/ssj_font_20.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/ssj_font_48.c"
)

# 自检固件:开机 2 秒后自动开始一次会话,并打印每拍时间戳,用于在没有按键操作
# 的情况下验证音乐/节拍/闪屏链路。默认关闭,发布固件不含该逻辑。
# 用法:idf.py -B <build> -D SSJ_SELFTEST=ON build
option(SSJ_SELFTEST "Build a self-test firmware that starts a session automatically" OFF)
if(SSJ_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SSJ_SELFTEST=1)
    message(STATUS "SSJ_SELFTEST enabled: firmware will auto-start a session")
endif()
