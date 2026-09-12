#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ipc/ipc.h"
#include "ipc/input_key_policy.h"
#include "tests/includes/test_framework.h"

TEST_CASE(ipc_reverse_pipes_buffer_multiple_complete_frames)
{
    REQUIRE(FANY_IME_TO_TSF_PIPE_FRAME_CAPACITY >= 64);
    REQUIRE(FANY_IME_TO_TSF_WORKER_PIPE_FRAME_CAPACITY >= 16);
    REQUIRE(sizeof(FanyImeNamedpipeDataToTsf) * FANY_IME_TO_TSF_PIPE_FRAME_CAPACITY >=
            sizeof(FanyImeNamedpipeDataToTsf) * 64);
    REQUIRE(sizeof(FanyImeNamedpipeDataToTsfWorkerThread) * FANY_IME_TO_TSF_WORKER_PIPE_FRAME_CAPACITY >=
            sizeof(FanyImeNamedpipeDataToTsfWorkerThread) * 16);
}

TEST_CASE(tsf_diagnostic_batches_have_a_bounded_versioned_frame)
{
    REQUIRE_EQ(FANY_IME_TSF_DIAGNOSTIC_MAGIC, 0x474F4C54u);
    REQUIRE_EQ(FANY_IME_TSF_DIAGNOSTIC_VERSION, 1u);
    REQUIRE_EQ(sizeof(FanyImeTsfDiagnosticBatchHeader), 28u);
    REQUIRE_EQ(FANY_IME_TSF_DIAGNOSTIC_MAX_FRAME_BYTES, 16u * 1024u);
    REQUIRE(sizeof(FanyImeTsfDiagnosticBatchHeader) < FANY_IME_TSF_DIAGNOSTIC_MAX_FRAME_BYTES);
}

TEST_CASE(ipc_pipe_ready_is_a_distinct_server_reply)
{
    REQUIRE_EQ(Global::DataFromServerMsgType::PipeReady, 9u);
    REQUIRE(Global::DataFromServerMsgType::PipeReady > Global::DataFromServerMsgType::MovePageNext);
    REQUIRE_EQ(Global::DataFromServerMsgType::CommitExactText, 10u);
    REQUIRE(Global::DataFromServerMsgType::CommitExactText > Global::DataFromServerMsgType::PipeReady);
    REQUIRE_EQ(Global::DataFromServerMsgType::UiLessComposition, 11u);
    REQUIRE(Global::DataFromServerMsgType::UiLessComposition > Global::DataFromServerMsgType::CommitExactText);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::FocusSessionReady, 8u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::PipeReady, 9u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::InsertText, 10u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationChanged, 11u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::PairedPunctuationChanged, 12u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::MicrosoftShuangpinChanged, 13u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationRepeatToChineseChanged, 14u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::UpdateVoiceComposition, 15u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::CancelVoiceComposition, 16u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::CommitVoiceComposition, 17u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::InputModeChanged, 18u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::CapsLockChanged, 19u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::TsfDiagnosticLogChanged, 20u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::PunctuationLockChanged, 21u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::LowercaseNumberModeChanged, 22u);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::MaxKnown, 22u);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::FocusSessionReady >
            Global::DataFromServerMsgTypeToTsfWorkerThread::PagingCommaPeriodChanged);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::PipeReady >
            Global::DataFromServerMsgTypeToTsfWorkerThread::FocusSessionReady);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::InsertText >
            Global::DataFromServerMsgTypeToTsfWorkerThread::PipeReady);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationChanged >
            Global::DataFromServerMsgTypeToTsfWorkerThread::InsertText);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::PairedPunctuationChanged >
            Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationChanged);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::MicrosoftShuangpinChanged >
            Global::DataFromServerMsgTypeToTsfWorkerThread::PairedPunctuationChanged);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationRepeatToChineseChanged >
            Global::DataFromServerMsgTypeToTsfWorkerThread::MicrosoftShuangpinChanged);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::UpdateVoiceComposition >
            Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationRepeatToChineseChanged);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::CancelVoiceComposition >
            Global::DataFromServerMsgTypeToTsfWorkerThread::UpdateVoiceComposition);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::CommitVoiceComposition >
            Global::DataFromServerMsgTypeToTsfWorkerThread::CancelVoiceComposition);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::InputModeChanged >
            Global::DataFromServerMsgTypeToTsfWorkerThread::CommitVoiceComposition);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::CapsLockChanged >
            Global::DataFromServerMsgTypeToTsfWorkerThread::InputModeChanged);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::TsfDiagnosticLogChanged >
            Global::DataFromServerMsgTypeToTsfWorkerThread::CapsLockChanged);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::PunctuationLockChanged >
            Global::DataFromServerMsgTypeToTsfWorkerThread::TsfDiagnosticLogChanged);
    REQUIRE(Global::DataFromServerMsgTypeToTsfWorkerThread::LowercaseNumberModeChanged >
            Global::DataFromServerMsgTypeToTsfWorkerThread::PunctuationLockChanged);
    REQUIRE_EQ(Global::DataFromServerMsgTypeToTsfWorkerThread::MaxKnown,
               Global::DataFromServerMsgTypeToTsfWorkerThread::LowercaseNumberModeChanged);
}

TEST_CASE(ipc_client_suspension_is_a_distinct_nonterminal_route_reset)
{
    REQUIRE_EQ(FanyImePipeEventType::ClientActivated, 11u);
    REQUIRE_EQ(FanyImePipeEventType::ClientDeactivated, 12u);
    REQUIRE_EQ(FanyImePipeEventType::StatusSnapshot, 13u);
    REQUIRE_EQ(FanyImePipeEventType::ClientSuspended, 14u);

    REQUIRE(FanyImePipeEventType::IsRouteDeactivation(FanyImePipeEventType::ClientDeactivated));
    REQUIRE(FanyImePipeEventType::IsRouteDeactivation(FanyImePipeEventType::ClientSuspended));
    REQUIRE(FanyImePipeEventType::IsTerminalDeactivation(FanyImePipeEventType::ClientDeactivated));
    REQUIRE(!FanyImePipeEventType::IsTerminalDeactivation(FanyImePipeEventType::ClientSuspended));
    REQUIRE(!FanyImePipeEventType::IsRouteDeactivation(FanyImePipeEventType::ClientActivated));
}

TEST_CASE(ipc_focus_restored_is_an_appended_opcode_and_not_a_route_reset)
{
    REQUIRE_EQ(FanyImePipeEventType::FocusRestored, 15u);
    REQUIRE(!FanyImePipeEventType::IsRouteDeactivation(FanyImePipeEventType::FocusRestored));
    REQUIRE(!FanyImePipeEventType::IsTerminalDeactivation(FanyImePipeEventType::FocusRestored));
}

TEST_CASE(ipc_uiless_flag_is_outside_key_modifier_mask)
{
    REQUIRE_EQ(FanyImePipeFlags::UiLess, 0x80000000u);
    REQUIRE((FanyImePipeFlags::UiLess & FanyImeIpc::kKeyModifierMask) == 0u);
    REQUIRE((FanyImeIpc::kModifierUiLess & FanyImeIpc::kKeyModifierMask) == 0u);
}

TEST_CASE(ipc_pipe_dacl_admits_only_the_owning_account)
{
    const std::wstring sddl = FanyImeIpc::BuildPipeSecurityDescriptorSddl(L"S-1-5-21-1-2-3-1001");

    // Everyone must never appear: the pipe namespace is machine-global, so a WD ACE lets any local account connect to
    // the server, and FILE_ALL_ACCESS additionally lets it add instances to a name the server already owns.
    REQUIRE(sddl.find(L";;;WD)") == std::wstring::npos);
    REQUIRE(sddl.find(L"(A;;FA;;;S-1-5-21-1-2-3-1001)") != std::wstring::npos);
    REQUIRE(sddl.find(L"(A;;FA;;;SY)") != std::wstring::npos);
    REQUIRE(sddl.find(L"S:(ML;;NW;;;LW)") != std::wstring::npos);
}

TEST_CASE(ipc_pipe_dacl_gives_app_containers_connect_only_rights)
{
    const std::wstring sddl = FanyImeIpc::BuildPipeSecurityDescriptorSddl(L"S-1-5-21-1-2-3-1001");

    REQUIRE(sddl.find(L"(A;;FA;;;AC)") == std::wstring::npos);
    REQUIRE(sddl.find(L"(A;;0x12019b;;;AC)") != std::wstring::npos);
    REQUIRE((FanyImeIpc::kPipeClientAccessMask & FILE_CREATE_PIPE_INSTANCE) == 0u);
    REQUIRE((FanyImeIpc::kPipeClientAccessMask & FILE_READ_DATA) != 0u);
    REQUIRE((FanyImeIpc::kPipeClientAccessMask & FILE_WRITE_DATA) != 0u);
    REQUIRE((FanyImeIpc::kPipeClientAccessMask & FILE_WRITE_ATTRIBUTES) != 0u);
    REQUIRE((FanyImeIpc::kPipeClientAccessMask & SYNCHRONIZE) != 0u);
}

TEST_CASE(ipc_pipe_security_descriptor_fails_closed_without_an_owner_sid)
{
    REQUIRE(FanyImeIpc::BuildPipeSecurityDescriptorSddl(L"").empty());
}
