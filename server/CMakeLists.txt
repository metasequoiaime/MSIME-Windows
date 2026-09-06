cmake_minimum_required(VERSION 3.25.0)
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()

set(PROJECT_NAME "MetasequoiaImeServer")

cmake_policy(SET CMP0167 NEW)
project(${PROJECT_NAME} VERSION 1.0)

# Release binaries need usable symbols for crash diagnosis. Keep the PDB beside each executable so
# the release job can validate and publish it without changing the binary's install layout.
if(MSVC)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")
    add_compile_options("$<$<CONFIG:Release>:/Zi>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG>")
endif()

include(CTest)

# The shared engine and the third-party sources are submodules of the monorepo, not of this
# component, so they sit one level up in vendor/ next to server/, ui/ and windows/.
set(MSIME_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor")

# Boost
# An explicit -DBoost_ROOT wins, then BOOST_ROOT from the environment, and only then the scoop
# layout under the current user's profile. The fallback used to name one developer's home directory,
# which meant anyone else without BOOST_ROOT set got a configure failure pointing at a path that
# does not exist on their machine.
if(NOT DEFINED Boost_ROOT)
    if(DEFINED ENV{BOOST_ROOT})
        set(Boost_ROOT "$ENV{BOOST_ROOT}")
    elseif(DEFINED ENV{USERPROFILE} AND EXISTS "$ENV{USERPROFILE}/scoop/apps/boost/current")
        set(Boost_ROOT "$ENV{USERPROFILE}/scoop/apps/boost/current")
    endif()
endif()
set (Boost_USE_STATIC_LIBS ON)
find_package(Boost REQUIRED COMPONENTS locale json)
include_directories(${Boost_INCLUDE_DIRS})

# fmt, spdlog and sqlite3
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

# The engine ships its own CMake target. Consume that instead of listing its
# translation units here, so that adding a file to the engine no longer requires
# a matching edit in this file. EXCLUDE_FROM_ALL keeps the engine's own test
# executables out of the default build.
#
# Which is also why BUILD_TESTING has to be off across that add_subdirectory: the engine registers
# eleven add_test() entries behind that flag, and EXCLUDE_FROM_ALL then never builds the
# executables they point at, so ctest reports eleven "Unable to find executable" failures. The
# engine's own CI runs those tests on Ubuntu, macOS and Windows; running them from here as well
# would be redundant even if they did build. Restored immediately afterwards so this repository's
# own tests/ still register.
set(_metasequoia_saved_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
set(METASEQUOIA_IME_BUILD_VOICE ON CACHE BOOL "" FORCE)
set(MSIME_VOICE_CAPTURE ON CACHE BOOL "" FORCE)
add_subdirectory("${MSIME_VENDOR_DIR}/MetasequoiaImeEngine" "${CMAKE_BINARY_DIR}/MetasequoiaImeEngine" EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${_metasequoia_saved_build_testing}")
unset(_metasequoia_saved_build_testing)

# Build the OpenCC submodule as a static dependency of the server.
add_subdirectory("${MSIME_VENDOR_DIR}/opencc" "${CMAKE_BINARY_DIR}/opencc" EXCLUDE_FROM_ALL)

# OpenCC's Windows dictionary helper copies the library into the tools output
# directory before MSBuild has necessarily created that directory.
add_custom_target(opencc_tools_output_dir
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_BINARY_DIR}/opencc/src/tools/$<CONFIG>"
)
add_dependencies(copy_libopencc_to_dir_of_opencc_dict opencc_tools_output_dir)

# cpp-pinyin: settings uses it to annotate pure-Chinese phrase imports.
set(_cpp_pinyin_saved_shared_libs "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
set(CPP_PINYIN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CPP_PINYIN_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${MSIME_VENDOR_DIR}/cpp-pinyin" "${CMAKE_BINARY_DIR}/cpp-pinyin" EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS "${_cpp_pinyin_saved_shared_libs}")
unset(_cpp_pinyin_saved_shared_libs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_definitions(
    /D_UNICODE=1
    /DUNICODE=1
    /DFANY_DEBUG=1
)

# HandwritingPanel still uses the legacy <experimental/coroutine>; on newer
# MSVC STL the deprecation is a hard error, so silence it explicitly.
add_definitions(/D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)

# add_definitions(
#     /DFANY_DEBUG=1
#     /DFANY_IPC_DEBUG=1
# )

# For utf8cpp, make it use C++11 and later
add_compile_options(/Zc:__cplusplus /FS /utf-8)

include_directories("./")
include_directories("./src/")
include_directories("${MSIME_VENDOR_DIR}")
include_directories("${MSIME_VENDOR_DIR}/MetasequoiaImeEngine/utfcpp/source/")

set(MY_EXECUTABLE_NAME "MetasequoiaImeServer")
set(WATCHDOG_EXECUTABLE_NAME "MetasequoiaImeWatchdog")
set(DICTIONARY_REPLAY_EXECUTABLE_NAME "MetasequoiaImeDictionaryReplay")

add_executable(${WATCHDOG_EXECUTABLE_NAME} WIN32
    "./src/watchdog/main.cpp"
    "./src/watchdog/watchdog_protocol.h"
    "./src/utils/single_instance.h"
    "./src/resource/resource.h"
    "./src/resource/resource.rc"
)
target_compile_definitions(${WATCHDOG_EXECUTABLE_NAME} PRIVATE
    NOMINMAX=1
    WIN32_LEAN_AND_MEAN=1
)
target_link_libraries(${WATCHDOG_EXECUTABLE_NAME} PRIVATE shell32 ole32)
if(MSVC)
    target_compile_options(${WATCHDOG_EXECUTABLE_NAME} PRIVATE /W4 /permissive-)
endif()

set(SRC_FILES
    "./src/ai/ai_assistant.cpp"
    "./src/ai/ai_assistant.h"
    "./src/clipboard/clipboard_history.cpp"
    "./src/clipboard/clipboard_history.h"
    "./src/cloud/cloud_ime.cpp"
    "./src/cloud/cloud_ime.h"
    "./src/cloud/cloud_translation.cpp"
    "./src/cloud/cloud_translation.h"
    "./src/cloud/custom_translation.cpp"
    "./src/cloud/custom_translation.h"
    "./src/cloud/tencent_tmt.cpp"
    "./src/cloud/tencent_tmt.h"
    "./src/cloud/translation_gloss.cpp"
    "./src/cloud/translation_gloss.h"
    "./src/config/ime_config.cpp"
    "./src/config/ime_config.h"
    "./src/conversion/chinese_converter.cpp"
    "./src/conversion/chinese_converter.h"
    "./src/defines/defines.h"
    "./src/emoji/emoji_ime.cpp"
    "./src/emoji/emoji_ime.h"
    "./src/english/english_ime.cpp"
    "./src/english/english_ime.h"
    "./src/global/globals.h"
    "./src/ipc/event_listener.cpp"
    "./src/ipc/event_listener.h"
    "./src/ipc/ipc.cpp"
    "./src/ipc/ipc.h"
    "./src/ipc/voice_composition_pipe.h"
    "./src/kaomoji/kaomoji_ime.cpp"
    "./src/kaomoji/kaomoji_ime.h"
    "./src/log/fanylog.cpp"
    "./src/log/fanylog.h"
    "./src/log/candidate_diag_log.cpp"
    "./src/log/candidate_diag_log.h"
    "./src/log/ftb_diag_log.h"
    "./src/resource/resource.h"
    "./src/resource/resource.rc"
    "./src/settings/settings_launcher.cpp"
    "./src/settings/settings_launcher.h"
    "./src/skin/candidate_skin_catalog.cpp"
    "./src/skin/candidate_skin_catalog.h"
    "./src/session/engine_input_session.cpp"
    "./src/session/engine_input_session.h"
    "./src/session/input_session.h"
    "./src/session/session_factory.cpp"
    "./src/session/session_factory.h"
    "./src/utils/candidate_size_estimator.cpp"
    "./src/utils/candidate_size_estimator.h"
    "./src/utils/common_utils.cpp"
    "./src/utils/common_utils.h"
    "./src/utils/ime_paths.cpp"
    "./src/utils/ime_utils.cpp"
    "./src/utils/ime_utils.h"
    "./src/utils/webview_utils.cpp"
    "./src/utils/webview_utils.h"
    "./src/utils/window_utils.cpp"
    "./src/utils/window_utils.h"
    "./src/webview2/windows_webview2.cpp"
    "./src/webview2/skin_css_policy.cpp"
    "./src/webview2/windows_webview2.h"
    "./src/window/candidate_presenter.cpp"
    "./src/window/candidate_presenter.h"
    "./src/window/tray_menu_presenter.cpp"
    "./src/window/tray_menu_presenter.h"
    "./src/window/floating_toolbar_presenter.cpp"
    "./src/window/floating_toolbar_presenter.h"
    "./src/window/ime_windows.cpp"
    "./src/window/ime_windows.h"
    "./src/window/window_hook.cpp"
    "./src/window/window_hook.h"
    "./src/voice-input/voice_input_service.cpp"
    "./src/voice-input/voice_input_service.h"
    "./src/voice-input/system_audio_muter.cpp"
    "./src/voice-input/system_audio_muter.h"
    "./src/voice-input/doubao_asr_client.cpp"
    "./src/voice-input/doubao_asr_client.h"
    "./src/voice-input/voice_batch_protocol.cpp"
    "./src/voice-input/voice_providers.cpp"
    "./src/voice-input/voice_providers.h"
    "./src/voice-input/wave_overlay.cpp"
    "./src/voice-input/wave_overlay.h"
    "./src/voice-input/cue_player.cpp"
    "./src/voice-input/cue_player.h"
    "./src/voice-input/mvi_utils.h"
    "./src/voice-input/voice_input_overlay_utils.cpp"
    "./src/main.cpp"
)

add_executable(${MY_EXECUTABLE_NAME} WIN32
    ${SRC_FILES}
)

# Tomlplusplus, Webview2 and other head-only libraries
target_include_directories(${PROJECT_NAME}
    PRIVATE
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/include"
    "${CMAKE_SOURCE_DIR}/src/voice-input"
    "${MSIME_VENDOR_DIR}/MetasequoiaImeEngine/voice/third_party/miniaudio"
)

target_link_libraries(${PROJECT_NAME} PRIVATE MetasequoiaIme::Engine MetasequoiaIme::Voice MetasequoiaIme::VoiceCapture)
target_link_libraries(${PROJECT_NAME} PRIVATE unofficial::sqlite3::sqlite3 fmt::fmt spdlog::spdlog Boost::locale Boost::json nlohmann_json::nlohmann_json)
target_link_libraries(${PROJECT_NAME} PRIVATE OpenCC::OpenCC)
add_dependencies(${PROJECT_NAME} Dictionaries)
target_link_libraries(${PROJECT_NAME} PRIVATE winhttp)
target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl)
target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
target_link_libraries(${PROJECT_NAME} PRIVATE d2d1 dwrite dwmapi shell32 shcore ole32 bcrypt)

# The helper executables below already build at /W4. The server itself -- the process that owns
# every network call, the WebView2 hosts and the skin loader -- was still at the default /W3.
if(MSVC)
    target_compile_options(${MY_EXECUTABLE_NAME} PRIVATE /W4)
endif()

add_executable(${DICTIONARY_REPLAY_EXECUTABLE_NAME}
    "./src/user-dictionary-replay/main.cpp"
    "./src/resource/resource.h"
    "./src/resource/resource.rc"
)
target_link_libraries(${DICTIONARY_REPLAY_EXECUTABLE_NAME} PRIVATE
    MetasequoiaIme::Engine
    unofficial::sqlite3::sqlite3
)
target_compile_definitions(${DICTIONARY_REPLAY_EXECUTABLE_NAME} PRIVATE NOMINMAX=1)
add_dependencies(${PROJECT_NAME} ${DICTIONARY_REPLAY_EXECUTABLE_NAME})

# msimeui stays a separately built GUI framework component at ../ui. The product-owned emoji panel
# lives under src/emoji-panel so it can evolve separately from the framework's example applications.
# msimeui's standalone layout tests belong to its own CI job. Its EXCLUDE_FROM_ALL
# targets are not built here, so registering those tests would break Server CTest.
set(_metasequoia_saved_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../ui" "${CMAKE_BINARY_DIR}/msimeui" EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${_metasequoia_saved_build_testing}")
unset(_metasequoia_saved_build_testing)
set(EMOJI_EXECUTABLE_NAME "MetasequoiaImeEmojiPanel")
add_executable(${EMOJI_EXECUTABLE_NAME} WIN32
    "./src/emoji-panel/EmojiPanel.cpp"
    "./src/emoji-panel/EmojiPanel.h"
    "./src/emoji-panel/emoji_panel_icons.cpp"
    "./src/emoji-panel/emoji_panel_icons.h"
    "./src/emoji-panel/emoji_panel_splash.cpp"
    "./src/emoji-panel/emoji_panel_splash.h"
    "./src/emoji-panel/svg_path_geometry.cpp"
    "./src/emoji-panel/svg_path_geometry.h"
    "./src/emoji-panel/main.cpp"
    "./src/emoji-panel/emoji_panel.rc"
    "./src/emoji-panel/resource.h"
    "./src/clipboard/clipboard_history.cpp"
    "./src/clipboard/clipboard_history.h"
    "./src/utils/ime_paths.cpp"
    "./src/utils/surface_theme_config.cpp"
    "./src/utils/surface_theme_config.h"
)
add_dependencies(${MY_EXECUTABLE_NAME} ${WATCHDOG_EXECUTABLE_NAME})
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX=1)
target_link_libraries(${PROJECT_NAME} PRIVATE msimeui d3d11 dxgi dcomp)
target_link_libraries(${EMOJI_EXECUTABLE_NAME} PRIVATE msimeui unofficial::sqlite3::sqlite3 shell32 advapi32 ole32)
target_compile_definitions(${EMOJI_EXECUTABLE_NAME} PRIVATE
    NOMINMAX=1
    WIN32_LEAN_AND_MEAN=1
)
if(MSVC)
    target_compile_options(${EMOJI_EXECUTABLE_NAME} PRIVATE /W4 /permissive-)
endif()
add_dependencies(${PROJECT_NAME} ${EMOJI_EXECUTABLE_NAME})
add_custom_command(TARGET ${EMOJI_EXECUTABLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/sqlite3.dll"
        $<TARGET_FILE_DIR:${EMOJI_EXECUTABLE_NAME}>
)

set(KEYBOARD_EXECUTABLE_NAME "MetasequoiaImeKeyboardPanel")
add_executable(${KEYBOARD_EXECUTABLE_NAME} WIN32
    "./src/keyboard-panel/KeyboardPanel.cpp"
    "./src/keyboard-panel/KeyboardPanel.h"
    "./src/keyboard-panel/main.cpp"
    "./src/keyboard-panel/keyboard_panel.rc"
    "./src/keyboard-panel/resource.h"
    "./src/utils/ime_paths.cpp"
    "./src/utils/surface_theme_config.cpp"
    "./src/utils/surface_theme_config.h"
)
target_link_libraries(${KEYBOARD_EXECUTABLE_NAME} PRIVATE msimeui shell32 advapi32 ole32)
target_compile_definitions(${KEYBOARD_EXECUTABLE_NAME} PRIVATE
    NOMINMAX=1
    WIN32_LEAN_AND_MEAN=1
)
if(MSVC)
    target_compile_options(${KEYBOARD_EXECUTABLE_NAME} PRIVATE /W4 /permissive-)
endif()
add_dependencies(${PROJECT_NAME} ${KEYBOARD_EXECUTABLE_NAME})

set(HANDWRITING_EXECUTABLE_NAME "MetasequoiaImeHandwritingPanel")
add_executable(${HANDWRITING_EXECUTABLE_NAME} WIN32
    "./src/handwriting-panel/HandwritingPanel.cpp"
    "./src/handwriting-panel/HandwritingPanel.h"
    "./src/handwriting-panel/main.cpp"
    "./src/handwriting-panel/handwriting_panel.rc"
    "./src/handwriting-panel/resource.h"
    "./src/utils/ime_paths.cpp"
    "./src/utils/surface_theme_config.cpp"
    "./src/utils/surface_theme_config.h"
)
target_link_libraries(${HANDWRITING_EXECUTABLE_NAME} PRIVATE msimeui windowsapp.lib shell32 advapi32 ole32)
target_compile_definitions(${HANDWRITING_EXECUTABLE_NAME} PRIVATE
    NOMINMAX=1
    WIN32_LEAN_AND_MEAN=1
)
if(MSVC)
    target_compile_options(${HANDWRITING_EXECUTABLE_NAME} PRIVATE /W4 /permissive-)
endif()
add_dependencies(${PROJECT_NAME} ${HANDWRITING_EXECUTABLE_NAME})

# WebView2 loader (required for CreateCoreWebView2EnvironmentWithOptions)
set(WEBVIEW2_VCPKG_ROOT "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows")
set(WEBVIEW2_LOADER_RELEASE "${WEBVIEW2_VCPKG_ROOT}/lib/WebView2Loader.dll.lib")
set(WEBVIEW2_LOADER_DEBUG "${WEBVIEW2_VCPKG_ROOT}/debug/lib/WebView2Loader.dll.lib")
target_link_libraries(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:${WEBVIEW2_LOADER_DEBUG}>
    $<$<CONFIG:Release>:${WEBVIEW2_LOADER_RELEASE}>
)

set(SETTINGS_EXECUTABLE_NAME "MetasequoiaImeSettings")
add_executable(${SETTINGS_EXECUTABLE_NAME} WIN32
    "./src/settings/settings_app.cpp"
    "./src/settings/settings_splash.cpp"
    "./src/settings/settings_splash.h"
    "./src/settings/settings_launcher.cpp"
    "./src/settings/settings_launcher.h"
    "./src/skin/candidate_skin_catalog.cpp"
    "./src/skin/candidate_skin_catalog.h"
    "./src/settings/dictionary_manager.cpp"
    "./src/settings/dictionary_manager.h"
    "./src/settings/dictionary_validation.cpp"
    "./src/settings/dictionary_validation.h"
    "./src/config/ime_config.cpp"
    "./src/config/ime_config.h"
    "./src/voice-input/voice_providers.cpp"
    "./src/voice-input/voice_providers.h"
    "./src/clipboard/clipboard_history.cpp"
    "./src/clipboard/clipboard_history.h"
    "./src/utils/common_utils.cpp"
    "./src/utils/common_utils.h"
    "./src/utils/ime_paths.cpp"
    "./src/resource/resource.rc"
)
target_include_directories(${SETTINGS_EXECUTABLE_NAME}
    PRIVATE
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/include"
)
target_link_libraries(${SETTINGS_EXECUTABLE_NAME} PRIVATE
    MetasequoiaIme::Engine
    fmt::fmt
    Boost::locale
    Boost::json
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    spdlog::spdlog
    cpp-pinyin::cpp-pinyin
    dwmapi
    dcomp
    d2d1
    dwrite
    shell32
    $<$<CONFIG:Debug>:${WEBVIEW2_LOADER_DEBUG}>
    $<$<CONFIG:Release>:${WEBVIEW2_LOADER_RELEASE}>
)
if(MSVC)
    target_compile_options(${SETTINGS_EXECUTABLE_NAME} PRIVATE /W4)
endif()
# The server launches the settings executable from the same output directory.
# Building the server target alone must therefore also produce Settings.
add_dependencies(${PROJECT_NAME} ${SETTINGS_EXECUTABLE_NAME})
add_custom_command(TARGET ${SETTINGS_EXECUTABLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<$<CONFIG:Debug>:${WEBVIEW2_VCPKG_ROOT}/debug/bin/WebView2Loader.dll>
        $<$<CONFIG:Release>:${WEBVIEW2_VCPKG_ROOT}/bin/WebView2Loader.dll>
        $<TARGET_FILE_DIR:${SETTINGS_EXECUTABLE_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${MSIME_VENDOR_DIR}/cpp-pinyin/res/dict"
        "$<TARGET_FILE_DIR:${SETTINGS_EXECUTABLE_NAME}>/dict"
)

# Ensure the loader DLL is next to the executable
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<$<CONFIG:Debug>:${WEBVIEW2_VCPKG_ROOT}/debug/bin/WebView2Loader.dll>
        $<$<CONFIG:Release>:${WEBVIEW2_VCPKG_ROOT}/bin/WebView2Loader.dll>
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/audios
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/assets/audios/start.mp3
        ${CMAKE_SOURCE_DIR}/assets/audios/end.mp3
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/audios
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/opencc
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSIME_VENDOR_DIR}/opencc/data/config/s2t.json
        ${CMAKE_BINARY_DIR}/opencc/data/CJK_Compatibility_Ideographs.ocd2
        ${CMAKE_BINARY_DIR}/opencc/data/STCharacters.ocd2
        ${CMAKE_BINARY_DIR}/opencc/data/STPhrases.ocd2
        ${CMAKE_BINARY_DIR}/opencc/data/STPhrases_GeneratedFromRegionalPhrases.ocd2
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/opencc
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
