cmake_minimum_required(VERSION 3.20)
project(Afterglow VERSION 0.2.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
include(CTest)
find_package(Threads REQUIRED)

if(MSVC)
  set(AFTERGLOW_WARNINGS /W4)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  set(AFTERGLOW_WARNINGS -Wall -Wextra)
endif()

# ---- third party -------------------------------------------------------------
set(IMGUI third_party/imgui)
add_library(imgui STATIC ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp)
target_include_directories(imgui PUBLIC ${IMGUI})

# sokol (window, gfx, audio, imgui glue): one translation unit, Objective-C++ on macOS.
if(APPLE)
  set(SOKOL_IMPL_SRC src/platform/sokol_impl.mm)
else()
  set(SOKOL_IMPL_SRC src/platform/sokol_impl.cpp)
endif()
add_library(sokol STATIC ${SOKOL_IMPL_SRC})
target_include_directories(sokol PUBLIC third_party/sokol third_party/sokol/util)
target_link_libraries(sokol PUBLIC imgui Threads::Threads)
if(APPLE)
  set_source_files_properties(src/platform/sokol_impl.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
  target_link_libraries(sokol PUBLIC "-framework Cocoa" "-framework QuartzCore" "-framework Metal"
                                     "-framework MetalKit" "-framework AudioToolbox")
elseif(WIN32)
  target_link_libraries(sokol PUBLIC kernel32 user32 shell32 gdi32 ole32 d3d11 dxgi)
else()
  find_package(X11 REQUIRED)
  find_package(OpenGL REQUIRED)
  find_package(ALSA REQUIRED)
  target_link_libraries(sokol PUBLIC X11::X11 X11::Xi X11::Xcursor OpenGL::GL ALSA::ALSA ${CMAKE_DL_LIBS} m)
endif()

add_library(dr_libs STATIC src/platform/dr_impl.c)
target_include_directories(dr_libs PUBLIC third_party/dr_libs)

# ---- app core (no UI; also used by the tests) --------------------------------
add_library(afterglow_core STATIC src/Engine.cpp src/SetFile.cpp src/Transitions.cpp src/SynthGen.cpp src/TrackIO.cpp src/Library.cpp)
target_include_directories(afterglow_core PUBLIC src)
target_link_libraries(afterglow_core PUBLIC dr_libs Threads::Threads)
target_compile_options(afterglow_core PRIVATE ${AFTERGLOW_WARNINGS})

# Embed the UI font so the app looks the same everywhere and never depends on the launch directory.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/assets/Roboto-Regular.ttf" FONT_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," FONT_BYTES "${FONT_HEX}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/font_roboto.h"
     "#pragma once\ninline const unsigned char afterglow_font_roboto[] = {${FONT_BYTES}};\n")

# Embed the demo set; the app installs it into the user's sets folder on first run.
# Regenerate with: afterglow_set compose "assets/sets/Sunset to Jungle.set"
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/assets/sets/Sunset to Jungle.set" DEMO_SET_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," DEMO_SET_BYTES "${DEMO_SET_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/demo_set.h"
     "#pragma once\ninline const unsigned char afterglow_demo_set[] = {${DEMO_SET_BYTES}};\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/assets/sets/Sunset to Jungle.set" "${CMAKE_CURRENT_SOURCE_DIR}/assets/Roboto-Regular.ttf")

# ---- the app -----------------------------------------------------------------
set(APP_SOURCES src/main.cpp src/UIDecks.cpp src/UIPanels.cpp src/UISets.cpp src/Visuals.cpp src/Widgets.cpp)
if(APPLE)
  set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/Afterglow.icns")
  set_source_files_properties(${APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  list(APPEND APP_SOURCES ${APP_ICON})
endif()
add_executable(afterglow WIN32 MACOSX_BUNDLE ${APP_SOURCES})
target_include_directories(afterglow PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(afterglow PRIVATE afterglow_core sokol imgui)
target_compile_options(afterglow PRIVATE ${AFTERGLOW_WARNINGS})
set_target_properties(afterglow PROPERTIES
  OUTPUT_NAME Afterglow
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
  MACOSX_BUNDLE_BUNDLE_NAME Afterglow
  MACOSX_BUNDLE_GUI_IDENTIFIER com.afterglow.dj
  MACOSX_BUNDLE_ICON_FILE Afterglow.icns
  MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
install(TARGETS afterglow BUNDLE DESTINATION . RUNTIME DESTINATION bin)

# ---- tools -------------------------------------------------------------------
# Set tool: `afterglow_set compose <out.set> [out.wav]` records the scripted demo set,
# `afterglow_set render <in.set> <out.wav>` renders any recorded set.
add_executable(afterglow_set tools/afterglow_set.cpp)
target_link_libraries(afterglow_set PRIVATE afterglow_core)
target_compile_options(afterglow_set PRIVATE ${AFTERGLOW_WARNINGS})

# ---- tests -------------------------------------------------------------------
if(BUILD_TESTING)
  add_executable(afterglow_tests test/engine_test.cpp)
  target_link_libraries(afterglow_tests PRIVATE afterglow_core)
  add_test(NAME engine COMMAND afterglow_tests)
endif()
