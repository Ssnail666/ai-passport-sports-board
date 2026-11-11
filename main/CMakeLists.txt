idf_component_register(
    SRCS "main.c"
         "sports_app.c"
         "sports_model.c"
         "sports_store.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer nvs_flash
)

# 中文由 tools/gen_sports_fonts.py 从 Noto Sans SC 生成固定子集，覆盖界面里
# 实际出现的全部字符。字体数据编译进 Flash，不占运行内存；大号数字使用
# sdkconfig.defaults 启用的 Montserrat 40。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/sports_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/sports_font_20.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/sports_font_28.c"
)
