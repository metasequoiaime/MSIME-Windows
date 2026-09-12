cmake_minimum_required(VERSION 3.25.0)

cmake_policy(SET CMP0167 OLD)

project(MetasequoiaImeEngineTest VERSION 1.0 LANGUAGES CXX)

# Specify the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_definitions(
    /D_UNICODE=1
    /DUNICODE=1
    # Windows.h（经 data_path.h）默认定义的 min/max 宏会破坏引擎源里的
    # std::min/std::max；正式构建（engine/CMakeLists.txt）同样公开定义
    # NOMINMAX，这里保持一致。
    /DNOMINMAX
)

# For utf8cpp, make it use C++11 and later
add_compile_options(/Zc:__cplusplus)

# Boost
# Resolve Boost without pinning one developer's machine: an explicit -DBoost_ROOT wins, then Boost_ROOT/BOOST_ROOT from the environment, and only then the scoop layout this project has historically assumed. Leaving all three unset lets find_package fall back to its own search instead of failing on a path that exists nowhere.
if(NOT DEFINED Boost_ROOT)
    if(DEFINED ENV{Boost_ROOT})
        set(Boost_ROOT "$ENV{Boost_ROOT}")
    elseif(DEFINED ENV{BOOST_ROOT})
        set(Boost_ROOT "$ENV{BOOST_ROOT}")
    elseif(DEFINED ENV{USERPROFILE} AND EXISTS "$ENV{USERPROFILE}/scoop/apps/boost/current")
        set(Boost_ROOT "$ENV{USERPROFILE}/scoop/apps/boost/current")
    endif()
endif()
set (Boost_USE_STATIC_LIBS ON)
find_package(Boost REQUIRED COMPONENTS locale)
include_directories(${Boost_INCLUDE_DIRS})

# fmt, spdlog and sqlite3
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

include_directories("./includes/")
include_directories("../")
include_directories("../include")
include_directories("../utfcpp/source/")

set(MY_EXECUTABLE_NAME "imetest")
set(
    ENGINE_SOURCE_FILES
    "../core/candidate_queries.cpp"
    "../core/ime_session.cpp"
    "../core/input_session.cpp"
    "../core/input_session_candidates.cpp"
    "../core/input_session_composition.cpp"
    "../core/input_session_editing.cpp"
    "../core/nine_key_session.cpp"
    "../core/pinyin_decoder.cpp"
    "../core/punctuation_policy.cpp"
    "../core/runtime_paths.cpp"
    "../core/session.cpp"
    "../local_modes/date_time_query.cpp"
    "../local_modes/emoji_query.cpp"
    "../local_modes/jianpin_query.cpp"
    "../local_modes/kaomoji_query.cpp"
    "../local_modes/quick_phrase_query.cpp"
    "../local_modes/unicode_query.cpp"
    "../providers/pinyin_candidate_provider.cpp"
    "../providers/wubi_candidate_provider.cpp"
    "../providers/japanese_candidate_provider.cpp"
    "../providers/provider_registry.cpp"
    "../schemes/shuangpin_scheme.cpp"
    "../schemes/quanpin_scheme.cpp"
    "../schemes/wubi_scheme.cpp"
    "../schemes/japanese_romaji_scheme.cpp"
    "../quanpin/engine.cpp"
    "../quanpin/quanpin_dictionary.cpp"
    "../quanpin/quanpin_query.cpp"
    "../quanpin/quanpin_utils.cpp"
    "../quanpin/word_lattice.cpp"
    "../shuangpin/engine.cpp"
    "../common/helpcode_utils.cpp"
    "../common/string_utils.cpp"
    "../shuangpin/shuangpin_dictionary.cpp"
    "../shuangpin/shuangpin_profile.cpp"
    "../shuangpin/shuangpin_query.cpp"
    "../shuangpin/shuangpin_utils.cpp"
    "../english/english_dictionary.cpp"
    "../japanese/japanese_matrix_search.cpp"
    "../japanese/japanese_sentence_decoder.cpp"
    "../japanese/romaji_converter.cpp"
    "../user_dictionary/user_dictionary_journal.cpp"
    "../user_dictionary/personal_dictionary.cpp"
    "../user_dictionary/dictionary_state.cpp"
    # Google IME
    "../googlepinyinime-rev/src/share/dictbuilder.cpp"
    "../googlepinyinime-rev/src/share/dictlist.cpp"
    "../googlepinyinime-rev/src/share/dicttrie.cpp"
    "../googlepinyinime-rev/src/share/lpicache.cpp"
    "../googlepinyinime-rev/src/share/matrixsearch.cpp"
    "../googlepinyinime-rev/src/share/mystdlib.cpp"
    "../googlepinyinime-rev/src/share/ngram.cpp"
    "../googlepinyinime-rev/src/share/pinyinime.cpp"
    "../googlepinyinime-rev/src/share/searchutility.cpp"
    "../googlepinyinime-rev/src/share/spellingtable.cpp"
    "../googlepinyinime-rev/src/share/spellingtrie.cpp"
    "../googlepinyinime-rev/src/share/splparser.cpp"
    "../googlepinyinime-rev/src/share/sync.cpp"
    "../googlepinyinime-rev/src/share/userdict.cpp"
    "../googlepinyinime-rev/src/share/utf16char.cpp"
    "../googlepinyinime-rev/src/share/utf16reader.cpp"
)

# imetest 的 main 在 test_pinyin.cpp；引擎源单独成变量，便于测试二进制之间复用。
set(
    SOURCE_FILES
    "./src/test_pinyin.cpp"
    ${ENGINE_SOURCE_FILES}
)

add_executable(${MY_EXECUTABLE_NAME} ${SOURCE_FILES} )

target_link_libraries(${MY_EXECUTABLE_NAME} fmt::fmt spdlog::spdlog unofficial::sqlite3::sqlite3 Boost::locale)
