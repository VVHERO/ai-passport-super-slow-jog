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
)

# 超慢跑应用字体子集(assets/fonts 生成,见 docs/development/engineering/lvgl-chinese-fonts.zh_CN.md)。
# 由 lv_font_conv 从思源黑体生成,源文件纳入版本管理。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/ssj_font_20.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/ssj_font_48.c"
)
