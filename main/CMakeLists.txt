set(SOURCES "audio_codecs/audio_codec.cc"
            "audio_codecs/no_audio_codec.cc"
            "audio_codecs/box_audio_codec.cc"
            "audio_codecs/es8311_audio_codec.cc"
            "audio_codecs/es8388_audio_codec.cc"
            "led/single_led.cc"
            "led/circular_strip.cc"
            "led/gpio_led.cc"
            "display/display.cc"
            "display/lcd_display.cc"
            "display/oled_display.cc"
            "display/st7306_gfx.cc"
            "display/eink_display_st7306.cc"
            # "display/waveguide_display.cc"
            "display/eink/Adafruit_GFX.cpp"
            "st7306_driver.c"
            # "../reference code/src/jbd013_api.cpp"
            # "../reference code/src/hal_driver.cpp"
            "protocols/protocol.cc"
            "iot/thing.cc"
            "iot/thing_manager.cc"
            "iot/component_database.cc"
            "iot/component_led_control.cc"
            "iot/buzzer_control.cc"
            "iot/component_ws_server.cc"
            "iot/component_http_server.cc"
            "iot/match_game.cc"
            "system_info.cc"
            "application.cc"
            "ota.cc"
            "settings.cc"
            "background_task.cc"
            "main.cc"
            )

# set(INCLUDE_DIRS "." "display" "audio_codecs" "protocols" "audio_processing" "../reference code/src")
set(INCLUDE_DIRS "." "display" "audio_codecs" "protocols" "audio_processing")

# 添加 IOT 相关文件
file(GLOB IOT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/iot/things/*.cc)
list(APPEND SOURCES ${IOT_SOURCES})

# 添加板级公共文件
file(GLOB BOARD_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/boards/common/*.cc)
list(APPEND SOURCES ${BOARD_COMMON_SOURCES})
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/common)

# 根据 BOARD_TYPE 配置添加对应的板级文件
if(CONFIG_BOARD_TYPE_AI_PRINTER_LCD)
    set(BOARD_TYPE "ai-printer-lcd")
    # LCD板子需要复用ai-printer的打印服务
    list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/boards/ai-printer/printer_service.cc")
elseif(CONFIG_BOARD_TYPE_AI_PRINTER)
    set(BOARD_TYPE "ai-printer")
elseif(CONFIG_BOARD_TYPE_AI_PRINTER_VOICE)
    set(BOARD_TYPE "ai-printer-voice")
    # Voice板子需要复用ai-printer的打印服务
    list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/boards/ai-printer/printer_service.cc")
elseif(CONFIG_BOARD_TYPE_BREAD_TEST)
    set(BOARD_TYPE "bread-test")
elseif(CONFIG_BOARD_TYPE_BREAD_TEST_20L)
    set(BOARD_TYPE "bread-test-20l")
elseif(CONFIG_BOARD_TYPE_BREAD_TEST_MQTT)
    set(BOARD_TYPE "bread-test-mqtt")
elseif(CONFIG_BOARD_TYPE_COMPONENT_SORTER)
    set(BOARD_TYPE "component-sorter")
endif()
file(GLOB BOARD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE}/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE}/*.c
)
list(APPEND SOURCES ${BOARD_SOURCES})

if(CONFIG_CONNECTION_TYPE_MQTT_UDP)
    list(APPEND SOURCES "protocols/mqtt_protocol.cc")
elseif(CONFIG_CONNECTION_TYPE_WEBSOCKET)
    list(APPEND SOURCES "protocols/websocket_protocol.cc")
endif()

if(CONFIG_USE_AUDIO_PROCESSOR)
    list(APPEND SOURCES "audio_processing/audio_processor.cc")
endif()
if(CONFIG_USE_WAKE_WORD_DETECT)
    list(APPEND SOURCES "audio_processing/wake_word_detect.cc")
endif()

# 根据Kconfig选择语言目录
if(CONFIG_LANGUAGE_ZH_CN)
    set(LANG_DIR "zh-CN")
elseif(CONFIG_LANGUAGE_ZH_TW)
    set(LANG_DIR "zh-TW")
elseif(CONFIG_LANGUAGE_EN_US)
    set(LANG_DIR "en-US")
elseif(CONFIG_LANGUAGE_JA_JP)
    set(LANG_DIR "ja-JP")
elseif(CONFIG_LANGUAGE_TH_TH)
    set(LANG_DIR "th-TH")
endif()

# 定义生成路径
set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")
file(GLOB LANG_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/${LANG_DIR}/*.p3)
file(GLOB COMMON_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/common/*.p3)

# 如果目标芯片是 ESP32，则排除特定文件
if(CONFIG_IDF_TARGET_ESP32)
    list(REMOVE_ITEM SOURCES "audio_codecs/box_audio_codec.cc"
                             "audio_codecs/es8388_audio_codec.cc"
                             "led/gpio_led.cc"
                             )
endif()

idf_component_register(SRCS ${SOURCES}
                    EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS} "assets/jsQR.js"
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    )

# 使用 target_compile_definitions 来定义 BOARD_TYPE, BOARD_NAME
# 如果 BOARD_NAME 为空，则使用 BOARD_TYPE
if(NOT BOARD_NAME)
    set(BOARD_NAME ${BOARD_TYPE})
endif()
target_compile_definitions(${COMPONENT_LIB}
                    PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
                    )

# 添加生成规则
add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND python ${PROJECT_DIR}/scripts/gen_lang.py
            --input "${LANG_JSON}"
            --output "${LANG_HEADER}"
    DEPENDS
        ${LANG_JSON}
        ${PROJECT_DIR}/scripts/gen_lang.py
    COMMENT "Generating ${LANG_DIR} language config"
)

# 强制建立生成依赖
add_custom_target(lang_header ALL
    DEPENDS ${LANG_HEADER}
)
