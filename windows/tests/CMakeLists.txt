cmake_minimum_required(VERSION 3.25)

# Also usable without the TSF DLL, Windows SDK or Engine submodules.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MsimeTsfLocalPolicyTests LANGUAGES CXX)
    enable_testing()
endif()

add_executable(test_client_fallback client_fallback.cpp)
target_include_directories(test_client_fallback PRIVATE ../src)
target_compile_features(test_client_fallback PRIVATE cxx_std_17)
add_test(NAME client_fallback COMMAND test_client_fallback)
