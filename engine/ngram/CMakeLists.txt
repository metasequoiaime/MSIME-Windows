# kenlm 查询子集 + 本仓的语言模型封装。
#
# 只编译 kenlm 的 query / trie-build 路径：lm/ 与 util/ 的顶层源文件，外加
# util/double-conversion。lm/builder、lm/filter、lm/interpolate、lm/wrappers
# 没有复制进来，顶层源文件也不 include 它们。
#
# 不要定义 WITH_THREADS。定义之后 util/parallel_read.cc 会走 thread_pool.hh
# 那条分支，从而拖进 boost::thread 这个需要编译的库；不定义时它退化成串行的
# ErsatzPRead，整个子集因此是零 Boost 依赖的。
#
# KENLM_MAX_ORDER 必须与生成 .lm 时使用的值一致。libime 用的是 3，出货的
# sc.lm 也是三元模型，改这里等于让运行时读不懂已经发布的模型文件。
#
# language_model.cpp 故意编进同一个目标：它是唯一 include kenlm 头文件的地方。
# 这样 _HAS_AUTO_PTR_ETC 和 kenlm 的 include 目录都能保持 PRIVATE，engine 的
# 其余部分只看得到 language_model.h 这个纯 std 的头。

# 这个目录既被 engine/ 和 engine/tests/ 用 add_subdirectory 挂进去，也可以单独
# configure —— scripts/build-language-model.ps1 就是这么用的：它只要 ARPA -> .lm
# 那个工具，不该为了 engine 依赖的 SQLite 去拉起 vcpkg。作为子目录时下面这段不执行，
# 顶层工程的 C++ 标准和策略照旧由父目录决定。
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(metasequoia_ime_ngram_tools CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

# kenlm 全程用窄字符调 Win32（util/file.cc 里的 GetTempPath 就收一个 char[]），
# 所以这里必须在 UNICODE/_UNICODE 之外编。add_subdirectory 会继承父目录用
# add_definitions 设的宏，engine/tests/CMakeLists.txt 正好就是这么定义 UNICODE 的，
# 于是 GetTempPath 解析成 GetTempPathW 而编不过。清掉本目录继承来的那一份；
# 引擎自己那套 PUBLIC 的 UNICODE 走的是 target_compile_definitions，不经过这里。
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "")

file(GLOB METASEQUOIA_IME_KENLM_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/kenlm/lm/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/kenlm/util/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/kenlm/util/double-conversion/*.cc")
# *_main.cc 带 main()，是工具的入口而不是库的一部分。它们留在上游布局里，
# 因为其中的 #include "model.hh" 是相对自身目录解析的，挪走就编不过。
list(FILTER METASEQUOIA_IME_KENLM_SOURCES EXCLUDE REGEX "_main\\.cc$")

add_library(metasequoia_ime_ngram STATIC
    ${METASEQUOIA_IME_KENLM_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/language_model.cpp")

# PUBLIC 指向仓库根，这样调用方按 AGENTS.md 的约定写
# #include "engine/ngram/language_model.h"。kenlm 的头保持 PRIVATE。
target_include_directories(metasequoia_ime_ngram
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/kenlm")
target_compile_definitions(metasequoia_ime_ngram PRIVATE KENLM_MAX_ORDER=3)
set_target_properties(metasequoia_ime_ngram PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    # kenlm 仍在用 C++17 移除的 std::binary_function（trie_sort.hh、
    # sized_iterator.hh、joint_sort.hh）。_HAS_AUTO_PTR_ETC 把这批符号加回来，
    # 这是 MSVC 侧对应 libime 给 libc++ 加的
    # _LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION。
    # 它只恢复符号、不改变既有代码行为，且保持 PRIVATE 不外泄。
    target_compile_definitions(metasequoia_ime_ngram PRIVATE
        _HAS_AUTO_PTR_ETC=1 NOMINMAX _CRT_SECURE_NO_WARNINGS)
    # 上游副本，不为它改代码风格，也不逐个修收窄转换告警。
    target_compile_options(metasequoia_ime_ngram PRIVATE /wd4244 /wd4267 /wd4996 /wd4018 /wd4334)
    # language_model.{h,cpp} 带中文 UTF-8 注释，而本目标没继承 engine 的 /utf-8：
    # MSVC 按系统代码页（GBK）解码时注释尾字节会吞掉换行，直接编不过。kenlm 上游源码
    # 是纯 ASCII，/utf-8 对它们无感。
    target_compile_options(metasequoia_ime_ngram PRIVATE /utf-8)
endif()

# kenlm 的 _open 走 ANSI 代码页，本仓的路径是 UTF-8。强制包含一个只声明
# util::_open 重载的头把它接到宽字符 API 上，不改上游源码。理由见该头文件。
if(WIN32)
    if(MSVC)
        set(METASEQUOIA_IME_KENLM_FILE_IO_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/kenlm_file_io.h")
    else()
        set(METASEQUOIA_IME_KENLM_FILE_IO_OPTIONS -include "${CMAKE_CURRENT_SOURCE_DIR}/kenlm_file_io.h")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/kenlm/util/file.cc"
        APPEND PROPERTY COMPILE_OPTIONS ${METASEQUOIA_IME_KENLM_FILE_IO_OPTIONS})
endif()

# 把 ARPA 转成出货的 sc.lm 的工具。默认不建：它只在重新生成模型数据时才需要，
# 产品构建不依赖它。开启方式见 README.md 的"重新生成语言模型"。
option(METASEQUOIA_IME_BUILD_NGRAM_TOOLS "Build the ARPA -> .lm conversion tool" OFF)
if(METASEQUOIA_IME_BUILD_NGRAM_TOOLS)
    # getopt.c 是 kenlm 自带的 Windows 移植（MSVC 的 CRT 没有 getopt），只有
    # 工具入口用得到，库本身不需要，所以不进上面那个 glob。
    # 它必须按 C 编译：getopt.hh 用 extern "C" 声明这些符号。enable_language 放在
    # 这个默认关闭的分支里，产品构建因此仍然是纯 CXX 工程。
    enable_language(C)
    add_executable(metasequoia_ime_build_binary
        "${CMAKE_CURRENT_SOURCE_DIR}/kenlm/lm/build_binary_main.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/kenlm/util/getopt.c")
    target_link_libraries(metasequoia_ime_build_binary PRIVATE metasequoia_ime_ngram)
    target_include_directories(metasequoia_ime_build_binary PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/kenlm")
    target_compile_definitions(metasequoia_ime_build_binary PRIVATE KENLM_MAX_ORDER=3)
    if(MSVC)
        target_compile_definitions(metasequoia_ime_build_binary PRIVATE
            _HAS_AUTO_PTR_ETC=1 NOMINMAX _CRT_SECURE_NO_WARNINGS)
        target_compile_options(metasequoia_ime_build_binary PRIVATE /wd4244 /wd4267 /wd4996)
    endif()
endif()
