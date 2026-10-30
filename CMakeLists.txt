cmake_minimum_required(VERSION 3.22)

project(memoria VERSION 0.1.0 LANGUAGES C CXX)

# The core (queue, clock, keys, recording) builds anywhere and is tested on
# Linux and Windows; the plugin needs Windows, 32-bit MSVC, and the Ashita SDK.
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
    GIT_TAG        v3.2.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(xicap)

add_library(memoria_core STATIC
    src/core/clock.cpp
    src/core/event_queue.cpp
    src/core/seed_file.cpp
)
target_include_directories(memoria_core PUBLIC src)
target_link_libraries(memoria_core PUBLIC xicap)

if(MEMORIA_BUILD_PLUGIN)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
    find_package(AshitaSDK REQUIRED)
    ashita_sdk_initialize()

    add_library(memoria MODULE
        src/plugin/exports.def
        src/plugin/memoria.cpp
        src/plugin/memoria.h
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
    target_link_libraries(memoria PRIVATE memoria_core)
    # The SDK helper clears the global flags, /EHsc among them, so the core
    # gets the plugin's exception model back explicitly.
    target_compile_options(memoria_core PRIVATE /EHa)
    # C5298: a stray control character in the SDK's own d3d8 dxfile.h.
    target_compile_options(memoria PRIVATE /wd5298)
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
    target_link_libraries(memoria_core_tests PRIVATE memoria_core Catch2::Catch2WithMain)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(memoria_core_tests)
endif()
