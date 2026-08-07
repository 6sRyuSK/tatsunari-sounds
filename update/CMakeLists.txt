# factory_update — badge-side update check (HTTP + state machine + prefs).
# Visage-free / JUCE-free. Linked only by clap GUI shells (and its own tests).

add_library(factory_update STATIC
  src/LatestDocument.cpp
  src/UpdatePrefs.cpp
  src/UpdateCheck.cpp
  src/InstallerLocator.cpp
)

if(APPLE)
  target_sources(factory_update PRIVATE src/HttpTransport_mac.mm)
  target_link_libraries(factory_update PUBLIC "-framework Foundation")
elseif(WIN32)
  target_sources(factory_update PRIVATE src/HttpTransport_win.cpp)
  target_link_libraries(factory_update PUBLIC winhttp)
else()
  target_sources(factory_update PRIVATE src/HttpTransport_stub.cpp)
endif()

target_include_directories(factory_update PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(factory_update PUBLIC cxx_std_20)
set_target_properties(factory_update PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Headless unit tests (FakeHttpTransport; no network).
add_executable(factory_update_test tests/update_check_test.cpp)
target_link_libraries(factory_update_test PRIVATE factory_update)
target_compile_features(factory_update_test PRIVATE cxx_std_20)

# Fixture directory (shared with the Go installer package).
set(_UPDATE_FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/../tools/installer/testdata/updates/v1)
add_test(NAME factory_update COMMAND factory_update_test ${_UPDATE_FIXTURES})
