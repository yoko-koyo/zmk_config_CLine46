# CLine46 独自の Zephyr モジュール
# ZMK 本体の include を参照するため APPLICATION_SOURCE_DIR (= zmk/app) を追加する

if(CONFIG_ZMK_INPUT_PROCESSOR_KEY_TAPPER OR CONFIG_ZMK_INPUT_PROCESSOR_SCROLL_INERTIA)
  zephyr_library()
  zephyr_library_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_KEY_TAPPER src/input_processor_key_tapper.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_SCROLL_INERTIA src/input_processor_scroll_inertia.c)
  zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()
