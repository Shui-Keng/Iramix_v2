cmake_minimum_required(VERSION 3.28)

project(
    Iramix
    VERSION 0.0.1
    DESCRIPTION "A cross-platform DAW for composers and sound designers"
    LANGUAGES CXX
)

option(IRAMIX_BUILD_TESTS "Build Iramix tests" ON)
option(
    IRAMIX_ENABLE_SANITIZERS
    "Enable AddressSanitizer and UndefinedBehaviorSanitizer"
    OFF
)
option(
    IRAMIX_ENABLE_THREAD_SANITIZER
    "Enable ThreadSanitizer"
    OFF
)
set(
    IRAMIX_ASIO_SDK_PATH
    ""
    CACHE PATH
    "Path to an external Steinberg ASIO SDK checkout"
)
# Optional like the ASIO path, but for a different reason: the VST3 SDK is
# MIT from 3.8 and could be vendored, yet leaving it external keeps CI free
# of a large third-party tree it does not need in order to prove the scan
# isolation. Only plugin *metadata* decoding depends on it.
set(
    IRAMIX_VST3_SDK_PATH
    ""
    CACHE PATH
    "Path to an external Steinberg VST3 SDK checkout (3.8 or later)"
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
iramix_enable_sanitizers()

add_library(iramix_core STATIC
    src/audio/DeviceBufferConversion.cpp
    src/audio/Graph.cpp
    src/audio/MidiEventBuffer.cpp
    src/audio/Nodes.cpp
    src/audio/ParameterEvents.cpp
    src/audio/RenderPlan.cpp
    src/audio/RenderPlanExecutor.cpp
    src/ipc/Protocol.cpp
    src/platform/Platform.cpp
    src/plugin/PluginBridge.cpp
    src/plugin/PluginScanner.cpp
    src/persistence/AsyncProjectSaver.cpp
    src/persistence/AsyncSessionSaver.cpp
    src/persistence/AutosaveClock.cpp
    src/persistence/DeviceResolver.cpp
    src/persistence/DiskAudioWorkers.cpp
    src/persistence/MediaResolver.cpp
    src/persistence/Persistence.cpp
    src/persistence/ProjectBackupStore.cpp
    src/persistence/SessionDocument.cpp
    src/persistence/SessionPersistenceService.cpp
    src/persistence/SessionSaveCoordinator.cpp
    src/realtime/Audit.cpp
    src/session/JournaledSession.cpp
    src/session/SessionController.cpp
)

add_library(Iramix::Core ALIAS iramix_core)

target_compile_features(iramix_core PUBLIC cxx_std_20)
target_include_directories(iramix_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
iramix_set_warnings(iramix_core)

if(IRAMIX_VST3_SDK_PATH)
    if(NOT EXISTS
        "${IRAMIX_VST3_SDK_PATH}/pluginterfaces/base/ipluginbase.h")
        message(
            FATAL_ERROR
            "Invalid VST3 SDK path: missing pluginterfaces/base/ipluginbase.h"
        )
    endif()
    # Only the interface headers are used. They are declarations of a
    # stable ABI, so nothing from the SDK is compiled or linked, and the
    # scanner still runs without the SDK — it just cannot name a plugin.
    # SYSTEM so the SDK's own warnings are not this project's to fix. The
    # build stays clean at /W4 and -Wall -Wextra without lowering the bar
    # for code we do own.
    target_include_directories(iramix_core
        SYSTEM PRIVATE "${IRAMIX_VST3_SDK_PATH}"
    )
    target_compile_definitions(iramix_core PRIVATE IRAMIX_HAS_VST3_SDK)
endif()

# dlopen lives in libdl on glibc older than 2.34.
if(UNIX AND NOT APPLE)
    find_library(IRAMIX_DL_LIBRARY dl)
    if(IRAMIX_DL_LIBRARY)
        target_link_libraries(iramix_core PUBLIC ${IRAMIX_DL_LIBRARY})
    endif()
endif()

# shm_open lives in librt on glibc older than 2.34; harmless where it does not.
if(UNIX AND NOT APPLE)
    find_library(IRAMIX_RT_LIBRARY rt)
    if(IRAMIX_RT_LIBRARY)
        target_link_libraries(iramix_core PUBLIC ${IRAMIX_RT_LIBRARY})
    endif()
endif()

add_executable(iramix_engine_probe apps/engine-probe/main.cpp)
target_link_libraries(iramix_engine_probe PRIVATE Iramix::Core)
iramix_set_warnings(iramix_engine_probe)

add_executable(iramix_plugin_scan apps/plugin-scan/main.cpp)
target_link_libraries(iramix_plugin_scan PRIVATE Iramix::Core)
iramix_set_warnings(iramix_plugin_scan)

set(IRAMIX_AUDIO_PROBE_SOURCES
    apps/audio-probe/AllocationHooks.cpp
    apps/audio-probe/main.cpp
)
if(WIN32)
    list(APPEND IRAMIX_AUDIO_PROBE_SOURCES
        apps/audio-probe/WasapiProbe.cpp
    )
elseif(APPLE)
    list(APPEND IRAMIX_AUDIO_PROBE_SOURCES
        apps/audio-probe/CoreAudioProbe.cpp
    )
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(JACK QUIET IMPORTED_TARGET jack)
    endif()
    if(TARGET PkgConfig::JACK)
        list(APPEND IRAMIX_AUDIO_PROBE_SOURCES
            apps/audio-probe/JackProbe.cpp
        )
    else()
        list(APPEND IRAMIX_AUDIO_PROBE_SOURCES
            apps/audio-probe/UnsupportedProbe.cpp
        )
    endif()
endif()

add_executable(iramix_audio_probe ${IRAMIX_AUDIO_PROBE_SOURCES})
target_link_libraries(iramix_audio_probe PRIVATE Iramix::Core)
if(WIN32)
    target_link_libraries(iramix_audio_probe PRIVATE avrt ksuser ole32)
elseif(APPLE)
    target_link_libraries(
        iramix_audio_probe
        PRIVATE
            "-framework CoreAudio"
    )
elseif(TARGET PkgConfig::JACK)
    target_link_libraries(iramix_audio_probe PRIVATE PkgConfig::JACK)
endif()
iramix_set_warnings(iramix_audio_probe)

if(WIN32 AND IRAMIX_ASIO_SDK_PATH)
    foreach(requiredPath
        "common/asio.h"
        "common/asio.cpp"
        "host/asiodrivers.cpp"
        "host/pc/asiolist.cpp"
    )
        if(NOT EXISTS "${IRAMIX_ASIO_SDK_PATH}/${requiredPath}")
            message(
                FATAL_ERROR
                "Invalid ASIO SDK path: missing ${requiredPath}"
            )
        endif()
    endforeach()

    add_executable(iramix_asio_probe
        apps/audio-probe/AllocationHooks.cpp
        apps/audio-probe/AsioProbe.cpp
        apps/audio-probe/AsioProbeMain.cpp
        "${IRAMIX_ASIO_SDK_PATH}/common/asio.cpp"
        "${IRAMIX_ASIO_SDK_PATH}/host/asiodrivers.cpp"
        "${IRAMIX_ASIO_SDK_PATH}/host/pc/asiolist.cpp"
    )
    target_include_directories(iramix_asio_probe PRIVATE
        "${IRAMIX_ASIO_SDK_PATH}/common"
        "${IRAMIX_ASIO_SDK_PATH}/host"
        "${IRAMIX_ASIO_SDK_PATH}/host/pc"
    )
    target_link_libraries(
        iramix_asio_probe
        PRIVATE
            Iramix::Core
            ole32
            winmm
    )
    iramix_set_warnings(iramix_asio_probe)
endif()

if(IRAMIX_BUILD_TESTS)
    enable_testing()
    add_executable(iramix_smoke_tests tests/SmokeTests.cpp)
    target_link_libraries(iramix_smoke_tests PRIVATE Iramix::Core)
    iramix_set_warnings(iramix_smoke_tests)
    add_test(NAME iramix.smoke COMMAND iramix_smoke_tests)

    add_executable(iramix_audio_graph_tests
        apps/audio-probe/AllocationHooks.cpp
        tests/AudioGraphTests.cpp
    )
    target_link_libraries(iramix_audio_graph_tests PRIVATE Iramix::Core)
    iramix_set_warnings(iramix_audio_graph_tests)
    add_test(
        NAME iramix.audio_graph
        COMMAND iramix_audio_graph_tests
    )

    add_executable(iramix_plan_swap_tests
        apps/audio-probe/AllocationHooks.cpp
        tests/PlanSwapTests.cpp
    )
    target_link_libraries(iramix_plan_swap_tests PRIVATE Iramix::Core)
    iramix_set_warnings(iramix_plan_swap_tests)
    add_test(
        NAME iramix.plan_swap
        COMMAND iramix_plan_swap_tests
    )

    add_executable(
        iramix_persistence_tests
        apps/audio-probe/AllocationHooks.cpp
        tests/PersistenceTests.cpp
    )
    target_link_libraries(
        iramix_persistence_tests
        PRIVATE
            Iramix::Core
    )
    iramix_set_warnings(iramix_persistence_tests)
    add_test(
        NAME iramix.persistence
        COMMAND iramix_persistence_tests
    )

    add_executable(iramix_session_tests tests/SessionTests.cpp)
    target_link_libraries(
        iramix_session_tests
        PRIVATE
            Iramix::Core
    )
    iramix_set_warnings(iramix_session_tests)
    add_test(
        NAME iramix.session
        COMMAND iramix_session_tests
    )

    add_executable(iramix_plugin_tests tests/PluginTests.cpp)
    target_link_libraries(
        iramix_plugin_tests
        PRIVATE
            Iramix::Core
    )
    iramix_set_warnings(iramix_plugin_tests)
    add_test(
        NAME iramix.plugin
        COMMAND iramix_plugin_tests
    )

    add_test(
        NAME iramix.audio_probe.audit
        COMMAND iramix_audio_probe --self-test
    )
endif()
