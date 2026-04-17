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
set(OPENVR_ROOT "${CMAKE_SOURCE_DIR}/third_party/openvr")
set(OPENVR_INCLUDE_DIR "${OPENVR_ROOT}/headers")
set(OPENVR_LIBRARY "${OPENVR_ROOT}/lib/win64/openvr_api.lib")
set(OPENVR_DLL "${OPENVR_ROOT}/bin/win64/openvr_api.dll")
set(OVERLAY_MANIFEST_TEMPLATE "${CMAKE_SOURCE_DIR}/cmake/steamvr_capture_overlay.vrmanifest.in")
set(OVERLAY_MANIFEST_CONFIGURED "${CMAKE_BINARY_DIR}/steamvr_capture_overlay.vrmanifest")

option(STEAMVR_CAPTURE_DOWNLOAD_INNO "Download a portable Inno Setup compiler when ISCC.exe is not found." ON)
set(STEAMVR_CAPTURE_INNOSETUP_VERSION "6.7.1" CACHE STRING "Tools.InnoSetup NuGet package version.")
set(STEAMVR_CAPTURE_INNOSETUP_SHA256 "00A5A07E8A4BD3A30364CB579D744677B37ADFC22E365099CB01B8BD1E651828" CACHE STRING "Expected SHA256 for the Tools.InnoSetup NuGet package.")

if (NOT EXISTS "${OPENVR_INCLUDE_DIR}/openvr.h")
    message(FATAL_ERROR "OpenVR headers were not found. Expected ${OPENVR_INCLUDE_DIR}/openvr.h")
endif ()

if (NOT EXISTS "${OPENVR_LIBRARY}")
    message(FATAL_ERROR "OpenVR import library was not found. Expected ${OPENVR_LIBRARY}")
endif ()

find_program(INNOSETUP_COMPILER
    NAMES ISCC.exe iscc.exe iscc
    DOC "Inno Setup command line compiler."
)

if (NOT INNOSETUP_COMPILER AND STEAMVR_CAPTURE_DOWNLOAD_INNO)
    set(INNOSETUP_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/innosetup")
    set(INNOSETUP_NUPKG "${INNOSETUP_DOWNLOAD_DIR}/Tools.InnoSetup.${STEAMVR_CAPTURE_INNOSETUP_VERSION}.nupkg")
    set(INNOSETUP_EXTRACT_DIR "${INNOSETUP_DOWNLOAD_DIR}/Tools.InnoSetup.${STEAMVR_CAPTURE_INNOSETUP_VERSION}")
    set(INNOSETUP_URL "https://www.nuget.org/api/v2/package/Tools.InnoSetup/${STEAMVR_CAPTURE_INNOSETUP_VERSION}")

    file(MAKE_DIRECTORY "${INNOSETUP_DOWNLOAD_DIR}")
    if (NOT EXISTS "${INNOSETUP_NUPKG}")
        message(STATUS "Downloading portable Inno Setup compiler ${STEAMVR_CAPTURE_INNOSETUP_VERSION}")
        file(DOWNLOAD
            "${INNOSETUP_URL}"
            "${INNOSETUP_NUPKG}"
            EXPECTED_HASH "SHA256=${STEAMVR_CAPTURE_INNOSETUP_SHA256}"
            SHOW_PROGRESS
            TLS_VERIFY ON
        )
    endif ()

    if (NOT EXISTS "${INNOSETUP_EXTRACT_DIR}/tools/ISCC.exe")
        file(REMOVE_RECURSE "${INNOSETUP_EXTRACT_DIR}")
        file(MAKE_DIRECTORY "${INNOSETUP_EXTRACT_DIR}")
        file(ARCHIVE_EXTRACT
            INPUT "${INNOSETUP_NUPKG}"
            DESTINATION "${INNOSETUP_EXTRACT_DIR}"
        )
    endif ()

    set(INNOSETUP_COMPILER "${INNOSETUP_EXTRACT_DIR}/tools/ISCC.exe")
endif ()

if (INNOSETUP_COMPILER)
    message(STATUS "Inno Setup compiler: ${INNOSETUP_COMPILER}")
else ()
    message(WARNING "Inno Setup compiler was not found. The steamvr_capture_installer target will be unavailable.")
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
        winmm
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
    src/hotpatch/server_driver_hook.cpp
)

target_include_directories(steamvr_capture_hotpatch
    PRIVATE
        "${OPENVR_INCLUDE_DIR}"
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
    src/capture/openvr_app_helpers.cpp
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

add_executable(steamvr_capture_setup_helper
    src/setup_helper/main.cpp
)

target_include_directories(steamvr_capture_setup_helper
    PRIVATE
        "${OPENVR_INCLUDE_DIR}"
        "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(steamvr_capture_setup_helper
    PRIVATE
        "${OPENVR_LIBRARY}"
        shell32
        ole32
)

target_compile_definitions(steamvr_capture_setup_helper
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

set_target_properties(steamvr_capture_setup_helper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_RUNTIME_DIR}/tools"
)

add_custom_command(
    TARGET steamvr_capture_setup_helper
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${OPENVR_DLL}"
        "$<TARGET_FILE_DIR:steamvr_capture_setup_helper>/openvr_api.dll"
)

if (MINGW)
    foreach (runtime_dll IN LISTS MINGW_RUNTIME_DLLS)
        if (EXISTS "${runtime_dll}")
            add_custom_command(
                TARGET steamvr_capture_setup_helper
                POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_dll}"
                    "$<TARGET_FILE_DIR:steamvr_capture_setup_helper>"
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
        steamvr_capture_session
        "${OPENVR_LIBRARY}"
        gdi32
        user32
        shell32
        winmm
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
    src/replay_driver/hotpatch_bridge_exports.cpp
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

install(TARGETS
    steamvr_capture_recorder
    steamvr_capture_broker
    steamvr_capture_setup_helper
    steamvr_capture_overlay
    RUNTIME DESTINATION tools
)

install(TARGETS steamvr_capture_hotpatch
    RUNTIME DESTINATION tools
)

install(TARGETS driver_steamvr_capture_replay
    RUNTIME DESTINATION "${DRIVER_PACKAGE_NAME}/bin/win64"
)

install(FILES "${OPENVR_DLL}" DESTINATION tools)
install(FILES "${OPENVR_DLL}" DESTINATION "${DRIVER_PACKAGE_NAME}/bin/win64")

if (MINGW)
    foreach (runtime_dll IN LISTS MINGW_RUNTIME_DLLS)
        if (EXISTS "${runtime_dll}")
            install(FILES "${runtime_dll}" DESTINATION tools)
            install(FILES "${runtime_dll}" DESTINATION "${DRIVER_PACKAGE_NAME}/bin/win64")
        endif ()
    endforeach ()
endif ()

install(DIRECTORY "${CMAKE_SOURCE_DIR}/driver/${DRIVER_PACKAGE_NAME}/"
    DESTINATION "${DRIVER_PACKAGE_NAME}"
)

if (INNOSETUP_COMPILER)
    set(INSTALLER_STAGE_ROOT "${CMAKE_BINARY_DIR}/installer_stage/SteamVRCapture")
    set(INSTALLER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/installer")
    file(TO_NATIVE_PATH "${INSTALLER_STAGE_ROOT}" INSTALLER_STAGE_ROOT_NATIVE)
    file(TO_NATIVE_PATH "${INSTALLER_OUTPUT_DIR}" INSTALLER_OUTPUT_DIR_NATIVE)

    add_custom_target(steamvr_capture_installer
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${INSTALLER_STAGE_ROOT}"
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --prefix "${INSTALLER_STAGE_ROOT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${INSTALLER_OUTPUT_DIR}"
        COMMAND "${INNOSETUP_COMPILER}"
            /Qp
            "/DMyAppVersion=${PROJECT_VERSION}"
            "/DMySourceRoot=${INSTALLER_STAGE_ROOT_NATIVE}"
            "/DMyOutputDir=${INSTALLER_OUTPUT_DIR_NATIVE}"
            "${CMAKE_SOURCE_DIR}/installer/SteamVRCapture.iss"
        DEPENDS
            steamvr_capture_recorder
            steamvr_capture_broker
            steamvr_capture_setup_helper
            steamvr_capture_overlay
            steamvr_capture_hotpatch
            driver_steamvr_capture_replay
        COMMENT "Building SteamVR Capture installer"
        VERBATIM
    )
endif ()
