# tools/ui-dev — STANDALONE project that builds the Visage widget gallery to
# WASM for the daily UI-development loop. This is the project `emcmake` configures
# (it is not part of the repository-root build). It consumes the design-system
# library from ui/visage and the header-only params/core modules.

cmake_minimum_required(VERSION 3.17)
project(factory_ui_dev CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The design-system library owns the pinned + sandbox-patched visage dependency,
# so all this project has to do is add it and link.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../ui/visage
                 ${CMAKE_BINARY_DIR}/factory_ui_visage)

add_executable(gallery
  gallery/main.cpp
  gallery/GalleryFrame.cpp
  gallery/Bridge.cpp
)

# factory_ui_visage brings visage + the params/core includes transitively; the
# params/core includes are also listed here to document the consumer's intent.
target_link_libraries(gallery PRIVATE factory_ui_visage)
target_include_directories(gallery PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/common     # BridgeCommon.h (shared bridge bodies)
  ${CMAKE_CURRENT_SOURCE_DIR}/../../params/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)

# --- rs-editor: the Phase P3 resonance-suppressor editor, JUCE-free, bound to the
#     shared design system + a synthetic feed + preset/A-B mocks (same harness
#     pattern as the gallery). It composes the RS-specific UI in
#     plugins/resonance-suppressor/ui/ (JUCE-free) with the REAL 64-param table
#     from that plugin's Source/Params.h.
set(RS_PLUGIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/resonance-suppressor)

add_executable(rs-editor
  rs-editor/main.cpp
  rs-editor/RsBridge.cpp
  ${RS_PLUGIN_DIR}/ui/RsEditor.cpp
  ${RS_PLUGIN_DIR}/ui/RsSuppressionCurveView.cpp
  ${RS_PLUGIN_DIR}/ui/RsNodePanel.cpp
)
target_link_libraries(rs-editor PRIVATE factory_ui_visage)
target_include_directories(rs-editor PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/common     # BridgeCommon.h (shared bridge bodies)
  ${CMAKE_CURRENT_SOURCE_DIR}/rs-editor
  ${RS_PLUGIN_DIR}/ui
  ${RS_PLUGIN_DIR}/Source            # Params.h (buildRsParams — JUCE-free)
  ${CMAKE_CURRENT_SOURCE_DIR}/../../params/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)

if(EMSCRIPTEN)
  set(FACTORY_UI_DEV_SHELL ${CMAKE_CURRENT_SOURCE_DIR}/shell.html)

  target_link_options(gallery PRIVATE
    --shell-file ${FACTORY_UI_DEV_SHELL}
    -sGL_ENABLE_GET_PROC_ADDRESS
    -sALLOW_MEMORY_GROWTH
    --bind
    "-sEXPORTED_FUNCTIONS=['_main','_pasteCallback','_ui_list_params','_ui_get_param','_ui_set_param','_ui_freeze','_ui_reload_theme','_ui_last_error','_ui_get_accent','_ui_widget_x','_ui_widget_y','_ui_widget_rect','_ui_set_font','_ui_font','_ui_feed_spectrum','_ui_open_dropdown','_ui_dropdown_open','_ui_dropdown_item_count','_ui_dropdown_x','_ui_dropdown_row_y','_ui_preset_index','_ui_last_mouse_x','_ui_last_mouse_y']"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString']"
  )

  # Relink when the shell page changes.
  set_target_properties(gallery PROPERTIES LINK_DEPENDS ${FACTORY_UI_DEV_SHELL})

  # index.html/.js/.wasm land in <build>/web — the dir dev_server.py serves.
  set_target_properties(gallery PROPERTIES
    SUFFIX ".html"
    OUTPUT_NAME "index"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
  )

  target_link_options(rs-editor PRIVATE
    --shell-file ${FACTORY_UI_DEV_SHELL}
    -sGL_ENABLE_GET_PROC_ADDRESS
    -sALLOW_MEMORY_GROWTH
    --bind
    "-sEXPORTED_FUNCTIONS=['_main','_pasteCallback','_ui_list_params','_ui_get_param','_ui_set_param','_ui_freeze','_ui_reload_theme','_ui_last_error','_ui_get_accent','_ui_widget_rect','_ui_set_font','_ui_font','_rs_select_node','_rs_selected_node','_rs_node_x','_rs_node_y','_rs_listen_node','_rs_display_smooth','_rs_ab_slot','_rs_set_ab','_rs_copy_ab','_rs_preset_index','_rs_preset_load','_rs_ui_edit','_rs_undo','_rs_redo','_rs_can_undo','_rs_can_redo','_rs_set_clock','_rs_pump','_rs_open_dropdown','_rs_dropdown_open','_rs_dropdown_count','_rs_dropdown_x','_rs_dropdown_row_y','_rs_plot_rect','_rs_set_size','_rs_profile_db_at','_rs_mini_knob_tip']"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString']"
  )
  set_target_properties(rs-editor PROPERTIES LINK_DEPENDS ${FACTORY_UI_DEV_SHELL})
  # rs-editor lands in <build>/web-rs so it never collides with the gallery's web/.
  set_target_properties(rs-editor PROPERTIES
    SUFFIX ".html"
    OUTPUT_NAME "index"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web-rs"
  )
else()
  message(WARNING "gallery/rs-editor are WASM targets; configure with `emcmake cmake ...` "
                  "(this non-Emscripten configure will not produce a runnable page).")
endif()
