#include "Ipc.h"
#include <debugapi.h>
#include <handleapi.h>
#include <minwindef.h>
#include <atomic>
#include <winbase.h>
#include <winnt.h>
#include <AclAPI.h>
#include <Sddl.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include "ipc.h"
#include "engine/contracts/ipc_negotiation.h"
#include "ipc/active_client_state.h"
#include "ipc/event_listener.h"
#include "ipc/outbound_session_state.h"
#include "ipc/pipe_write_policy.h"
#include "ipc/terminal_deactivation_policy.h"
#include "utils/common_utils.h"
#include "fmt/xchar.h"

// IPC logging is compiled out. The macros still have to *mention* their arguments, otherwise every
// parameter of the log helpers below is unreferenced (C4100). sizeof keeps the arguments in an
// unevaluated context, so nothing is computed and no side effect runs — only the name is used.
template <typename... Args> int FanyIpcDiscardLogArgs(const Args &...);
#define FANY_IPC_LOG_RAW(message) ((void)sizeof(FanyIpcDiscardLogArgs(message)))
#define FANY_IPC_LOGF(...) ((void)sizeof(FanyIpcDiscardLogArgs(__VA_ARGS__)))

static HANDLE hMapFile = nullptr;
static void *pBuf;
static FanyImeSharedMemoryData *sharedData;

static bool canUseSharedMemory = true;
static bool canUseNamedPipe = true;

namespace
{
class PipeEndpoint
{
  public:
    explicit PipeEndpoint(HANDLE pipe) : pipe_(pipe)
    {
    }

    ~PipeEndpoint()
    {
        Shutdown();
        if (pipe_ && pipe_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(pipe_);
        }
    }

    PipeEndpoint(const PipeEndpoint &) = delete;
    PipeEndpoint &operator=(const PipeEndpoint &) = delete;

    HANDLE handle() const
    {
        return pipe_;
    }

    void Shutdown()
    {
        if (!shutting_down_.exchange(true) && pipe_ && pipe_ != INVALID_HANDLE_VALUE)
        {
            // Do not close here: a writer may still hold a shared reference. Disconnecting
            // wakes blocked pipe I/O, and the handle is closed exactly once by the destructor.
            DisconnectNamedPipe(pipe_);
        }
    }

    bool Write(const void *data, DWORD data_size, DWORD &bytes_written, DWORD &write_error)
    {
        std::lock_guard lock(write_mutex_);
        bytes_written = 0;
        if (shutting_down_.load())
        {
            write_error = ERROR_PIPE_NOT_CONNECTED;
            return false;
        }

        const BOOL result = WriteFile(pipe_, data, data_size, &bytes_written, nullptr);
        if (!result)
        {
            write_error = GetLastError();
            return false;
        }
        if (!FanyImeIpc::IsCompleteMessageFrameWrite(true, bytes_written, data_size))
        {
            write_error = bytes_written == 0 ? ERROR_NO_DATA : ERROR_WRITE_FAULT;
            return false;
        }
        write_error = ERROR_SUCCESS;
        return true;
    }

    HANDLE ReleaseHandle()
    {
        std::lock_guard lock(write_mutex_);
        shutting_down_.store(true);
        const HANDLE released = pipe_;
        pipe_ = INVALID_HANDLE_VALUE;
        return released;
    }

  private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::mutex write_mutex_;
    std::atomic_bool shutting_down_ = false;
};

class PipeControlHandle
{
  public:
    explicit PipeControlHandle(HANDLE pipe) : pipe_(pipe)
    {
    }

    ~PipeControlHandle()
    {
        Disconnect();
        if (pipe_ && pipe_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(pipe_);
        }
    }

    PipeControlHandle(const PipeControlHandle &) = delete;
    PipeControlHandle &operator=(const PipeControlHandle &) = delete;

    void Disconnect()
    {
        if (!disconnected_.exchange(true) && pipe_ && pipe_ != INVALID_HANDLE_VALUE)
        {
            DisconnectNamedPipe(pipe_);
        }
    }

  private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::atomic_bool disconnected_ = false;
};

struct PipeClientSession
{
    FanyImeProtocol::Negotiation protocol;
    HANDLE main_pipe = INVALID_HANDLE_VALUE;
    HANDLE main_control_pipe = INVALID_HANDLE_VALUE;
    uint64_t main_registration_id = 0;
    std::shared_ptr<PipeEndpoint> to_tsf_pipe;
    uint64_t to_tsf_registration_id = 0;
    bool to_tsf_ready = false;
    std::shared_ptr<PipeEndpoint> to_tsf_worker_thread_pipe;
    uint64_t to_tsf_worker_thread_registration_id = 0;
    bool to_tsf_worker_thread_ready = false;
    uint64_t focus_token = 0;
    uint64_t inactive_focus_token = 0;
    FanyImeIpc::OutboundSessionState outbound_state;
};

std::mutex g_pipe_clients_mutex;
std::condition_variable g_pipe_clients_cv;
std::unordered_map<uint64_t, PipeClientSession> g_pipe_clients;
FanyImeIpc::ActiveClientState g_active_client_state;
uint64_t g_next_pipe_registration_id = 0;

std::mutex g_pipe_handlers_mutex;
std::condition_variable g_pipe_handlers_cv;
std::unordered_map<uint64_t, std::shared_ptr<PipeControlHandle>> g_pipe_handlers;
uint64_t g_next_pipe_handler_id = 0;
size_t g_active_pipe_handler_count = 0;
bool g_pipe_handlers_shutting_down = false;

uint64_t NextPipeRegistrationIdLocked()
{
    ++g_next_pipe_registration_id;
    if (g_next_pipe_registration_id == 0)
    {
        ++g_next_pipe_registration_id;
    }
    return g_next_pipe_registration_id;
}

uint64_t NextPipeHandlerIdLocked()
{
    ++g_next_pipe_handler_id;
    if (g_next_pipe_handler_id == 0)
    {
        ++g_next_pipe_handler_id;
    }
    return g_next_pipe_handler_id;
}

void LogPipeWriteFailure(const wchar_t *pipe_name, UINT msg_type, DWORD write_error, DWORD bytes_written,
                         DWORD expected_bytes)
{
    FANY_IPC_LOGF(L"[msime]: [ipc] WriteFile failed on {}: gle={}, msg_type={}, bytes_written={}, expected_bytes={}",
                  pipe_name, write_error, msg_type, bytes_written, expected_bytes);
}

void LogPipeDisconnected(const wchar_t *pipe_name, UINT msg_type)
{
    FANY_IPC_LOGF(L"[msime]: [ipc] Attempted to write to disconnected pipe {}: msg_type={}", pipe_name, msg_type);
}

void LogPipeWriteTarget(const wchar_t *pipe_name, uint64_t client_id, UINT msg_type, const std::wstring &payload)
{
    FANY_IPC_LOGF(L"[msime]: [ipc] write target: pipe={}, client_id={}, msg_type={}, payload={}", pipe_name, client_id,
                  msg_type, payload);
}

// The account the server process runs as. It owns every pipe name the server publishes, and besides LocalSystem it is
// the only account the pipe DACL admits.
std::wstring GetProcessUserSidString()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return std::wstring();
    }

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::wstring sid;
    std::vector<BYTE> token_user(needed);
    if (needed != 0 && GetTokenInformation(token, TokenUser, token_user.data(), needed, &needed))
    {
        LPWSTR sid_text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(token_user.data())->User.Sid, &sid_text))
        {
            sid.assign(sid_text);
            LocalFree(sid_text);
        }
    }
    CloseHandle(token);
    return sid;
}

HANDLE CreateNamedPipeInstance(const wchar_t *pipe_name, DWORD out_buffer_size, DWORD in_buffer_size)
{
    // Names this process has already published. Only the very first instance of a name may carry
    // FILE_FLAG_FIRST_PIPE_INSTANCE, which is what turns a name some other process got to first into a loud
    // CreateNamedPipe failure instead of a silent second instance answering half of the client connections. A failed
    // creation is deliberately not recorded, so a squatted name keeps failing instead of eventually joining.
    static std::mutex owned_names_mutex;
    static std::vector<std::wstring> owned_names;

    const std::wstring sddl = FanyImeIpc::BuildPipeSecurityDescriptorSddl(GetProcessUserSidString());
    if (sddl.empty())
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }

    PSECURITY_DESCRIPTOR pd = nullptr;
    SECURITY_ATTRIBUTES sa = {};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &pd, NULL))
    {
        return INVALID_HANDLE_VALUE;
    }
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = pd;
    sa.bInheritHandle = FALSE;

    const std::lock_guard<std::mutex> owned_names_guard(owned_names_mutex);
    const bool owns_name = std::find(owned_names.begin(), owned_names.end(), pipe_name) != owned_names.end();

    HANDLE pipe = CreateNamedPipe(                                             //
        pipe_name,                                                             //
        PIPE_ACCESS_DUPLEX | (owns_name ? 0u : FILE_FLAG_FIRST_PIPE_INSTANCE), //
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, //
        out_buffer_size,          //
        in_buffer_size,           //
        0,                        //
        &sa                       //
    );

    if (pipe != INVALID_HANDLE_VALUE && !owns_name)
    {
        owned_names.emplace_back(pipe_name);
    }

    if (pd)
    {
        LocalFree(pd);
    }
    return pipe;
}

void ShutdownEndpoints(std::vector<std::shared_ptr<PipeEndpoint>> endpoints)
{
    for (const auto &endpoint : endpoints)
    {
        if (endpoint)
        {
            endpoint->Shutdown();
        }
    }
}

bool ConfigureReversePipeForBoundedWrites(HANDLE pipe)
{
    if (!pipe || pipe == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }

    // Reverse writes run synchronously while the routing mutex is held. The
    // server handle must therefore be nonblocking before it is published or
    // before the PipeReady ACK is attempted. For a message pipe with a full
    // buffer, PIPE_NOWAIT makes WriteFile return immediately with zero bytes.
    DWORD mode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
    return SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) != FALSE;
}

template <size_t N> bool CopyPipeText(wchar_t (&destination)[N], const std::wstring &source)
{
    if (source.size() >= N)
    {
        return false;
    }
    std::copy(source.begin(), source.end(), destination);
    destination[source.size()] = L'\0';
    return true;
}
} // namespace

int InitIpc()
{
    //
    // Shared memory
    //
    canUseSharedMemory = true;
    pBuf = nullptr;
    sharedData = nullptr;

    SetLastError(ERROR_SUCCESS);
    hMapFile = CreateFileMappingW(       //
        INVALID_HANDLE_VALUE,            //
        nullptr,                         //
        PAGE_READWRITE,                  //
        0,                               //
        static_cast<DWORD>(BUFFER_SIZE), //
        FANY_IME_SHARED_MEMORY           //
    );                                   //

    const bool alreadyExists = hMapFile && GetLastError() == ERROR_ALREADY_EXISTS;
    if (hMapFile)
    {
        pBuf = MapViewOfFile(    //
            hMapFile,            //
            FILE_MAP_ALL_ACCESS, //
            0,                   //
            0,                   //
            BUFFER_SIZE          //
        );                       //
    }

    if (!hMapFile || !pBuf)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        canUseSharedMemory = false;
        pBuf = nullptr;
        sharedData = nullptr;
        if (hMapFile)
        {
            CloseHandle(hMapFile);
            hMapFile = nullptr;
        }
    }
    else
    {
        sharedData = static_cast<FanyImeSharedMemoryData *>(pBuf);
        // Only initialize the shared memory when first created.
        if (!alreadyExists)
        {
            *sharedData = {};
            sharedData->point[0] = 100;
            sharedData->point[1] = 100;
        }
    }

    //
    // Events, create here
    //
    for (size_t i = 0; i < FANY_IME_EVENT_ARRAY.size(); ++i)
    {
        hEvents[i] = CreateEventW(          //
            nullptr,                        //
            FALSE,                          //
            FALSE,                          // Auto reset
            FANY_IME_EVENT_ARRAY[i].c_str() //
        );                                  //
        if (!hEvents[i])
        {
// Error handling
#ifdef FANY_DEBUG
            (void)0;
#endif
        }
    }

    return 0;
}

HANDLE CreateMainNamedPipeInstance()
{
    return CreateNamedPipeInstance(FANY_IME_NAMED_PIPE, BUFFER_SIZE, BUFFER_SIZE);
}

HANDLE CreateAuxNamedPipeInstance()
{
    return CreateNamedPipeInstance(FANY_IME_AUX_NAMED_PIPE, 128, 128);
}

HANDLE CreateTsfDiagnosticNamedPipeInstance()
{
    return CreateNamedPipeInstance(FANY_IME_TSF_DIAGNOSTIC_NAMED_PIPE, 128,
                                   static_cast<DWORD>(FANY_IME_TSF_DIAGNOSTIC_MAX_FRAME_BYTES));
}

HANDLE CreateVoiceControlNamedPipeInstance()
{
    return CreateNamedPipeInstance(FANY_IME_VOICE_CONTROL_NAMED_PIPE, 512, 512);
}

HANDLE CreateToTsfNamedPipeInstance()
{
    return CreateNamedPipeInstance(
        FANY_IME_TO_TSF_NAMED_PIPE,
        static_cast<DWORD>(sizeof(FanyImeNamedpipeDataToTsf) * FANY_IME_TO_TSF_PIPE_FRAME_CAPACITY),
        static_cast<DWORD>(sizeof(FanyImePipeHello)));
}

HANDLE CreateToTsfWorkerThreadNamedPipeInstance()
{
    return CreateNamedPipeInstance(
        FANY_IME_TO_TSF_WORKER_THREAD_NAMED_PIPE,
        static_cast<DWORD>(sizeof(FanyImeNamedpipeDataToTsfWorkerThread) * FANY_IME_TO_TSF_WORKER_PIPE_FRAME_CAPACITY),
        static_cast<DWORD>(sizeof(FanyImePipeHello)));
}

//
// We also create events for thread-communication here
//
int InitNamedPipe()
{
    //
    // Named Pipe
    //

    // Nmaedpipe for IME communication
    hPipe = CreateMainNamedPipeInstance();

    // Nmaedpipe for reconnecting main pipe
    hAuxPipe = CreateAuxNamedPipeInstance();

    // Short-lived, batch-only TSF diagnostic transport. It is independent of
    // Aux so logging can never delay lifecycle or settings notifications.
    hTsfDiagnosticPipe = CreateTsfDiagnosticNamedPipeInstance();
    hVoiceControlPipe = CreateVoiceControlNamedPipeInstance();

    // Namedpipe for passing data from this process to TSF process
    hToTsfPipe = CreateToTsfNamedPipeInstance();

    // Namedpipe for sending msg to TSF side
    hToTsfWorkerThreadPipe = CreateToTsfWorkerThreadNamedPipeInstance();

    if (hPipe == INVALID_HANDLE_VALUE)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        FANY_IPC_LOGF(L"[msime]: [ipc] CreateNamedPipe failed for main pipe: gle={}", GetLastError());
    }
    else
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        FANY_IPC_LOG_RAW(L"[msime]: [ipc] Main named pipe created");
    }

    if (hAuxPipe == INVALID_HANDLE_VALUE)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        FANY_IPC_LOGF(L"[msime]: [ipc] CreateNamedPipe failed for aux pipe: gle={}", GetLastError());
    }
    else
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        FANY_IPC_LOG_RAW(L"[msime]: [ipc] Aux named pipe created");
    }

    if (hToTsfPipe == INVALID_HANDLE_VALUE)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        FANY_IPC_LOGF(L"[msime]: [ipc] CreateNamedPipe failed for to-tsf pipe: gle={}", GetLastError());
    }
    else
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        FANY_IPC_LOG_RAW(L"[msime]: [ipc] To-tsf named pipe created");
    }

    if (hToTsfWorkerThreadPipe == INVALID_HANDLE_VALUE)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        FANY_IPC_LOGF(L"[msime]: [ipc] CreateNamedPipe failed for to-tsf-worker pipe: gle={}", GetLastError());
    }
    else
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        FANY_IPC_LOG_RAW(L"[msime]: [ipc] To-tsf-worker named pipe created");
    }

    //
    // Events, create here
    //
    for (size_t i = 0; i < FANY_IME_EVENT_PIPE_ARRAY.size(); ++i)
    {
        hPipeEvents[i] = CreateEventW(           //
            nullptr,                             //
            FALSE,                               //
            FALSE,                               // Auto reset
            FANY_IME_EVENT_PIPE_ARRAY[i].c_str() //
        );                                       //
        if (!hPipeEvents[i])
        {
// Error handling
#ifdef FANY_DEBUG
            (void)0;
#endif
        }
    }

    for (size_t i = 0; i < FANY_IME_EVENT_PIPE_TO_TSF_WORKER_THREAD_ARRAY.size(); ++i)
    {
        hWorkerPipeEvents[i] = CreateEventW(                          //
            nullptr,                                                  //
            FALSE,                                                    //
            FALSE,                                                    // Auto reset
            FANY_IME_EVENT_PIPE_TO_TSF_WORKER_THREAD_ARRAY[i].c_str() //
        );                                                            //
        if (!hWorkerPipeEvents[i])
        {
// Error handling
#ifdef FANY_DEBUG
            (void)0;
#endif
        }
    }

    return 0;
}

int CloseIpc()
{
    //
    // Shared memory
    //
    if (pBuf)
    {
        UnmapViewOfFile(pBuf);
        pBuf = nullptr;
    }
    sharedData = nullptr;
    canUseSharedMemory = false;

    if (hMapFile)
    {
        CloseHandle(hMapFile);
        hMapFile = nullptr;
    }

    //
    // Events
    //
    const auto closeEvents = [](std::vector<HANDLE> &events) {
        for (HANDLE &eventHandle : events)
        {
            if (eventHandle && eventHandle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(eventHandle);
            }
            eventHandle = nullptr;
        }
    };
    closeEvents(hEvents);
    closeEvents(hPipeEvents);
    closeEvents(hWorkerPipeEvents);

    return 0;
}

/**
 * @brief Close named pipe
 *
 * @return int
 */
int CloseNamedPipe()
{
    if (hPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hPipe);
        hPipe = INVALID_HANDLE_VALUE;
    }
    if (hVoiceControlPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hVoiceControlPipe);
        hVoiceControlPipe = INVALID_HANDLE_VALUE;
    }
    return 0;
}

int OpenToTsfNamedPipe()
{
    hToTsfPipe = CreateToTsfNamedPipeInstance();
    return 0;
}

int CloseToTsfNamedPipe()
{
    if (hToTsfPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hToTsfPipe);
        hToTsfPipe = INVALID_HANDLE_VALUE;
    }
    return 0;
}

int CloseAuxNamedPipe()
{
    if (hAuxPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hAuxPipe);
        hAuxPipe = INVALID_HANDLE_VALUE;
    }
    return 0;
}

int OpenToTsfWorkerThreadNamedPipe()
{
    hToTsfWorkerThreadPipe = CreateToTsfWorkerThreadNamedPipeInstance();
    return 0;
}

int CloseToTsfWorkerThreadNamedPipe()
{
    if (hToTsfWorkerThreadPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hToTsfWorkerThreadPipe);
        hToTsfWorkerThreadPipe = INVALID_HANDLE_VALUE;
    }
    return 0;
}

uint64_t BeginPipeClientHandler(HANDLE pipe)
{
    if (!pipe || pipe == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    HANDLE controlPipe = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), pipe, GetCurrentProcess(), &controlPipe, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        return 0;
    }

    auto control = std::make_shared<PipeControlHandle>(controlPipe);
    std::lock_guard lock(g_pipe_handlers_mutex);
    if (g_pipe_handlers_shutting_down)
    {
        return 0;
    }

    const uint64_t handlerId = NextPipeHandlerIdLocked();
    g_pipe_handlers.emplace(handlerId, std::move(control));
    ++g_active_pipe_handler_count;
    return handlerId;
}

void EndPipeClientHandler(uint64_t handler_id)
{
    if (handler_id == 0)
    {
        return;
    }

    std::shared_ptr<PipeControlHandle> control;
    {
        std::lock_guard lock(g_pipe_handlers_mutex);
        auto it = g_pipe_handlers.find(handler_id);
        if (it == g_pipe_handlers.end())
        {
            return;
        }
        control = std::move(it->second);
        g_pipe_handlers.erase(it);
        if (g_active_pipe_handler_count != 0)
        {
            --g_active_pipe_handler_count;
        }
    }
    g_pipe_handlers_cv.notify_all();
}

uint64_t RegisterMainPipeClient(uint64_t client_id, HANDLE pipe)
{
    if (client_id == 0 || !pipe || pipe == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    HANDLE control_pipe = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), pipe, GetCurrentProcess(), &control_pipe, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
    {
        return 0;
    }

    HANDLE replaced_control_pipe = INVALID_HANDLE_VALUE;
    uint64_t registration_id = 0;
    uint64_t invalidation_epoch = 0;
    {
        std::lock_guard lock(g_pipe_clients_mutex);
        auto &session = g_pipe_clients[client_id];
        replaced_control_pipe = session.main_control_pipe;
        if (g_active_client_state.is_current(client_id))
        {
            // A new main connection is a new logical session even when the
            // PID/TID-derived client id is unchanged. Advancing the epoch here
            // invalidates queued work and late replies from the old reader.
            session.inactive_focus_token = session.focus_token;
            invalidation_epoch = g_active_client_state.deactivate(client_id);
        }
        session.focus_token = 0;
        session.main_pipe = pipe;
        session.main_control_pipe = control_pipe;
        control_pipe = INVALID_HANDLE_VALUE;
        session.main_registration_id = NextPipeRegistrationIdLocked();
        registration_id = session.main_registration_id;
    }

    if (control_pipe && control_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(control_pipe);
    }
    if (replaced_control_pipe && replaced_control_pipe != INVALID_HANDLE_VALUE)
    {
        DisconnectNamedPipe(replaced_control_pipe);
        CloseHandle(replaced_control_pipe);
    }
    FanyNamedPipe::EnqueuePipeSessionInvalidatedTask(client_id, invalidation_epoch);
    g_pipe_clients_cv.notify_all();
    return registration_id;
}

namespace
{
bool SharedMemoryAvailable()
{
    return canUseSharedMemory && sharedData != nullptr && pBuf != nullptr && hMapFile != nullptr;
}
} // namespace

bool NegotiateMainPipeClient(const FanyImeNamedpipeData &hello, uint64_t registration_id)
{
    const auto protocol =
        FanyImeProtocol::Negotiate(hello, FanyImeProtocol::Capabilities | FanyImeProtocol::CharacterSetShortcut);
    std::lock_guard lock(g_pipe_clients_mutex);
    auto it = g_pipe_clients.find(hello.client_id);
    if (it == g_pipe_clients.end() || registration_id == 0 || it->second.main_registration_id != registration_id)
        return false;
    auto &session = it->second;
    session.protocol = protocol;
    if (protocol.legacy)
        return protocol.accepted;
    if (!session.to_tsf_pipe || !session.to_tsf_ready)
        return false;
    const auto reply = FanyImeProtocol::Reply(hello, protocol);
    DWORD written = 0;
    DWORD error = ERROR_SUCCESS;
    // Tie the ACK to this main registration while the registry lock excludes
    // endpoint replacement. No activation or candidate result can precede it.
    return session.to_tsf_pipe->Write(&reply, sizeof(reply), written, error) && written == sizeof(reply) &&
           protocol.accepted;
}

uint64_t RegisterToTsfPipeClient(uint64_t client_id, HANDLE pipe)
{
    if (client_id == 0 || !pipe || pipe == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    if (!ConfigureReversePipeForBoundedWrites(pipe))
    {
        FANY_IPC_LOGF(L"[msime]: [ipc] failed to configure to-tsf PIPE_NOWAIT: client_id={}, gle={}", client_id,
                      GetLastError());
        return 0;
    }

    auto newEndpoint = std::make_shared<PipeEndpoint>(pipe);
    std::shared_ptr<PipeEndpoint> replacedEndpoint;
    std::shared_ptr<PipeEndpoint> failedEndpoint;
    uint64_t registration_id = 0;
    DWORD bytesWritten = 0;
    DWORD writeError = ERROR_SUCCESS;
    {
        std::lock_guard lock(g_pipe_clients_mutex);
        auto &session = g_pipe_clients[client_id];
        replacedEndpoint = std::move(session.to_tsf_pipe);
        const uint64_t replacedRegistrationId = session.to_tsf_registration_id;
        const bool replacedReady = session.to_tsf_ready;

        session.to_tsf_pipe = newEndpoint;
        session.to_tsf_registration_id = NextPipeRegistrationIdLocked();
        session.to_tsf_ready = false;

        // The ready ACK is written while the registry lock is held, so every
        // other reply sender is excluded and this is necessarily the first
        // frame on the newly published reverse endpoint.
        FanyImeNamedpipeDataToTsf readyPacket = {};
        readyPacket.msg_type = Global::DataFromServerMsgType::PipeReady;
        if (newEndpoint->Write(&readyPacket, sizeof(readyPacket), bytesWritten, writeError))
        {
            session.to_tsf_ready = true;
            session.outbound_state.mark_recovered(FanyImeIpc::OutboundRoute::Reply);
            registration_id = session.to_tsf_registration_id;
        }
        else
        {
            failedEndpoint = std::move(session.to_tsf_pipe);
            session.to_tsf_pipe = std::move(replacedEndpoint);
            session.to_tsf_registration_id = replacedRegistrationId;
            session.to_tsf_ready = replacedReady;
            if (session.main_pipe == INVALID_HANDLE_VALUE && !session.to_tsf_pipe && !session.to_tsf_worker_thread_pipe)
            {
                g_pipe_clients.erase(client_id);
            }
        }
    }

    if (failedEndpoint)
    {
        LogPipeWriteFailure(L"to-tsf-ready", Global::DataFromServerMsgType::PipeReady, writeError, bytesWritten,
                            sizeof(FanyImeNamedpipeDataToTsf));
        // Restore ownership to the handler so the zero-return path retains the
        // same close contract as every other registration failure.
        (void)failedEndpoint->ReleaseHandle();
        return 0;
    }
    if (replacedEndpoint)
    {
        replacedEndpoint->Shutdown();
    }
    g_pipe_clients_cv.notify_all();
    return registration_id;
}

uint64_t RegisterToTsfWorkerThreadPipeClient(uint64_t client_id, HANDLE pipe)
{
    if (client_id == 0 || !pipe || pipe == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    if (!ConfigureReversePipeForBoundedWrites(pipe))
    {
        FANY_IPC_LOGF(L"[msime]: [ipc] failed to configure to-tsf-worker PIPE_NOWAIT: client_id={}, gle={}", client_id,
                      GetLastError());
        return 0;
    }

    auto new_endpoint = std::make_shared<PipeEndpoint>(pipe);
    std::shared_ptr<PipeEndpoint> replaced_endpoint;
    std::shared_ptr<PipeEndpoint> failed_endpoint;
    uint64_t registration_id = 0;
    DWORD bytes_written = 0;
    DWORD write_error = ERROR_SUCCESS;
    {
        std::lock_guard lock(g_pipe_clients_mutex);
        auto &session = g_pipe_clients[client_id];
        replaced_endpoint = std::move(session.to_tsf_worker_thread_pipe);
        const uint64_t replaced_registration_id = session.to_tsf_worker_thread_registration_id;
        const bool replaced_ready = session.to_tsf_worker_thread_ready;

        session.to_tsf_worker_thread_pipe = new_endpoint;
        session.to_tsf_worker_thread_registration_id = NextPipeRegistrationIdLocked();
        session.to_tsf_worker_thread_ready = false;

        // The ready ACK is written under the registry lock, so it is the first
        // frame and no status/focus/candidate writer can race ahead of it.
        FanyImeNamedpipeDataToTsfWorkerThread ready_packet = {};
        ready_packet.msg_type = Global::DataFromServerMsgTypeToTsfWorkerThread::PipeReady;
        if (new_endpoint->Write(&ready_packet, sizeof(ready_packet), bytes_written, write_error))
        {
            session.to_tsf_worker_thread_ready = true;
            session.outbound_state.mark_recovered(FanyImeIpc::OutboundRoute::Worker);
            registration_id = session.to_tsf_worker_thread_registration_id;
        }
        else
        {
            failed_endpoint = std::move(session.to_tsf_worker_thread_pipe);
            session.to_tsf_worker_thread_pipe = std::move(replaced_endpoint);
            session.to_tsf_worker_thread_registration_id = replaced_registration_id;
            session.to_tsf_worker_thread_ready = replaced_ready;
            if (session.main_pipe == INVALID_HANDLE_VALUE && !session.to_tsf_pipe && !session.to_tsf_worker_thread_pipe)
            {
                g_pipe_clients.erase(client_id);
            }
        }
    }
    if (failed_endpoint)
    {
        LogPipeWriteFailure(L"to-tsf-worker-ready", Global::DataFromServerMsgTypeToTsfWorkerThread::PipeReady,
                            write_error, bytes_written, sizeof(FanyImeNamedpipeDataToTsfWorkerThread));
        // Preserve the registration_id == 0 close contract for the handler.
        (void)failed_endpoint->ReleaseHandle();
        return 0;
    }
    if (replaced_endpoint)
    {
        replaced_endpoint->Shutdown();
    }
    g_pipe_clients_cv.notify_all();
    return registration_id;
}

PipeClientUnregisterResult UnregisterPipeClientHandle(uint64_t client_id, UINT pipe_role, HANDLE pipe,
                                                      uint64_t registration_id)
{
    PipeClientUnregisterResult result;
    if (client_id == 0 || registration_id == 0)
    {
        return result;
    }

    std::vector<std::shared_ptr<PipeEndpoint>> endpoints_to_shutdown;
    HANDLE main_control_pipe_to_close = INVALID_HANDLE_VALUE;
    {
        std::lock_guard lock(g_pipe_clients_mutex);
        auto it = g_pipe_clients.find(client_id);
        if (it == g_pipe_clients.end())
        {
            return result;
        }

        if (pipe_role == FanyImePipeRole::Main && it->second.main_pipe == pipe &&
            it->second.main_registration_id == registration_id)
        {
            result.removed = true;
            main_control_pipe_to_close = it->second.main_control_pipe;
            it->second.main_pipe = INVALID_HANDLE_VALUE;
            it->second.main_control_pipe = INVALID_HANDLE_VALUE;
            it->second.main_registration_id = 0;
            if (g_active_client_state.is_current(client_id))
            {
                it->second.inactive_focus_token = it->second.focus_token;
                result.deactivation_epoch = g_active_client_state.deactivate(client_id);
            }
            it->second.focus_token = 0;
        }
        else if (pipe_role == FanyImePipeRole::ToTsf && it->second.to_tsf_pipe &&
                 it->second.to_tsf_pipe->handle() == pipe && it->second.to_tsf_registration_id == registration_id)
        {
            result.removed = true;
            endpoints_to_shutdown.push_back(std::move(it->second.to_tsf_pipe));
            it->second.to_tsf_registration_id = 0;
            it->second.to_tsf_ready = false;
            it->second.outbound_state.mark_failed(FanyImeIpc::OutboundRoute::Reply);
            if (g_active_client_state.is_current(client_id))
            {
                it->second.inactive_focus_token = it->second.focus_token;
                result.deactivation_epoch = g_active_client_state.deactivate(client_id);
                it->second.focus_token = 0;
            }
        }
        else if (pipe_role == FanyImePipeRole::ToTsfWorkerThread && it->second.to_tsf_worker_thread_pipe &&
                 it->second.to_tsf_worker_thread_pipe->handle() == pipe &&
                 it->second.to_tsf_worker_thread_registration_id == registration_id)
        {
            result.removed = true;
            endpoints_to_shutdown.push_back(std::move(it->second.to_tsf_worker_thread_pipe));
            it->second.to_tsf_worker_thread_registration_id = 0;
            it->second.to_tsf_worker_thread_ready = false;
            it->second.outbound_state.mark_failed(FanyImeIpc::OutboundRoute::Worker);
            if (g_active_client_state.is_current(client_id))
            {
                it->second.inactive_focus_token = it->second.focus_token;
                result.deactivation_epoch = g_active_client_state.deactivate(client_id);
                it->second.focus_token = 0;
            }
        }

        if (it->second.main_pipe == INVALID_HANDLE_VALUE && !it->second.to_tsf_pipe &&
            !it->second.to_tsf_worker_thread_pipe)
        {
            g_pipe_clients.erase(it);
        }
    }

    if (result.removed)
    {
        g_pipe_clients_cv.notify_all();
    }
    if (main_control_pipe_to_close && main_control_pipe_to_close != INVALID_HANDLE_VALUE)
    {
        CloseHandle(main_control_pipe_to_close);
    }
    ShutdownEndpoints(std::move(endpoints_to_shutdown));
    return result;
}

bool IsPipeClientRegistrationCurrent(uint64_t client_id, UINT pipe_role, uint64_t registration_id)
{
    std::lock_guard lock(g_pipe_clients_mutex);
    auto it = g_pipe_clients.find(client_id);
    if (client_id == 0 || registration_id == 0 || it == g_pipe_clients.end())
    {
        return false;
    }
    if (pipe_role == FanyImePipeRole::Main)
    {
        return it->second.main_registration_id == registration_id;
    }
    if (pipe_role == FanyImePipeRole::ToTsf)
    {
        return it->second.to_tsf_registration_id == registration_id;
    }
    if (pipe_role == FanyImePipeRole::ToTsfWorkerThread)
    {
        return it->second.to_tsf_worker_thread_registration_id == registration_id;
    }
    return false;
}

PipeClientActivation ActivatePipeClient(uint64_t client_id, uint64_t main_registration_id, bool wait_for_reverse_pipe,
                                        uint64_t focus_token, bool update_focus_token)
{
    std::unique_lock lock(g_pipe_clients_mutex);
    const auto routeReady = [&] {
        const auto it = g_pipe_clients.find(client_id);
        return client_id != 0 && main_registration_id != 0 && it != g_pipe_clients.end() &&
               it->second.main_pipe != INVALID_HANDLE_VALUE &&
               it->second.main_registration_id == main_registration_id && it->second.to_tsf_pipe &&
               it->second.to_tsf_registration_id != 0 && it->second.to_tsf_ready &&
               it->second.to_tsf_worker_thread_pipe && it->second.to_tsf_worker_thread_registration_id != 0 &&
               it->second.to_tsf_worker_thread_ready && !it->second.outbound_state.is_dirty();
    };

    if (wait_for_reverse_pipe && !routeReady())
    {
        g_pipe_clients_cv.wait_for(lock, std::chrono::milliseconds(100), routeReady);
    }
    if (!routeReady())
    {
        return {};
    }

    auto &session = g_pipe_clients.find(client_id)->second;
    if ((update_focus_token && focus_token == 0) || (!update_focus_token && session.focus_token == 0))
    {
        // Explicit activation must introduce a nonzero focus token, and an
        // implicit key fallback may only reuse a token from an already fenced
        // session. Never create an active epoch that cannot be acknowledged
        // on the worker pipe.
        return {};
    }
    const FanyImeIpc::ActiveClientTransition previous = g_active_client_state.snapshot();
    // Do not clear the displaced client's already-acknowledged focus token.
    // TextInputHost.exe temporarily becomes the active TSF client for Win+.,
    // but Windows does not reliably send the original application another
    // OnSetFocus/OnSetThreadFocus callback when the panel closes. Its first
    // real KeyEvent is therefore the only foreground signal available and
    // must be allowed to reuse that previously fenced token for implicit
    // activation. Explicit suspension/deactivation and pipe invalidation still
    // clear the owning session's token in their respective paths.
    const bool new_focus_session = update_focus_token && session.focus_token != focus_token;
    if (update_focus_token)
    {
        session.focus_token = focus_token;
    }
    const FanyImeIpc::ActiveClientTransition transition = new_focus_session && previous.client_id == client_id
                                                              ? g_active_client_state.renew(client_id)
                                                              : g_active_client_state.activate(client_id);
    if (transition.client_id != 0)
    {
        session.inactive_focus_token = 0;
    }
    return {transition.client_id, transition.epoch, transition.changed, session.focus_token};
}

uint64_t DeactivatePipeClient(uint64_t client_id, uint64_t main_registration_id)
{
    std::lock_guard lock(g_pipe_clients_mutex);
    auto it = g_pipe_clients.find(client_id);
    if (client_id == 0 || main_registration_id == 0 || it == g_pipe_clients.end() ||
        it->second.main_registration_id != main_registration_id)
    {
        return 0;
    }
    const uint64_t epoch = g_active_client_state.deactivate(client_id);
    if (epoch != 0)
    {
        it->second.inactive_focus_token = it->second.focus_token;
        it->second.focus_token = 0;
    }
    return epoch;
}

uint64_t DeactivatePipeClientByFocusToken(uint64_t client_id, uint64_t focus_token)
{
    std::lock_guard lock(g_pipe_clients_mutex);
    const auto it = g_pipe_clients.find(client_id);
    if (it == g_pipe_clients.end())
    {
        return 0;
    }

    const FanyImeIpc::ActiveClientTransition active = g_active_client_state.snapshot();
    const uint64_t inactive_epoch = g_active_client_state.terminal_deactivation_epoch(client_id);
    if (!FanyImeIpc::CanApplyTerminalDeactivationFallback(client_id, focus_token, active.client_id,
                                                          it->second.focus_token, inactive_epoch != 0,
                                                          it->second.inactive_focus_token))
    {
        return 0;
    }

    if (active.client_id == client_id)
    {
        const uint64_t epoch = g_active_client_state.deactivate(client_id);
        if (epoch != 0)
        {
            it->second.inactive_focus_token = it->second.focus_token;
            it->second.focus_token = 0;
        }
        return epoch;
    }
    return inactive_epoch;
}

uint64_t ResolvePipeClientTerminalDeactivationEpoch(uint64_t client_id, uint64_t transition_epoch)
{
    std::lock_guard lock(g_pipe_clients_mutex);
    return g_active_client_state.terminal_deactivation_epoch(client_id, transition_epoch);
}

PipeClientActivation GetActivePipeClient()
{
    std::lock_guard lock(g_pipe_clients_mutex);
    const FanyImeIpc::ActiveClientTransition snapshot = g_active_client_state.snapshot();
    uint64_t focus_token = 0;
    const auto session = g_pipe_clients.find(snapshot.client_id);
    if (session != g_pipe_clients.end())
    {
        focus_token = session->second.focus_token;
    }
    return {snapshot.client_id, snapshot.epoch, false, focus_token};
}

bool IsActivePipeClient(uint64_t client_id, uint64_t activation_epoch)
{
    std::lock_guard lock(g_pipe_clients_mutex);
    return g_active_client_state.is_current(client_id, activation_epoch);
}

bool IsPipeActivationCurrent(uint64_t client_id, uint64_t activation_epoch)
{
    std::lock_guard lock(g_pipe_clients_mutex);
    return g_active_client_state.matches(client_id, activation_epoch);
}

void ShutdownPipeClients()
{
    std::vector<std::shared_ptr<PipeControlHandle>> handler_controls;
    {
        std::lock_guard lock(g_pipe_handlers_mutex);
        g_pipe_handlers_shutting_down = true;
        handler_controls.reserve(g_pipe_handlers.size());
        for (const auto &[handler_id, control] : g_pipe_handlers)
        {
            (void)handler_id;
            handler_controls.push_back(control);
        }
    }

    std::vector<std::shared_ptr<PipeEndpoint>> endpoints_to_shutdown;
    std::vector<HANDLE> main_control_pipes_to_disconnect;
    {
        std::lock_guard lock(g_pipe_clients_mutex);
        for (auto &[client_id, session] : g_pipe_clients)
        {
            (void)client_id;
            if (session.main_control_pipe && session.main_control_pipe != INVALID_HANDLE_VALUE)
            {
                main_control_pipes_to_disconnect.push_back(session.main_control_pipe);
                session.main_control_pipe = INVALID_HANDLE_VALUE;
            }
            if (session.to_tsf_pipe)
            {
                endpoints_to_shutdown.push_back(std::move(session.to_tsf_pipe));
            }
            if (session.to_tsf_worker_thread_pipe)
            {
                endpoints_to_shutdown.push_back(std::move(session.to_tsf_worker_thread_pipe));
            }
        }
        g_pipe_clients.clear();
        g_active_client_state.deactivate_current();
    }
    g_pipe_clients_cv.notify_all();

    ShutdownEndpoints(std::move(endpoints_to_shutdown));
    for (HANDLE main_control_pipe : main_control_pipes_to_disconnect)
    {
        DisconnectNamedPipe(main_control_pipe);
        CloseHandle(main_control_pipe);
    }

    for (const auto &control : handler_controls)
    {
        if (control)
        {
            control->Disconnect();
        }
    }

    std::unique_lock handlerLock(g_pipe_handlers_mutex);
    const bool handlersStopped = g_pipe_handlers_cv.wait_for(handlerLock, std::chrono::seconds(3),
                                                             [] { return g_active_pipe_handler_count == 0; });
    if (!handlersStopped)
    {
        FANY_IPC_LOGF(L"[msime]: [ipc] timed out waiting for {} pipe client handlers", g_active_pipe_handler_count);
    }
}

int WriteDataToSharedMemory(              //
    const std::wstring &candidate_string, //
    bool write_flag                       //
)
{
    if (!write_flag)
    {
        return 0;
    }

    // The candidate window is in this process, so retain a local copy even if
    // the optional cross-process mapping could not be created.
    Global::CandidateString = candidate_string;
    if (!SharedMemoryAvailable())
    {
        return 0;
    }
    if (candidate_string.size() >= std::size(sharedData->candidate_string))
    {
        return -1;
    }
    wcscpy_s(sharedData->candidate_string, candidate_string.c_str());
    return 0;
}

int ReadDataFromSharedMemory(UINT read_flag)
{
    if (!SharedMemoryAvailable())
    {
        return 0;
    }
    if (read_flag >> 0 & 1u)
    {
        Global::Keycode = sharedData->keycode;
    }

    if (read_flag >> 1 & 1u)
    {
        Global::Wch = sharedData->wch;
    }

    if (read_flag >> 2 & 1u)
    {
        Global::ModifiersDown = sharedData->modifiers_down;
    }

    if (read_flag >> 3 & 1u)
    {
        Global::Point[0] = sharedData->point[0];
        Global::Point[1] = sharedData->point[1];
    }

    if (read_flag >> 4 & 1u)
    {
        Global::PinyinLength = sharedData->pinyin_length;
    }

    if (read_flag >> 5 & 1u)
    {
        Global::PinyinString = sharedData->pinyin_string;
    }

    if (read_flag >> 6 & 1u)
    {
        Global::CandidateString = sharedData->candidate_string;
    }

    return 0;
}

int ReadDataFromNamedPipe(UINT read_flag)
{
    if (read_flag >> 0 & 1u)
    {
        Global::Keycode = namedpipeData.keycode;
    }

    if (read_flag >> 1 & 1u)
    {
        Global::Wch = namedpipeData.wch;
    }

    if (read_flag >> 2 & 1u)
    {
        Global::ModifiersDown = namedpipeData.modifiers_down & ~FanyImePipeFlags::UiLess;
    }

    if (read_flag >> 3 & 1u)
    {
        Global::Point[0] = namedpipeData.point[0];
        Global::Point[1] = namedpipeData.point[1];
    }

    if (read_flag >> 4 & 1u)
    {
        Global::PinyinLength = namedpipeData.pinyin_length;
    }

    if (read_flag >> 5 & 1u)
    {
        const int length =
            std::clamp(namedpipeData.pinyin_length, 0, static_cast<int>(std::size(namedpipeData.pinyin_string)) - 1);
        Global::PinyinString.assign(namedpipeData.pinyin_string, static_cast<size_t>(length));
    }

    return 0;
}

bool SendToTsfClientViaNamedpipe(uint64_t client_id, uint64_t activation_epoch, UINT msg_type, uint64_t request_id,
                                 const std::wstring &pipeData)
{
    FanyImeNamedpipeDataToTsf packet = {};
    packet.msg_type = msg_type;
    packet.request_id = request_id;
    if (!CopyPipeText(packet.candidate_string, pipeData))
    {
        FANY_IPC_LOGF(L"[msime]: [ipc] to-tsf payload too long: client_id={}, msg_type={}, chars={}", client_id,
                      msg_type, pipeData.size());
        return false;
    }
    LogPipeWriteTarget(L"to-tsf", client_id, msg_type, pipeData);

    std::shared_ptr<PipeEndpoint> endpoint;
    DWORD bytes_written = 0;
    DWORD write_error = ERROR_SUCCESS;
    bool write_succeeded = false;
    bool failed_endpoint_removed = false;
    uint64_t invalidation_epoch = 0;
    {
        // Keep routing validation and the bounded NOWAIT write atomic with
        // respect to active-client and endpoint replacement.
        std::lock_guard lock(g_pipe_clients_mutex);
        auto it = g_pipe_clients.find(client_id);
        if (g_active_client_state.matches(client_id, activation_epoch) && it != g_pipe_clients.end() &&
            it->second.to_tsf_pipe && it->second.to_tsf_registration_id != 0 && it->second.to_tsf_ready)
        {
            endpoint = it->second.to_tsf_pipe;
            write_succeeded = endpoint->Write(&packet, sizeof(packet), bytes_written, write_error);
            if (!write_succeeded)
            {
                // Remove the failed route and invalidate the exact epoch in
                // the same critical section as validation/write. A delayed
                // failure can therefore never deactivate a newer same-client
                // activation.
                it->second.to_tsf_pipe.reset();
                it->second.to_tsf_registration_id = 0;
                it->second.to_tsf_ready = false;
                it->second.outbound_state.mark_failed(FanyImeIpc::OutboundRoute::Reply);
                invalidation_epoch = g_active_client_state.invalidate(client_id, activation_epoch);
                if (invalidation_epoch != 0)
                {
                    it->second.inactive_focus_token = it->second.focus_token;
                    it->second.focus_token = 0;
                }
                failed_endpoint_removed = true;
            }
        }
    }

    if (!endpoint)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        LogPipeDisconnected(L"to-tsf", msg_type);
        return false;
    }
    if (!write_succeeded)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        LogPipeWriteFailure(L"to-tsf", msg_type, write_error, bytes_written, sizeof(packet));
        if (failed_endpoint_removed)
        {
            endpoint->Shutdown();
            g_pipe_clients_cv.notify_all();
            FanyNamedPipe::EnqueuePipeSessionInvalidatedTask(client_id, invalidation_epoch);
        }
        return false;
    }
    return true;
}

namespace
{
bool SendWorkerPackets(uint64_t client_id, uint64_t activation_epoch, bool require_active,
                       const std::vector<FanyImeNamedpipeDataToTsfWorkerThread> &packets)
{
    if (packets.empty())
    {
        return true;
    }

    std::shared_ptr<PipeEndpoint> endpoint;
    DWORD bytes_written = 0;
    DWORD write_error = ERROR_SUCCESS;
    bool write_succeeded = false;
    bool failed_endpoint_removed = false;
    uint64_t invalidation_epoch = 0;
    UINT failed_msg_type = packets.front().msg_type;
    {
        std::lock_guard lock(g_pipe_clients_mutex);
        auto it = g_pipe_clients.find(client_id);
        const bool route_is_current = !require_active || g_active_client_state.matches(client_id, activation_epoch);
        if (client_id != 0 && route_is_current && it != g_pipe_clients.end() && it->second.to_tsf_worker_thread_pipe &&
            it->second.to_tsf_worker_thread_registration_id != 0 && it->second.to_tsf_worker_thread_ready)
        {
            endpoint = it->second.to_tsf_worker_thread_pipe;
            write_succeeded = true;
            for (const auto &packet : packets)
            {
                if (!endpoint->Write(&packet, sizeof(packet), bytes_written, write_error))
                {
                    write_succeeded = false;
                    failed_msg_type = packet.msg_type;
                    const FanyImeIpc::ActiveClientTransition active = g_active_client_state.snapshot();
                    it->second.to_tsf_worker_thread_pipe.reset();
                    it->second.to_tsf_worker_thread_registration_id = 0;
                    it->second.to_tsf_worker_thread_ready = false;
                    it->second.outbound_state.mark_failed(FanyImeIpc::OutboundRoute::Worker);
                    if (active.client_id == client_id)
                    {
                        invalidation_epoch = g_active_client_state.invalidate(client_id, active.epoch);
                        if (invalidation_epoch != 0)
                        {
                            it->second.inactive_focus_token = it->second.focus_token;
                            it->second.focus_token = 0;
                        }
                    }
                    failed_endpoint_removed = true;
                    break;
                }
            }
        }
    }

    if (!endpoint)
    {
        LogPipeDisconnected(L"to-tsf-worker", failed_msg_type);
        return false;
    }
    if (!write_succeeded)
    {
        LogPipeWriteFailure(L"to-tsf-worker", failed_msg_type, write_error, bytes_written,
                            sizeof(FanyImeNamedpipeDataToTsfWorkerThread));
        if (failed_endpoint_removed)
        {
            endpoint->Shutdown();
            g_pipe_clients_cv.notify_all();
            FanyNamedPipe::EnqueuePipeSessionInvalidatedTask(client_id, invalidation_epoch);
        }
        return false;
    }
    return true;
}

bool SendWorkerPacket(uint64_t client_id, uint64_t activation_epoch, bool require_active, UINT msg_type,
                      const std::wstring &pipe_data)
{
    FanyImeNamedpipeDataToTsfWorkerThread packet = {};
    packet.msg_type = msg_type;
    if (!CopyPipeText(packet.data, pipe_data))
    {
        FANY_IPC_LOGF(L"[msime]: [ipc] to-tsf-worker payload too long: client_id={}, msg_type={}, chars={}", client_id,
                      msg_type, pipe_data.size());
        return false;
    }
    LogPipeWriteTarget(L"to-tsf-worker", client_id, msg_type, pipe_data);

    std::shared_ptr<PipeEndpoint> endpoint;
    DWORD bytes_written = 0;
    DWORD write_error = ERROR_SUCCESS;
    bool write_succeeded = false;
    bool failed_endpoint_removed = false;
    uint64_t invalidation_epoch = 0;
    {
        std::lock_guard lock(g_pipe_clients_mutex);
        auto it = g_pipe_clients.find(client_id);
        const bool route_is_current = !require_active || g_active_client_state.matches(client_id, activation_epoch);
        if (client_id != 0 && route_is_current && it != g_pipe_clients.end() && it->second.to_tsf_worker_thread_pipe &&
            it->second.to_tsf_worker_thread_registration_id != 0 && it->second.to_tsf_worker_thread_ready)
        {
            endpoint = it->second.to_tsf_worker_thread_pipe;
            write_succeeded = endpoint->Write(&packet, sizeof(packet), bytes_written, write_error);
            if (!write_succeeded)
            {
                const FanyImeIpc::ActiveClientTransition active = g_active_client_state.snapshot();
                it->second.to_tsf_worker_thread_pipe.reset();
                it->second.to_tsf_worker_thread_registration_id = 0;
                it->second.to_tsf_worker_thread_ready = false;
                it->second.outbound_state.mark_failed(FanyImeIpc::OutboundRoute::Worker);
                if (active.client_id == client_id)
                {
                    invalidation_epoch = g_active_client_state.invalidate(client_id, active.epoch);
                    if (invalidation_epoch != 0)
                    {
                        it->second.inactive_focus_token = it->second.focus_token;
                        it->second.focus_token = 0;
                    }
                }
                failed_endpoint_removed = true;
            }
        }
    }

    if (!endpoint)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        LogPipeDisconnected(L"to-tsf-worker", msg_type);
        return false;
    }
    if (!write_succeeded)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        LogPipeWriteFailure(L"to-tsf-worker", msg_type, write_error, bytes_written, sizeof(packet));
        if (failed_endpoint_removed)
        {
            endpoint->Shutdown();
            g_pipe_clients_cv.notify_all();
            FanyNamedPipe::EnqueuePipeSessionInvalidatedTask(client_id, invalidation_epoch);
        }
        return false;
    }
    return true;
}
} // namespace

bool SendToTsfWorkerThreadClientViaNamedpipe(uint64_t client_id, UINT msg_type, const std::wstring &pipeData)
{
    return SendWorkerPacket(client_id, 0, false, msg_type, pipeData);
}

bool SendToTsfWorkerThreadClientViaNamedpipe(uint64_t client_id, uint64_t activation_epoch, UINT msg_type,
                                             const std::wstring &pipeData)
{
    return SendWorkerPacket(client_id, activation_epoch, true, msg_type, pipeData);
}

bool SendVoiceCompositionToTsfWorker(uint64_t client_id, uint64_t activation_epoch, UINT msg_type,
                                     const std::wstring &text)
{
    static std::atomic<unsigned int> generation{1};
    unsigned int next = generation.fetch_add(1, std::memory_order_relaxed);
    if (next == 0 || next > 0xFFFFu)
    {
        generation.store(1, std::memory_order_relaxed);
        next = 1;
    }
    const std::vector<std::wstring> payloads =
        FanyImeVoiceCompositionPipe::EncodeSnapshot(text, static_cast<wchar_t>(next));
    if (payloads.empty())
    {
        return false;
    }
    std::vector<FanyImeNamedpipeDataToTsfWorkerThread> packets;
    packets.reserve(payloads.size());
    for (const std::wstring &payload : payloads)
    {
        FanyImeNamedpipeDataToTsfWorkerThread packet = {};
        packet.msg_type = msg_type;
        if (!CopyPipeText(packet.data, payload))
        {
            return false;
        }
        packets.push_back(packet);
    }
    return SendWorkerPackets(client_id, activation_epoch, true, packets);
}

void SendToTsfViaNamedpipe(UINT msg_type, const std::wstring &pipeData)
{
    const PipeClientActivation active = GetActivePipeClient();
    if (active.client_id == 0)
    {
        LogPipeDisconnected(L"to-tsf", msg_type);
        return;
    }
    SendToTsfClientViaNamedpipe(active.client_id, active.epoch, msg_type, 0, pipeData);
}

void SendToTsfWorkerThreadViaNamedpipe(UINT msg_type, const std::wstring &pipeData)
{
    const PipeClientActivation active = GetActivePipeClient();
    if (active.client_id == 0)
    {
        LogPipeDisconnected(L"to-tsf-worker", msg_type);
        return;
    }
    SendWorkerPacket(active.client_id, active.epoch, true, msg_type, pipeData);
}

void BroadcastToTsfWorkerThreadViaNamedpipe(UINT msg_type, const std::wstring &pipeData)
{
    std::vector<uint64_t> client_ids;
    {
        std::lock_guard lock(g_pipe_clients_mutex);
        client_ids.reserve(g_pipe_clients.size());
        for (const auto &[client_id, session] : g_pipe_clients)
        {
            if (client_id != 0 && session.to_tsf_worker_thread_pipe &&
                session.to_tsf_worker_thread_registration_id != 0 && session.to_tsf_worker_thread_ready)
            {
                client_ids.push_back(client_id);
            }
        }
    }
    for (const uint64_t client_id : client_ids)
    {
        SendToTsfWorkerThreadClientViaNamedpipe(client_id, msg_type, pipeData);
    }
}
