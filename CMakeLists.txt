cmake_minimum_required(VERSION 3.20)
project(SimpleFPS CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Download and build raylib automatically (no manual install needed)
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
# raylib 5.5's own CMake files predate CMake 4; allow its older policy level
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(
    raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(raylib)

# ENet: small, reliable/unreliable UDP networking for multiplayer
FetchContent_Declare(
    enet
    URL https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(enet)

# Networking transport: the only code that includes ENet (and through it windows.h,
# which clashes with raylib's names), so it stays in its own files
# Project headers are included relative to src/ (e.g. "game/sim.hpp")
include_directories(src)

add_library(net_transport STATIC src/net/transport.cpp)
target_include_directories(net_transport PUBLIC src PRIVATE ${enet_SOURCE_DIR}/include)
target_link_libraries(net_transport PRIVATE enet)
if (WIN32)
    target_link_libraries(net_transport PRIVATE ws2_32 winmm)
endif()

add_executable(fps
    src/main.cpp
    src/game/sim.cpp
    src/game/session.cpp
    src/net/protocol.cpp
    src/soldier.cpp
    src/helmet.cpp
    src/player.cpp
    src/weapon.cpp
    src/world.cpp
    src/enemy.cpp
    src/viewmodel.cpp
    src/rifle_view.cpp
    src/weapon_models.cpp
    src/materials.cpp
    src/audio.cpp
    src/character.cpp
    src/render.cpp
    src/effects.cpp
    src/hud.cpp
)
target_link_libraries(fps PRIVATE raylib net_transport)

# Textures and models: copied next to the executable after every build
add_custom_command(TARGET fps POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:fps>/assets)

if (WIN32)
    target_link_libraries(fps PRIVATE winmm opengl32)
endif()

# Movement tests (run with: ctest --test-dir build --output-on-failure)
enable_testing()
add_executable(player_test tests/player_test.cpp src/player.cpp src/world.cpp)
target_link_libraries(player_test PRIVATE raylib)
if (WIN32)
    target_link_libraries(player_test PRIVATE winmm)
endif()
add_test(NAME player_test COMMAND player_test)

add_executable(enemy_test tests/enemy_test.cpp src/enemy.cpp src/world.cpp src/weapon.cpp src/player.cpp
    src/viewmodel.cpp src/rifle_view.cpp src/weapon_models.cpp src/character.cpp src/effects.cpp src/render.cpp src/materials.cpp
    src/soldier.cpp src/helmet.cpp)
target_link_libraries(enemy_test PRIVATE raylib)
if (WIN32)
    target_link_libraries(enemy_test PRIVATE winmm opengl32)
endif()
add_test(NAME enemy_test COMMAND enemy_test)

# Multiplayer: a host and a client in one process over loopback
add_executable(net_test tests/net_test.cpp src/game/sim.cpp src/game/session.cpp src/net/protocol.cpp
    src/enemy.cpp src/world.cpp src/weapon.cpp src/player.cpp src/weapon_models.cpp src/character.cpp src/render.cpp
    src/materials.cpp src/soldier.cpp src/helmet.cpp src/effects.cpp)
target_link_libraries(net_test PRIVATE raylib net_transport)
if (WIN32)
    target_link_libraries(net_test PRIVATE winmm opengl32)
endif()
add_test(NAME net_test COMMAND net_test)
