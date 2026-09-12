cmake_minimum_required(VERSION 3.25)

project(MetasequoiaImeEngine VERSION 0.9.0 LANGUAGES CXX) # x-release-please-version

include(CTest)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

if(TARGET SQLite3::SQLite3)
    set(METASEQUOIA_IME_SQLITE_TARGET SQLite3::SQLite3)
else()
    set(METASEQUOIA_IME_SQLITE_TARGET SQLite::SQLite3)
endif()

get_target_property(METASEQUOIA_IME_SPDLOG_INCLUDE_DIRS spdlog::spdlog_header_only INTERFACE_INCLUDE_DIRECTORIES)

set(METASEQUOIA_IME_ENGINE_SOURCES
    common/helpcode_utils.cpp
    common/string_utils.cpp
    core/session.cpp
    core/nine_key_session.cpp
    core/candidate_queries.cpp
    core/punctuation_policy.cpp
    core/runtime_paths.cpp
    core/pinyin_decoder.cpp
    core/ime_session.cpp
    core/input_session.cpp
    core/input_session_composition.cpp
    core/input_session_editing.cpp
    core/input_session_candidates.cpp
    english/english_dictionary.cpp
    japanese/japanese_matrix_search.cpp
    japanese/japanese_sentence_decoder.cpp
    japanese/romaji_converter.cpp
    local_modes/date_time_query.cpp
    local_modes/emoji_query.cpp
    local_modes/jianpin_query.cpp
    local_modes/kaomoji_query.cpp
    local_modes/number_query.cpp
    local_modes/quick_phrase_query.cpp
    local_modes/unicode_query.cpp
    providers/japanese_candidate_provider.cpp
    providers/pinyin_candidate_provider.cpp
    providers/provider_registry.cpp
    providers/wubi_candidate_provider.cpp
    quanpin/engine.cpp
    quanpin/quanpin_dictionary.cpp
    quanpin/quanpin_query.cpp
    quanpin/quanpin_utils.cpp
    quanpin/word_lattice.cpp
    schemes/japanese_romaji_scheme.cpp
    schemes/quanpin_scheme.cpp
    schemes/shuangpin_scheme.cpp
    schemes/wubi_scheme.cpp
    shuangpin/engine.cpp
    shuangpin/shuangpin_dictionary.cpp
    shuangpin/shuangpin_profile.cpp
    shuangpin/shuangpin_query.cpp
    shuangpin/shuangpin_utils.cpp
    user_dictionary/user_dictionary_journal.cpp
    user_dictionary/personal_dictionary.cpp
    user_dictionary/dictionary_state.cpp
    googlepinyinime-rev/src/share/dictbuilder.cpp
    googlepinyinime-rev/src/share/dictlist.cpp
    googlepinyinime-rev/src/share/dicttrie.cpp
    googlepinyinime-rev/src/share/lpicache.cpp
    googlepinyinime-rev/src/share/matrixsearch.cpp
    googlepinyinime-rev/src/share/mystdlib.cpp
    googlepinyinime-rev/src/share/ngram.cpp
    googlepinyinime-rev/src/share/pinyinime.cpp
    googlepinyinime-rev/src/share/searchutility.cpp
    googlepinyinime-rev/src/share/spellingtable.cpp
    googlepinyinime-rev/src/share/spellingtrie.cpp
    googlepinyinime-rev/src/share/splparser.cpp
    googlepinyinime-rev/src/share/sync.cpp
    googlepinyinime-rev/src/share/userdict.cpp
    googlepinyinime-rev/src/share/utf16char.cpp
    googlepinyinime-rev/src/share/utf16reader.cpp
)

# The vendored decoder accepts UTF-8 paths. Adapt its namespace-local file IO to native
# Windows paths without editing googlepinyinime-rev or changing the process locale.
if(WIN32)
    if(MSVC)
        set(METASEQUOIA_PINYIN_FILE_IO_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/core/pinyin_file_io.h")
    else()
        set(METASEQUOIA_PINYIN_FILE_IO_OPTIONS -include "${CMAKE_CURRENT_SOURCE_DIR}/core/pinyin_file_io.h")
    endif()
    set_property(SOURCE googlepinyinime-rev/src/share/dicttrie.cpp googlepinyinime-rev/src/share/userdict.cpp
        APPEND PROPERTY COMPILE_OPTIONS ${METASEQUOIA_PINYIN_FILE_IO_OPTIONS})
endif()

add_library(MetasequoiaImeEngine STATIC ${METASEQUOIA_IME_ENGINE_SOURCES})
add_library(MetasequoiaIme::Engine ALIAS MetasequoiaImeEngine)

target_include_directories(MetasequoiaImeEngine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/utfcpp/source
    SYSTEM PRIVATE
        ${METASEQUOIA_IME_SPDLOG_INCLUDE_DIRS}
)

target_compile_definitions(MetasequoiaImeEngine PRIVATE FMT_HEADER_ONLY=1 SPDLOG_FMT_EXTERNAL)

target_link_libraries(MetasequoiaImeEngine
    PUBLIC
        Boost::headers
        fmt::fmt-header-only
        ${METASEQUOIA_IME_SQLITE_TARGET}
        Threads::Threads
)

if(WIN32)
    target_link_libraries(MetasequoiaImeEngine PUBLIC ole32 shell32)
endif()

if(MSVC)
    # NOMINMAX and the UNICODE pair stay PUBLIC: they change what the Windows headers declare and
    # what the TCHAR-flavoured entry points resolve to, so a consumer that disagrees with the engine
    # would be compiling against a different API surface. WIN32_LEAN_AND_MEAN is only an
    # include-time trim of Windows.h, and forcing it on consumers breaks any of them that go on to
    # include a header needing the fuller surface - WebView2.h, for one, relies on the `interface`
    # macro that combaseapi.h provides.
    target_compile_definitions(MetasequoiaImeEngine PUBLIC NOMINMAX UNICODE _UNICODE)
    target_compile_definitions(MetasequoiaImeEngine PRIVATE WIN32_LEAN_AND_MEAN)
    target_compile_options(MetasequoiaImeEngine PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(MetasequoiaImeEngine PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_executable(test_segmentation_contract tests/src/test_segmentation_contract.cpp)
    target_link_libraries(test_segmentation_contract PRIVATE MetasequoiaIme::Engine)
    add_test(NAME segmentation_contract COMMAND test_segmentation_contract)

    add_executable(test_windows_ipc_contract contracts/tests/windows_ipc_contract.cpp)
    add_test(NAME windows_ipc_contract COMMAND test_windows_ipc_contract)

    add_executable(test_punctuation_contract contracts/tests/punctuation_contract.cpp)
    if(MSVC)
        target_compile_options(test_punctuation_contract PRIVATE /utf-8)
    endif()
    add_test(NAME punctuation_contract COMMAND test_punctuation_contract)

    add_executable(test_nine_key_session tests/src/test_nine_key_session.cpp)
    target_link_libraries(test_nine_key_session PRIVATE MetasequoiaImeEngine)
    add_test(NAME nine_key_session COMMAND test_nine_key_session)

    add_executable(test_platform_api tests/src/test_platform_api.cpp tests/src/test_public_session.cpp)
    target_link_libraries(test_platform_api PRIVATE MetasequoiaIme::Engine)
    add_test(NAME platform_api COMMAND test_platform_api)

    add_executable(test_data_path tests/src/test_data_path.cpp)
    target_link_libraries(test_data_path PRIVATE MetasequoiaIme::Engine)
    add_test(NAME data_path COMMAND test_data_path)

    add_executable(test_runtime_isolation tests/src/test_runtime_isolation.cpp)
    target_compile_definitions(test_runtime_isolation PRIVATE METASEQUOIA_TEST_PINYIN_MODEL="${CMAKE_CURRENT_SOURCE_DIR}/googlepinyinime-rev/data/dict_pinyin.dat")
    target_link_libraries(test_runtime_isolation PRIVATE MetasequoiaIme::Engine)
    add_test(NAME runtime_isolation COMMAND test_runtime_isolation)

    add_executable(test_dictionary_state tests/src/test_dictionary_state.cpp)
    target_link_libraries(test_dictionary_state PRIVATE MetasequoiaImeEngine)
    add_test(NAME dictionary_state COMMAND test_dictionary_state)
    add_test(NAME dictionary_state_capacity COMMAND test_dictionary_state --capacity)

    add_executable(test_personal_dictionary tests/src/test_personal_dictionary.cpp)
    target_link_libraries(test_personal_dictionary PRIVATE MetasequoiaImeEngine)
    add_test(NAME personal_dictionary COMMAND test_personal_dictionary)

    add_executable(test_candidate_removal tests/src/test_candidate_removal.cpp)
    target_link_libraries(test_candidate_removal PRIVATE MetasequoiaIme::Engine)
    add_test(NAME candidate_removal COMMAND test_candidate_removal)

    # These two are the disk-bound outliers. Both spend their time building, staging and tearing
    # down dictionaries on disk rather than in the decoder, and Windows runs them 15-25x slower than
    # the other platforms: runtime_isolation takes 10.6s there against 0.70s on macOS, and
    # candidate_removal 7.6s against 0.30s. That leaves them alone in having single-digit headroom
    # under the Windows job's 60s default, and a runner that lost disk throughput has already pushed
    # runtime_isolation past it while the CPU-bound tests barely moved. A timeout is here to catch a
    # hang, which is unbounded, so it is set against a hang rather than against the runner's best
    # day; the other twenty tests keep the strict default.
    set_tests_properties(runtime_isolation candidate_removal PROPERTIES TIMEOUT 300)

    add_executable(test_fuzzy_pinyin tests/src/test_fuzzy_pinyin.cpp)
    target_link_libraries(test_fuzzy_pinyin PRIVATE MetasequoiaIme::Engine)
    add_test(NAME fuzzy_pinyin COMMAND test_fuzzy_pinyin)

    add_executable(test_engine_smoke tests/src/test_engine_smoke.cpp)
    target_link_libraries(test_engine_smoke PRIVATE MetasequoiaIme::Engine)
    add_test(NAME engine_smoke COMMAND test_engine_smoke)

    add_executable(test_input_session tests/src/test_input_session.cpp)
    target_compile_definitions(test_input_session PRIVATE METASEQUOIA_SKIP_FREQUENCY_TESTS)
    target_link_libraries(test_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME input_session COMMAND test_input_session)

    add_executable(test_frequency_input_session tests/src/test_input_session.cpp)
    target_compile_definitions(test_frequency_input_session PRIVATE METASEQUOIA_FREQUENCY_TESTS_ONLY)
    target_link_libraries(test_frequency_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME frequency_input_session COMMAND test_frequency_input_session)

    add_executable(test_english_input_session tests/src/test_english_input_session.cpp)
    target_link_libraries(test_english_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME english_input_session COMMAND test_english_input_session)

    add_executable(test_mixed_expressive_input_session tests/src/test_mixed_expressive_input_session.cpp)
    target_link_libraries(test_mixed_expressive_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME mixed_expressive_input_session COMMAND test_mixed_expressive_input_session)

    add_executable(test_wubi_mixed_input_session tests/src/test_wubi_mixed_input_session.cpp)
    target_link_libraries(test_wubi_mixed_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME wubi_mixed_input_session COMMAND test_wubi_mixed_input_session)

    add_executable(test_jianpin_input_session tests/src/test_jianpin_input_session.cpp)
    target_link_libraries(test_jianpin_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME jianpin_input_session COMMAND test_jianpin_input_session)

    add_executable(test_temporary_input_session tests/src/test_temporary_input_session.cpp)
    target_link_libraries(test_temporary_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME temporary_input_session COMMAND test_temporary_input_session)

    add_executable(test_local_modes tests/src/test_local_modes.cpp)
    target_link_libraries(test_local_modes PRIVATE MetasequoiaIme::Engine)
    add_test(NAME local_modes COMMAND test_local_modes)

    add_executable(test_online_input_session tests/src/test_online_input_session.cpp)
    target_link_libraries(test_online_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME online_input_session COMMAND test_online_input_session)
endif()

option(METASEQUOIA_IME_BUILD_VOICE "Build shared voice input capabilities" OFF)
if(METASEQUOIA_IME_BUILD_VOICE)
    add_subdirectory(voice)
endif()
