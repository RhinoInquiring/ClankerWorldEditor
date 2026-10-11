# wow-world-editor: standalone world editor for WoW 3.3.5a (12340) + AzerothCore.

cmake_minimum_required(VERSION 3.24)
project(wow-world-editor LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The editor is 64-bit; configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b-docking
    GIT_SHALLOW TRUE)
set(STORM_USE_BUNDLED_LIBRARIES ON CACHE BOOL "" FORCE)
set(STORM_SKIP_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(stormlib
    GIT_REPOSITORY https://github.com/ladislav-zezula/StormLib.git
    GIT_TAG v9.40
    GIT_SHALLOW TRUE)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_Declare(imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG 18cef5e031d8c6973d80284c67f60549fafd78c1
    SOURCE_SUBDIR no-cmake)   # sources only: its own CMakeLists builds extras we do not need
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)   # CascLib asks for cmake 3.2, which cmake 4 no longer accepts
set(CASC_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(CASC_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
FetchContent_Declare(casclib   # CASC storages (modern clients' Data\data); master: newest root formats
    GIT_REPOSITORY https://github.com/ladislav-zezula/CascLib.git
    GIT_TAG 38a34665624b8775bb875274b36191b21c38d97b
    # CDN misses fetch just the file's bytes (HTTP range) instead of its whole ~256 MB archive. Re-check on a bump.
    PATCH_COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/cmake/casclib-cdn-range.patch -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply-patch.cmake)
FetchContent_MakeAvailable(imgui stormlib json imguizmo casclib)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp ${imguizmo_SOURCE_DIR}/src)

add_executable(wow-world-editor WIN32
    src/main.cpp
    src/Checks.cpp
    src/ChecksCore.cpp
    src/ChecksModels.cpp
    src/ChecksServer.cpp
    src/ChecksSources.cpp
    src/ChecksTerrain.cpp
    src/ChecksVersions.cpp
    src/App.cpp
    src/AppPopulate.cpp
    src/AppCompare.cpp
    src/AppDifferences.cpp
    src/AppFlights.cpp
    src/AppLights.cpp
    src/AppSound.cpp
    src/AppRoads.cpp
    src/Differences.cpp
    src/AppInspect.cpp
    src/AppNpc.cpp
    src/AppDialogue.cpp
    src/AppPaths.cpp
    src/AppPois.cpp
    src/AppRaces.cpp
    src/AppServer.cpp
    src/AppSources.cpp
    src/AppWorldMap.cpp
    src/AppTransform.cpp
    src/AppTriggers.cpp
    src/AppZones.cpp
    src/Areas.cpp
    src/Lights.cpp
    src/Sounds.cpp
    src/Roads.cpp
    src/ServerData.cpp
    src/Assets.cpp
    src/Downport.cpp
    src/Blend.cpp
    src/Blueprint.cpp
    src/Catalog.cpp
    src/Changes.cpp
    src/Flights.cpp
    src/Formats.cpp
    src/Ghosts.cpp
    src/Loader.cpp
    src/Minimap.cpp
    src/ModelRenderer.cpp
    src/Models.cpp
    src/Mpq.cpp
    src/Project.cpp
    src/Renderer.cpp
    src/Server.cpp
    src/Looks.cpp
    src/Spawns.cpp
    src/Tables.cpp
    src/Paths.cpp
    src/Pois.cpp
    src/Races.cpp
    src/Terrain.cpp
    src/Triggers.cpp)
target_compile_definitions(wow-world-editor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN CASCLIB_NO_AUTO_LINK_LIBRARY)
target_link_libraries(wow-world-editor PRIVATE imgui StormLib::storm casc_static nlohmann_json::nlohmann_json
    d3d11 d3dcompiler dxgi ole32 shell32 uuid windowscodecs advapi32 winhttp winmm)

# AzerothCore DB link: the MySQL 8.x client library of a local MySQL install. Delay-loaded, so the editor starts
# without it; the DLLs are copied next to the exe.
set(MYSQL_DIR "C:/Program Files/MySQL/MySQL Server 8.4" CACHE PATH "MySQL install with include/ and lib/libmysql.lib")
find_path(MYSQL_INCLUDE_DIR mysql.h PATHS "${MYSQL_DIR}/include" REQUIRED NO_DEFAULT_PATH)
find_library(MYSQL_LIBRARY libmysql PATHS "${MYSQL_DIR}/lib" REQUIRED NO_DEFAULT_PATH)
target_include_directories(wow-world-editor PRIVATE ${MYSQL_INCLUDE_DIR})
target_link_libraries(wow-world-editor PRIVATE ${MYSQL_LIBRARY} delayimp)
target_link_options(wow-world-editor PRIVATE /DELAYLOAD:libmysql.dll)
add_custom_command(TARGET wow-world-editor POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${MYSQL_DIR}/lib/libmysql.dll" "${MYSQL_DIR}/bin/libcrypto-3-x64.dll" "${MYSQL_DIR}/bin/libssl-3-x64.dll"
    $<TARGET_FILE_DIR:wow-world-editor>)
