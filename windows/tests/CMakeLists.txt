cmake_minimum_required(VERSION 3.25)

# Also usable without the TSF DLL, the Windows SDK or the engine.
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

add_executable(test_commit_candidate_and_continue_payload commit_candidate_and_continue_payload.cpp)
target_include_directories(test_commit_candidate_and_continue_payload PRIVATE ../src)
target_compile_features(test_commit_candidate_and_continue_payload PRIVATE cxx_std_17)
add_test(NAME commit_candidate_and_continue_payload COMMAND test_commit_candidate_and_continue_payload)

# The classifier and the event queue are compiled into this target directly:
# they must stay free of TSF and DLL lifecycle dependencies.
add_executable(test_statistics statistics.cpp ../src/Statistics/char_classify.cpp)
target_include_directories(test_statistics PRIVATE ../src)
target_compile_features(test_statistics PRIVATE cxx_std_17)
add_test(NAME statistics COMMAND test_statistics)

add_executable(test_key_focus_recovery key_focus_recovery.cpp)
target_include_directories(test_key_focus_recovery PRIVATE ../src)
target_compile_features(test_key_focus_recovery PRIVATE cxx_std_17)
add_test(NAME key_focus_recovery COMMAND test_key_focus_recovery)

add_executable(test_caret_anchor_policy caret_anchor_policy.cpp)
target_include_directories(test_caret_anchor_policy PRIVATE ../src)
target_compile_features(test_caret_anchor_policy PRIVATE cxx_std_17)
add_test(NAME caret_anchor_policy COMMAND test_caret_anchor_policy)

add_executable(test_focus_announcement_policy focus_announcement_policy.cpp)
target_include_directories(test_focus_announcement_policy PRIVATE ../src)
target_compile_features(test_focus_announcement_policy PRIVATE cxx_std_17)
add_test(NAME focus_announcement_policy COMMAND test_focus_announcement_policy)
