cmake_minimum_required(VERSION 3.22)

project(memoria VERSION 0.6.0 LANGUAGES C CXX)
# A pre-release names its suffix here (tag v0.6.0-rc.3); a release leaves it empty.
set(MEMORIA_PRERELEASE "rc.3")
if(MEMORIA_PRERELEASE)
    set(MEMORIA_VERSION_TEXT "${PROJECT_VERSION}-${MEMORIA_PRERELEASE}")
else()
    set(MEMORIA_VERSION_TEXT "${PROJECT_VERSION}")
endif()

# The core builds and is tested anywhere; the plugin needs 32-bit MSVC and the Ashita SDK.
option(MEMORIA_BUILD_PLUGIN "Build the Ashita plugin (needs ASHITA4_SDK_PATH)" ${WIN32})
option(MEMORIA_BUILD_TESTS "Build the core's tests" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(XICAP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(XICAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    xicap
    GIT_REPOSITORY https://github.com/ffxi-dev/xicap.git
    GIT_TAG        v4.0.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(xicap)

# Header only: SOURCE_SUBDIR keeps its test CMakeLists out of the build.
FetchContent_Declare(
    picosha2
    GIT_REPOSITORY https://github.com/okdshin/PicoSHA2.git
    GIT_TAG        v1.0.1
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  none
)
FetchContent_MakeAvailable(picosha2)

FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(json)

add_library(memoria_core STATIC
    src/core/captain.cpp
    src/core/clock.cpp
    src/core/event_queue.cpp
    src/core/link.cpp
    src/core/live.cpp
    src/core/recorder.cpp
    src/core/seed_file.cpp
    src/core/tus.cpp
    src/core/uploader.cpp
)
target_include_directories(memoria_core PUBLIC src ${picosha2_SOURCE_DIR})
target_link_libraries(memoria_core PUBLIC xicap PRIVATE nlohmann_json::nlohmann_json)

if(MEMORIA_BUILD_PLUGIN)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
    find_package(AshitaSDK REQUIRED)
    ashita_sdk_initialize()

    add_library(memoria MODULE
        src/plugin/exports.def
        src/plugin/ashita.cpp
        src/plugin/ashita.h
        src/plugin/client.cpp
        src/plugin/client.h
        src/plugin/live.cpp
        src/plugin/live.h
        src/plugin/session.cpp
        src/plugin/session.h
        src/plugin/winhttp.cpp
        src/plugin/winhttp.h
    )
    set_target_properties(memoria PROPERTIES OUTPUT_NAME "Memoria" CXX_EXTENSIONS ON)
    if(MEMORIA_OUTPUT_DIR)
        set_target_properties(memoria PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${MEMORIA_OUTPUT_DIR}
            RUNTIME_OUTPUT_DIRECTORY ${MEMORIA_OUTPUT_DIR}
        )
    endif()
    ashita_sdk_attach(memoria)
    ashita_sdk_set_compiler_defaults(memoria)
    ashita_sdk_set_compiler_warn_level(memoria 4)
    ashita_sdk_set_compiler_warnings_as_errors(memoria)
    ashita_sdk_set_linker_defaults(memoria)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        ashita_sdk_set_linker_enable_debug_linking(memoria)
    else()
        ashita_sdk_set_linker_disable_debug_linking(memoria)
    endif()
    target_link_libraries(memoria PRIVATE memoria_core winhttp shell32 ole32)
    target_compile_definitions(memoria PRIVATE MEMORIA_VERSION=${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} MEMORIA_VERSION_TEXT="${MEMORIA_VERSION_TEXT}")
    # The SDK helper clears the global C++ flags, so the core and xicap need their exception model and Release optimisation back.
    foreach(target memoria_core xicap)
        target_compile_options(${target} PRIVATE /EHa "$<$<CONFIG:Release>:/O2;/Ob2;/DNDEBUG>")
    endforeach()
    # C5298: a stray control character in the SDK's d3d8 dxfile.h.
    target_compile_options(memoria PRIVATE /wd5298)
    # Designated initializers leave fields out on purpose; they start at zero.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(memoria PRIVATE -Wno-missing-designated-field-initializers)
    endif()
endif()

if(MEMORIA_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.4
    )
    FetchContent_MakeAvailable(Catch2)
    add_executable(memoria_core_tests tests/core_tests.cpp)
    target_link_libraries(memoria_core_tests PRIVATE memoria_core nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(memoria_core_tests)
endif()
