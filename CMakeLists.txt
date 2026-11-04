cmake_minimum_required(VERSION 3.20)
project(sdvx_training CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(GAME_DLL "$ENV{GAME_DLL}" CACHE FILEPATH "the game's modules/soundvoltex.dll (game tests, install)")

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
add_link_options(-static)
include_directories(include)

add_library(core OBJECT
  src/core/core.cpp src/core/voices.cpp src/core/speed.cpp src/core/judge.cpp src/core/guard.cpp
  src/core/camera.cpp src/core/effects.cpp src/game.cpp src/hud.cpp src/log.cpp src/panel.cpp
  src/remote.cpp src/rtti.cpp src/trainer.cpp src/vox.cpp)
target_link_libraries(core PUBLIC gdi32)

add_library(sdvx_training SHARED src/dllmain.cpp)
target_link_libraries(sdvx_training PRIVATE core)
set_target_properties(sdvx_training PROPERTIES PREFIX "")

# tests
enable_testing()
add_executable(test_unit tests/test_unit.cpp src/trainer.cpp src/vox.cpp)
add_executable(test_overlay tests/test_overlay.cpp src/hud.cpp src/panel.cpp src/remote.cpp src/log.cpp)
target_link_libraries(test_overlay gdi32)
add_executable(test_resolve tests/test_resolve.cpp src/game.cpp src/rtti.cpp)
add_executable(smoke_host tests/smoke_host.cpp src/game.cpp src/rtti.cpp)
add_executable(test_frame tests/test_frame.cpp)
target_link_libraries(test_frame core)
foreach(t test_overlay test_resolve smoke_host test_frame)
  target_link_options(${t} PRIVATE -municode)
endforeach()

set(VOX_EXPECTED "" CACHE FILEPATH "chart timing table (tools/vox_expected.txt in the parent repo); its charts must be at the paths it lists")
add_test(NAME unit COMMAND test_unit ${VOX_EXPECTED})
add_test(NAME overlay COMMAND test_overlay)
if(EXISTS "${GAME_DLL}")  # these load the game's own dll
  add_test(NAME resolve COMMAND test_resolve "${GAME_DLL}")
  add_test(NAME frame COMMAND test_frame "${GAME_DLL}")
  add_test(NAME smoke COMMAND smoke_host "${GAME_DLL}" "$<TARGET_FILE:sdvx_training>")
endif()

# install: the dll next to spice64.exe, the ini only if none is there yet
if(GAME_DLL)
  cmake_path(GET GAME_DLL PARENT_PATH GAME_ROOT)
  cmake_path(GET GAME_ROOT PARENT_PATH GAME_ROOT)
  install(TARGETS sdvx_training RUNTIME DESTINATION "${GAME_ROOT}")
  install(CODE "if(NOT EXISTS \"${GAME_ROOT}/sdvx_training.ini\")
    file(COPY \"${CMAKE_SOURCE_DIR}/sdvx_training.ini\" DESTINATION \"${GAME_ROOT}\")
  endif()")
endif()
