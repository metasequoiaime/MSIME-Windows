set(TEST_EXECUTABLE_NAME "MetasequoiaImeServerTests")

set(TEST_SOURCE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_date_time_query.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_emoji_query.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_kaomoji_query.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_jianpin_query.cpp"
    "${CMAKE_SOURCE_DIR}/src/clipboard/clipboard_history.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_kaomoji_sql.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_shuangpin_scheme.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_quanpin_scheme.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_wubi.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_japanese_romaji.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_shuangpin_query.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_engine_shuangpin_session.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_english_dictionary.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_translation_gloss.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_default_config_credentials.cpp"
    "${CMAKE_SOURCE_DIR}/src/cloud/translation_gloss.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_custom_translation.cpp"
    "${CMAKE_SOURCE_DIR}/src/cloud/custom_translation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_active_client_state.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_outbound_session_state.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_pipe_write_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_input_key_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_ui_owner.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_async_request_origin.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_ui_action_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_ui_state.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_view_model.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_text_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_window_template.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_inline_protocol.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_settings_performance.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_size_estimator.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/candidate_size_estimator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_ipc_protocol_constants.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_voice_composition_pipe.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_voice_providers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_voice_batch_protocol.cpp"
    "${CMAKE_SOURCE_DIR}/src/voice-input/voice_batch_protocol.cpp"
    "${CMAKE_SOURCE_DIR}/src/voice-input/voice_providers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_floating_toolbar_visibility_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_terminal_deactivation_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_user_dictionary_journal.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_config_template_merge.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_config_non_ascii_path.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_dictionary_replay_non_ascii_path.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_skin_catalog_non_ascii_path.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_non_ascii_path_contract.cpp"
    "${CMAKE_SOURCE_DIR}/src/skin/candidate_skin_catalog.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_system_font_family.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_dictionary_validation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_skin_css_policy.cpp"
    "${CMAKE_SOURCE_DIR}/src/webview2/skin_css_policy.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config.cpp"
    "${CMAKE_SOURCE_DIR}/src/session/engine_input_session.cpp"
    "${CMAKE_SOURCE_DIR}/src/session/session_factory.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/common_utils.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/ime_paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/settings/dictionary_validation.cpp"
)

add_executable(${TEST_EXECUTABLE_NAME} ${TEST_SOURCE_FILES})

target_include_directories(${TEST_EXECUTABLE_NAME}
    PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${MSIME_VENDOR_DIR}"
    "${MSIME_VENDOR_DIR}/MetasequoiaImeEngine/utfcpp/source"
)

# test_default_config_credentials reads the config the installer actually ships, so the check is
# against the shipped file rather than a restatement of today's placeholder spellings.
# test_dictionary_replay_non_ascii_path launches the real replay tool, because the path handling it
# guards lives in that executable's main() and cannot be linked into the test binary.
target_compile_definitions(${TEST_EXECUTABLE_NAME}
    PRIVATE
    "MSIME_DEFAULT_CONFIG_PATH=\"${CMAKE_SOURCE_DIR}/../installer/default_config/config.default.toml\""
    "MSIME_DICTIONARY_REPLAY_EXE=\"$<TARGET_FILE:${DICTIONARY_REPLAY_EXECUTABLE_NAME}>\""
)

add_dependencies(${TEST_EXECUTABLE_NAME} ${DICTIONARY_REPLAY_EXECUTABLE_NAME})

target_link_libraries(${TEST_EXECUTABLE_NAME}
    PRIVATE
    MetasequoiaIme::Engine
    MetasequoiaIme::Voice
    nlohmann_json::nlohmann_json
    fmt::fmt
    spdlog::spdlog
    Boost::locale
    Boost::json
    CURL::libcurl
    unofficial::sqlite3::sqlite3
    dwrite
    shell32
    advapi32
    ole32
)

add_test(NAME ${TEST_EXECUTABLE_NAME} COMMAND ${TEST_EXECUTABLE_NAME})

# The identical fixture file also runs in Engine's JavaScript contract CI.
add_executable(test_webview_contract "${MSIME_VENDOR_DIR}/MetasequoiaImeEngine/contracts/webview/test.cpp")
target_link_libraries(test_webview_contract PRIVATE Boost::json)
add_test(NAME webview_contract COMMAND test_webview_contract
    "${MSIME_VENDOR_DIR}/MetasequoiaImeEngine/contracts/webview/fixtures.json")
