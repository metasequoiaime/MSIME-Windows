cmake_minimum_required(VERSION 3.25)

# Also usable without the TSF DLL or the engine. test_statistics additionally
# needs the Windows SDK for GetStringTypeW, which is fine on this repo's
# Windows-only build machines.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MsimeTsfLocalPolicyTests LANGUAGES CXX)
    enable_testing()
endif()

add_executable(test_client_fallback client_fallback.cpp)
target_include_directories(test_client_fallback PRIVATE ../src)
target_compile_features(test_client_fallback PRIVATE cxx_std_17)
add_test(NAME client_fallback COMMAND test_client_fallback)

add_executable(test_key_repeat_guard key_repeat_guard.cpp)
target_include_directories(test_key_repeat_guard PRIVATE ../src)
target_compile_features(test_key_repeat_guard PRIVATE cxx_std_17)
add_test(NAME key_repeat_guard COMMAND test_key_repeat_guard)

# The classifier uses GetStringTypeW (kernel32) only; the pending-queue logic is
# header-only, so this target stays independent of the TSF DLL and the engine.
add_executable(test_statistics statistics.cpp ../src/Statistics/char_classify.cpp)
target_include_directories(test_statistics PRIVATE ../src ../src/Statistics)
target_compile_features(test_statistics PRIVATE cxx_std_17)
add_test(NAME statistics COMMAND test_statistics)
