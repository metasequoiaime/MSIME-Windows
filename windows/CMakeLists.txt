cmake_minimum_required(VERSION 3.25.0)
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()

# TSF tip loads into host processes (QQ/Chrome/...). Static CRT avoids
# colliding with the host's MSVCP140/VCRUNTIME140 (common QQ crash signature).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(PROJECT_NAME MetasequoiaImeTsf)
project(${PROJECT_NAME} LANGUAGES CXX C)

# Release binaries need usable symbols for crash diagnosis. Keep the PDB beside the DLL so the
# release job can validate and publish it without changing the binary's install layout.
if(MSVC)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")
    add_compile_options("$<$<CONFIG:Release>:/Zi>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG>")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(test_windows_ipc_contract
        ../vendor/MetasequoiaImeEngine/contracts/tests/windows_ipc_contract.cpp)
    target_compile_features(test_windows_ipc_contract PRIVATE cxx_std_17)
    add_test(NAME windows_ipc_contract COMMAND test_windows_ipc_contract)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions(
    /D_UNICODE=1
    /DUNICODE=1
)

# For utf8cpp, make it use C++11 and later
add_compile_options(/Zc:__cplusplus)
# Match WeaselTSF: avoid dynamic TLS init guards in in-proc tip DLLs
add_compile_options(/Zc:threadSafeInit-)

# This DLL is loaded into Chrome, QQ, Office and every other host the user types into, and it parses
# what those hosts hand it -- yet it was building at the default /W3 while ui/ and server/ already
# use /W4 on their own targets. /WX is deliberately not set yet: the existing warnings have to be
# read and cleared on Windows first, and turning them into errors before that would only mean
# reaching for /wd suppressions. /permissive- is likewise left for a follow-up that can build here.
add_compile_options(/W4)

# add_definitions(
#     /DFANY_DEBUG=1
#     /DFANY_IPC_DEBUG=1
# )

# fmt
find_package(fmt CONFIG REQUIRED)

# For pure-header libs installed by vcpkg like utf8cpp
if(DEFINED VCPKG_TARGET_TRIPLET)
    include_directories("${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include")
endif()

include_directories("./src")
include_directories("./src/Candidate")
include_directories("./src/Compartment")
include_directories("./src/Composition")
include_directories("./src/DisplayAttribute")
include_directories("./src/Edit")
include_directories("./src/FanyLog")
include_directories("./src/Global")
include_directories("./src/Header")
include_directories("./src/IME")
include_directories("./src/IPC")
include_directories("./src/Key")
include_directories("./src/LanguageBar")
include_directories("./src/Register")
include_directories("./src/Tf")
include_directories("./src/Thread")
include_directories("./src/UI")
include_directories("./src/Utils")

add_library(${PROJECT_NAME} SHARED
    # Candidate
    "./src/Candidate/EnumTfCandidates.cpp"
    "./src/Candidate/EnumTfCandidates.h"
    "./src/Candidate/SearchCandidateProvider.cpp"
    "./src/Candidate/SearchCandidateProvider.h"
    "./src/Candidate/TipCandidateList.cpp"
    "./src/Candidate/TipCandidateList.h"
    "./src/Candidate/TipCandidateString.cpp"
    "./src/Candidate/TipCandidateString.h"
    # Compartment
    "./src/Compartment/Compartment.cpp"
    "./src/Compartment/Compartment.h"
    # Composition
    "./src/Composition/Composition.cpp"
    "./src/Composition/CompositionProcessorEngine.cpp"
    "./src/Composition/CompositionProcessorEngine.h"
    "./src/Composition/EndComposition.cpp"
    "./src/Composition/StartComposition.cpp"
    # DisplayAttribute
    "./src/DisplayAttribute/DisplayAttribute.cpp"
    "./src/DisplayAttribute/DisplayAttributeInfo.cpp"
    "./src/DisplayAttribute/DisplayAttributeInfo.h"
    "./src/DisplayAttribute/DisplayAttributeProvider.cpp"
    "./src/DisplayAttribute/EnumDisplayAttributeInfo.cpp"
    "./src/DisplayAttribute/EnumDisplayAttributeInfo.h"
    # Edit
    "./src/Edit/EditSession.cpp"
    "./src/Edit/EditSession.h"
    "./src/Edit/GetTextExtentEditSession.cpp"
    "./src/Edit/GetTextExtentEditSession.h"
    "./src/Edit/TextEditSink.cpp"
    # Global
    "./src/Global/FanyDefines.h"
    "./src/Global/Globals.cpp"
    "./src/Global/Globals.h"
    # Header
    "./src/Header/Define.h"
    "./src/Header/Private.h"
    "./src/Header/resource.h"
    "./src/Header/stdafx.h"
    # IME
    "./src/IME/ActiveLanguageProfileNotifySink.cpp"
    "./src/IME/FunctionProviderSink.cpp"
    "./src/IME/MetasequoiaIME.cpp"
    "./src/IME/MetasequoiaIME.def"
    "./src/IME/MetasequoiaIME.h"
    "./src/IME/MetasequoiaIME.rc"
    "./src/IME/MetasequoiaIMEBaseStructure.cpp"
    "./src/IME/MetasequoiaIMEBaseStructure.h"
    "./src/IME/MetasequoiaIMEStructureArray.h"
    # IPC
    "./src/IPC/Ipc.cpp"
    "./src/IPC/Ipc.h"
    "./src/IPC/VoiceCompositionPipe.h"
    # Key
    "./src/Key/KeyEventSink.cpp"
    "./src/Key/KeyHandler.cpp"
    "./src/Key/KeyHandlerEditSession.cpp"
    "./src/Key/KeyHandlerEditSession.h"
    "./src/Key/KeyStateCategory.cpp"
    "./src/Key/KeyStateCategory.h"
    # LanguageBar
    "./src/LanguageBar/LanguageBar.cpp"
    "./src/LanguageBar/LanguageBar.h"
    # Register
    "./src/Register/Register.cpp"
    "./src/Register/RegKey.cpp"
    "./src/Register/RegKey.h"
    # Tf
    "./src/Tf/TfInputProcessorProfile.cpp"
    "./src/Tf/TfInputProcessorProfile.h"
    "./src/Tf/TfTextLayoutSink.cpp"
    "./src/Tf/TfTextLayoutSink.h"
    # Thread
    "./src/Thread/ThreadFocusSink.cpp"
    "./src/Thread/ThreadMgrEventSink.cpp"
    # UI
    "./src/UI/CandidateSessionState.cpp"
    "./src/UI/CandidateSessionState.h"
    "./src/UI/CandidateListUIPresenter.cpp"
    "./src/UI/CandidateListUIPresenter.h"
    # Utils
    "./src/Utils/CommonUtils.cpp"
    "./src/Utils/CommonUtils.h"
    "./src/Utils/FanyUtils.cpp"
    "./src/Utils/FanyUtils.h"
    # Main
    "./src/DllMain.cpp"
    "./src/Server.cpp"
)

target_link_libraries(${PROJECT_NAME} PRIVATE fmt::fmt-header-only)

set_property(TARGET ${PROJECT_NAME} PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
