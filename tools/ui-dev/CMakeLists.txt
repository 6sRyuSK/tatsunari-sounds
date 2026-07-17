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
else()
  message(WARNING "gallery is a WASM target; configure with `emcmake cmake ...` "
                  "(this non-Emscripten configure will not produce a runnable page).")
endif()
