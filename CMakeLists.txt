cmake_minimum_required(VERSION 3.27)

project(steamvr_capture VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT WIN32)
    message(FATAL_ERROR "This bootstrap targets Windows SteamVR only.")
endif ()

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SteamVR requires a 64-bit build.")
endif ()

set(PROJECT_RUNTIME_DIR "${CMAKE_BINARY_DIR}/runtime")
set(DRIVER_PACKAGE_NAME "steamvr_capture_replay")
set(DRIVER_PACKAGE_DIR "${PROJECT_RUNTIME_DIR}/${DRIVER_PACKAGE_NAME}")
set(PROXY_DRIVER_PACKAGE_NAME "steamvr_capture_lighthouse_proxy")
set(PROXY_DRIVER_PACKAGE_DIR "${PROJECT_RUNTIME_DIR}/${PROXY_DRIVER_PACKAGE_NAME}")
set(OPENVR_ROOT "${CMAKE_SOURCE_DIR}/third_party/openvr")
set(OPENVR_INCLUDE_DIR "${OPENVR_ROOT}/headers")
set(OPENVR_LIBRARY "${OPENVR_ROOT}/lib/win64/openvr_api.lib")
set(OPENVR_DLL "${OPENVR_ROOT}/bin/win64/openvr_api.dll")
set(OVERLAY_MANIFEST_TEMPLATE "${CMAKE_SOURCE_DIR}/cmake/steamvr_capture_overlay.vrmanifest.in")
set(OVERLAY_MANIFEST_CONFIGURED "${CMAKE_BINARY_DIR}/steamvr_capture_overlay.vrmanifest")

if (NOT EXISTS "${OPENVR_INCLUDE_DIR}/openvr.h")
    message(FATAL_ERROR "OpenVR headers were not found. Expected ${OPENVR_INCLUDE_DIR}/openvr.h")
endif ()

if (NOT EXISTS "${OPENVR_LIBRARY}")
    message(FATAL_ERROR "OpenVR import library was not found. Expected ${OPENVR_LIBRARY}")
endif ()

add_library(steamvr_capture_session STATIC
    src/session/session_format.cpp
)

target_include_directories(steamvr_capture_session
    PUBLIC
        "${CMAKE_SOURCE_DIR}/src"
)

target_compile_definitions(steamvr_capture_session
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

add_executable(steamvr_capture_recorder
    src/capture/main.cpp
    src/capture/openvr_app_helpers.cpp
)

target_include_directories(steamvr_capture_recorder
    PRIVATE
        "${OPENVR_INCLUDE_DIR}"
        "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(steamvr_capture_recorder
    PRIVATE
        steamvr_capture_session
        "${OPENVR_LIBRARY}"
)

target_compile_definitions(steamvr_capture_recorder
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

set_target_properties(steamvr_capture_recorder PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_RUNTIME_DIR}/tools"
)

add_custom_command(
    TARGET steamvr_capture_recorder
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${OPENVR_DLL}"
        "$<TARGET_FILE_DIR:steamvr_capture_recorder>/openvr_api.dll"
)

if (MINGW)
    get_filename_component(CXX_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(MINGW_RUNTIME_DLLS
        "${CXX_COMPILER_DIR}/libstdc++-6.dll"
        "${CXX_COMPILER_DIR}/libgcc_s_seh-1.dll"
        "${CXX_COMPILER_DIR}/libwinpthread-1.dll"
    )
    foreach (runtime_dll IN LISTS MINGW_RUNTIME_DLLS)
        if (EXISTS "${runtime_dll}")
            add_custom_command(
                TARGET steamvr_capture_recorder
                POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_dll}"
                    "$<TARGET_FILE_DIR:steamvr_capture_recorder>"
            )
        endif ()
    endforeach ()
endif ()

add_library(steamvr_capture_hotpatch SHARED
    src/hotpatch/dllmain.cpp
    src/hotpatch/hotpatch_runtime.cpp
)

target_include_directories(steamvr_capture_hotpatch
    PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
)

target_compile_definitions(steamvr_capture_hotpatch
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

set_target_properties(steamvr_capture_hotpatch PROPERTIES
    OUTPUT_NAME "steamvr_capture_hotpatch"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_RUNTIME_DIR}/tools"
)

if (MINGW)
    foreach (runtime_dll IN LISTS MINGW_RUNTIME_DLLS)
        if (EXISTS "${runtime_dll}")
            add_custom_command(
                TARGET steamvr_capture_hotpatch
                POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_dll}"
                    "$<TARGET_FILE_DIR:steamvr_capture_hotpatch>"
            )
        endif ()
    endforeach ()
endif ()

add_executable(steamvr_capture_broker
    src/broker/main.cpp
    src/broker/broker_app.cpp
)

target_include_directories(steamvr_capture_broker
    PRIVATE
        "${OPENVR_INCLUDE_DIR}"
        "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(steamvr_capture_broker
    PRIVATE
        steamvr_capture_session
        "${OPENVR_LIBRARY}"
)

target_compile_definitions(steamvr_capture_broker
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

set_target_properties(steamvr_capture_broker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_RUNTIME_DIR}/tools"
)

add_dependencies(steamvr_capture_broker steamvr_capture_hotpatch)

add_custom_command(
    TARGET steamvr_capture_broker
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${OPENVR_DLL}"
        "$<TARGET_FILE_DIR:steamvr_capture_broker>/openvr_api.dll"
)

if (MINGW)
    foreach (runtime_dll IN LISTS MINGW_RUNTIME_DLLS)
        if (EXISTS "${runtime_dll}")
            add_custom_command(
                TARGET steamvr_capture_broker
                POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_dll}"
                    "$<TARGET_FILE_DIR:steamvr_capture_broker>"
            )
        endif ()
    endforeach ()
endif ()

add_library(driver_steamvr_capture_lighthouse_proxy SHARED
    src/proxy_driver/hmd_driver_factory.cpp
    src/proxy_driver/lighthouse_proxy_provider.cpp
    src/replay_driver/driver_log.cpp
)

target_include_directories(driver_steamvr_capture_lighthouse_proxy
    PRIVATE
        "${OPENVR_INCLUDE_DIR}"
        "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(driver_steamvr_capture_lighthouse_proxy
    PRIVATE
        steamvr_capture_session
        "${OPENVR_LIBRARY}"
)

target_compile_definitions(driver_steamvr_capture_lighthouse_proxy
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

set_target_properties(driver_steamvr_capture_lighthouse_proxy PROPERTIES
    OUTPUT_NAME "driver_steamvr_capture_lighthouse_proxy"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${PROXY_DRIVER_PACKAGE_DIR}/bin/win64"
)

add_custom_command(
    TARGET driver_steamvr_capture_lighthouse_proxy
    PRE_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/driver/${PROXY_DRIVER_PACKAGE_NAME}"
        "${PROXY_DRIVER_PACKAGE_DIR}"
)

add_custom_command(
    TARGET driver_steamvr_capture_lighthouse_proxy
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${OPENVR_DLL}"
        "$<TARGET_FILE_DIR:driver_steamvr_capture_lighthouse_proxy>/openvr_api.dll"
)

if (MINGW)
    foreach (runtime_dll IN LISTS MINGW_RUNTIME_DLLS)
        if (EXISTS "${runtime_dll}")
            add_custom_command(
                TARGET driver_steamvr_capture_lighthouse_proxy
                POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_dll}"
                    "$<TARGET_FILE_DIR:driver_steamvr_capture_lighthouse_proxy>"
            )
        endif ()
    endforeach ()
endif ()

file(TO_CMAKE_PATH "${PROJECT_RUNTIME_DIR}/tools/steamvr_capture_overlay.exe" OVERLAY_BINARY_PATH)
configure_file("${OVERLAY_MANIFEST_TEMPLATE}" "${OVERLAY_MANIFEST_CONFIGURED}" @ONLY)

add_executable(steamvr_capture_overlay
    src/overlay/main.cpp
    src/overlay/overlay_app.cpp
)

target_include_directories(steamvr_capture_overlay
    PRIVATE
        "${OPENVR_INCLUDE_DIR}"
        "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(steamvr_capture_overlay
    PRIVATE
        "${OPENVR_LIBRARY}"
        gdi32
        user32
        shell32
)

target_compile_definitions(steamvr_capture_overlay
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

set_target_properties(steamvr_capture_overlay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_RUNTIME_DIR}/tools"
)

add_custom_command(
    TARGET steamvr_capture_overlay
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${OPENVR_DLL}"
        "$<TARGET_FILE_DIR:steamvr_capture_overlay>/openvr_api.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${OVERLAY_MANIFEST_CONFIGURED}"
        "$<TARGET_FILE_DIR:steamvr_capture_overlay>/steamvr_capture_overlay.vrmanifest"
)

if (MINGW)
    foreach (runtime_dll IN LISTS MINGW_RUNTIME_DLLS)
        if (EXISTS "${runtime_dll}")
            add_custom_command(
                TARGET steamvr_capture_overlay
                POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_dll}"
                    "$<TARGET_FILE_DIR:steamvr_capture_overlay>"
            )
        endif ()
    endforeach ()
endif ()

add_library(driver_steamvr_capture_replay SHARED
    src/replay_driver/hmd_driver_factory.cpp
    src/replay_driver/device_provider.cpp
    src/replay_driver/replay_tracker_device.cpp
    src/replay_driver/driver_log.cpp
)

target_include_directories(driver_steamvr_capture_replay
    PRIVATE
        "${OPENVR_INCLUDE_DIR}"
        "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(driver_steamvr_capture_replay
    PRIVATE
        steamvr_capture_session
        "${OPENVR_LIBRARY}"
)

target_compile_definitions(driver_steamvr_capture_replay
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

set_target_properties(driver_steamvr_capture_replay PROPERTIES
    OUTPUT_NAME "driver_steamvr_capture_replay"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${DRIVER_PACKAGE_DIR}/bin/win64"
)

add_custom_command(
    TARGET driver_steamvr_capture_replay
    PRE_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/driver/${DRIVER_PACKAGE_NAME}"
        "${DRIVER_PACKAGE_DIR}"
)

add_custom_command(
    TARGET driver_steamvr_capture_replay
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${OPENVR_DLL}"
        "$<TARGET_FILE_DIR:driver_steamvr_capture_replay>/openvr_api.dll"
)

if (MINGW)
    foreach (runtime_dll IN LISTS MINGW_RUNTIME_DLLS)
        if (EXISTS "${runtime_dll}")
            add_custom_command(
                TARGET driver_steamvr_capture_replay
                POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_dll}"
                    "$<TARGET_FILE_DIR:driver_steamvr_capture_replay>"
            )
        endif ()
    endforeach ()
endif ()
