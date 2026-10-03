#include "collocation_model.h"

#include "config/ime_config.h"
#include "engine/core/data_path.h"
#include "engine/ngram/octagram/octagram_gram.h"
#include "utils/network_proxy.h"

#include <Windows.h>
#include <ShObjIdl.h>
#include <winhttp.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace collocation
{

// 编译期内置目录，首条目是推荐包。收录新模型 = 改这里发版，条目随签名安装包走；
// 不引入动态目录源，避免免签分发渠道。zh-moqi 的构建链未声明许可，license_note 如实
// 说明并标实验。
const std::vector<CatalogEntry> &Catalog()
{
    static const std::vector<CatalogEntry> kCatalog = {
        {kRecommendedModelId, "万象 LTS（推荐）", "万象", L"github.com",
         L"/amzxyz/RIME-LMDG/releases/download/LTS/wanxiang-lts-zh-hans.gram", "约 390 MB", "CC-BY-4.0",
         "© amzxyz / RIME-LMDG 项目"},
        {"zh-hans-t-essay-bgw", "八股文·词级", "八股", L"github.com",
         L"/lotem/rime-octagram-data/releases/download/20260712/zh-hans-t-essay-bgw.gram", "约 197 MB", "LGPL", ""},
        {"zh-hans-t-essay-bgw-compact", "八股文·词级紧凑", "八股", L"github.com",
         L"/lotem/rime-octagram-data/releases/download/20260712/zh-hans-t-essay-bgw-compact.gram", "约 39 MB", "LGPL",
         ""},
        {"zh-moqi", "白霜（实验）", "墨奇", L"raw.githubusercontent.com", L"/gaboolic/rime-frost/master/zh-moqi.gram",
         "约 7 MB", "GPL-3.0", "随 GPL-3.0 仓库（gaboolic/rime-frost）分发，构建链未声明许可，实验性收录"},
    };
    return kCatalog;
}

namespace
{

const CatalogEntry *FindEntry(const std::string &model_id)
{
    for (const auto &entry : Catalog())
    {
        if (model_id == entry.id)
            return &entry;
    }
    return nullptr;
}

std::filesystem::path ModelDirectory(const std::string &model_id)
{
    return metasequoia::data_directory() / "models" / model_id;
}

std::filesystem::path ModelFile(const std::string &model_id)
{
    return ModelDirectory(model_id) / (model_id + ".gram");
}

std::filesystem::path PartFile(const std::string &model_id)
{
    // 同一卷上的临时名，下载完成后原子 rename 到位；放进模型自己的目录，多个模型的
    // 下载互不碰撞。
    return ModelDirectory(model_id) / (model_id + ".gram.part");
}

// 逻辑删除的清扫：.gram 被引擎的内存映射占着时物理删除会失败，DeleteModel 落下的
// .trash 标记把目录转成「待清扫」。这里在状态查询与重新下载前重试，映射一旦释放
// （切换模型或重启 Server）目录就被清干净。
void PurgeIfTrashed(const std::string &model_id)
{
    std::error_code error;
    const std::filesystem::path trash = ModelDirectory(model_id) / ".trash";
    if (!std::filesystem::exists(trash, error) || error)
    {
        return;
    }
    if (std::filesystem::exists(ModelFile(model_id), error) && !error)
    {
        std::filesystem::remove(ModelFile(model_id), error);
        if (error)
        {
            return;
        }
    }
    std::filesystem::remove_all(ModelDirectory(model_id), error);
}

// 目录条目的 host/path 都是 ASCII（域名与仓库路径），NOTICE.md 是 UTF-8 文本，写之前把
// 宽字符转回来；走 CP_UTF8 而不是逐字节截断，将来条目里出现非 ASCII 也不会产出坏字节。
std::string Utf8FromWide(const wchar_t *wide)
{
    if (wide == nullptr)
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(bytes > 0 ? static_cast<size_t>(bytes) - 1 : 0, '\0');
    if (bytes > 1)
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow.data(), bytes, nullptr, nullptr);
    return narrow;
}

// 格式锁：唯一真正拦下坏模型的检查。让引擎读取端自己开一次，Magic 与双数组边界
// 都在 open() 里，越界在 set_array 之前就被拒，且不按文件自带的长度做堆分配。
// 刻意用局部实例而不是 shared_gram_db——后者按路径永久缓存，会一直攥着 .part 的
// 映射，而映射是以 FILE_SHARE_READ 打开的（没有 FILE_SHARE_DELETE），改名将失败。
bool ValidateFormat(const std::filesystem::path &file, std::string &error)
{
    gram::GramDb db;
    if (db.open(file))
    {
        return true;
    }
    error = db.error();
    return false;
}

enum class DownloadState
{
    Idle,
    Downloading,
    Importing,
    Error,
};

// 下载与导入共用单槽：两者都在写同一个模型目录，且都要等格式校验落位。
bool IsBusy(DownloadState state)
{
    return state == DownloadState::Downloading || state == DownloadState::Importing;
}

struct RuntimeState
{
    DownloadState state = DownloadState::Idle;
    int progress = 0;
    std::string error;
};

std::mutex g_mutex;
// 按 id 的下载态，map 大小就是目录条目数。成功后该 id 留下一个 Idle 条目，无碍：
// 查询侧以磁盘为准，runtime 态只在文件缺席时补充 downloading/error。
std::map<std::string, RuntimeState> g_states;

void SetState(const std::string &model_id, DownloadState state, int progress, std::string error)
{
    std::lock_guard lock(g_mutex);
    RuntimeState &runtime = g_states[model_id];
    runtime.state = state;
    runtime.progress = progress;
    runtime.error = std::move(error);
}

void InstallPart(const CatalogEntry &entry, bool imported);

// 后台线程：下载 -> 校验 -> 原子落位。全程只碰 PartFile，失败时不留半个模型
// 在正式位置上。条目按值收进线程：detached 线程的生命周期长于调用栈，host/path/id
// 必须自持。release 资产与 raw 文件都经 302 跳转到 CDN，显式放开自动重定向。
void DownloadThread(CatalogEntry entry)
{
    const std::string model_id = entry.id;
    SetState(model_id, DownloadState::Downloading, 0, {});

    std::error_code fs_error;
    const std::filesystem::path directory = ModelDirectory(model_id);
    std::filesystem::create_directories(directory, fs_error);
    if (fs_error)
    {
        SetState(model_id, DownloadState::Error, 0, "无法创建模型目录");
        return;
    }

    HINTERNET session = NetworkProxy::OpenWinHttpSession(L"MetasequoiaImeServer/1.0");
    if (session == nullptr)
    {
        SetState(model_id, DownloadState::Error, 0, "无法初始化网络");
        return;
    }
    // 400MB 不是快操作：连接阶段给足超时，收发阶段不设上限靠进度与失败兜底。
    WinHttpSetTimeouts(session, 30000, 30000, 30000, 0);
    HINTERNET connection = WinHttpConnect(session, entry.host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = nullptr;
    if (connection != nullptr)
    {
        request = WinHttpOpenRequest(connection, L"GET", entry.path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request != nullptr)
        {
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        }
    }
    bool ok = connection != nullptr && request != nullptr &&
              WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (ok)
    {
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
        ok = status == HTTP_STATUS_OK;
    }

    // 总长取响应自己声明的 Content-Length，不再钉死字节数常量：上游重训会让字节数
    // 变，而这个值顺带充当截断检测的基准（收完比对一次），比钉常量更贴近实际。
    long long expected = 0;
    if (ok)
    {
        DWORD length = 0;
        DWORD length_size = sizeof(length);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size, WINHTTP_NO_HEADER_INDEX))
        {
            expected = length;
        }
    }

    std::ofstream out;
    if (ok)
    {
        out.open(PartFile(model_id), std::ios::binary | std::ios::trunc);
        ok = static_cast<bool>(out);
    }

    long long received = 0;
    while (ok)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            ok = false;
            break;
        }
        if (available == 0)
            break;
        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0)
        {
            ok = false;
            break;
        }
        out.write(buffer.data(), static_cast<std::streamsize>(read));
        if (!out)
        {
            ok = false;
            break;
        }
        received += read;
        if (expected > 0)
        {
            const int progress = static_cast<int>((std::min)(100LL, received * 100LL / expected));
            SetState(model_id, DownloadState::Downloading, progress, {});
        }
    }
    if (request != nullptr)
        WinHttpCloseHandle(request);
    if (connection != nullptr)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    out.close();

    if (!ok)
    {
        SetState(model_id, DownloadState::Error, 0,
                 status != 0 && status != HTTP_STATUS_OK ? "下载响应异常" : "下载中断");
        std::error_code cleanup;
        std::filesystem::remove(PartFile(model_id), cleanup);
        return;
    }

    // 传输完整性：收完的字节数必须等于响应声明的 Content-Length。声明缺失时跳过，
    // 交给下面的格式校验兜底。
    if (expected > 0 && received != expected)
    {
        SetState(model_id, DownloadState::Error, 0, "下载字节数与响应声明不符");
        std::error_code cleanup;
        std::filesystem::remove(PartFile(model_id), cleanup);
        return;
    }

    InstallPart(entry, false);
}

// 后台线程：把用户在浏览器里下好的文件复制成 PartFile，之后与下载走同一条校验落位路径。
// 先复制再校验，而不是直接校验原文件：校验通过后的 rename 必须在同一卷上原子完成，
// 原文件可能在别的盘，也不该被我们挪走。
void ImportThread(CatalogEntry entry, std::filesystem::path source)
{
    const std::string model_id = entry.id;
    std::error_code fs_error;
    std::filesystem::create_directories(ModelDirectory(model_id), fs_error);
    if (fs_error)
    {
        SetState(model_id, DownloadState::Error, 0, "无法创建模型目录");
        return;
    }
    std::filesystem::copy_file(source, PartFile(model_id), std::filesystem::copy_options::overwrite_existing, fs_error);
    if (fs_error)
    {
        SetState(model_id, DownloadState::Error, 0, "无法读取所选文件");
        std::error_code cleanup;
        std::filesystem::remove(PartFile(model_id), cleanup);
        return;
    }
    InstallPart(entry, true);
}

// PartFile 已完整写好之后的公共尾段：格式校验 -> 原子落位 -> NOTICE。下载与本地导入都走这里，
// 所以「文件存在 = 曾通过校验」对两条来源同样成立。
void InstallPart(const CatalogEntry &entry, bool imported)
{
    const std::string model_id = entry.id;

    // 格式锁：唯一真正拦下坏模型的检查，也是「已下载」的定义——能被引擎读取端
    // 打开（魔数与双数组边界合法）就放行落位，打开不了的一律按损坏拒绝。
    std::string format_error;
    if (!ValidateFormat(PartFile(model_id), format_error))
    {
        SetState(model_id, DownloadState::Error, 0, "模型格式校验失败（上游可能改了格式）：" + format_error);
        std::error_code cleanup;
        std::filesystem::remove(PartFile(model_id), cleanup);
        return;
    }

    // ValidateFormat 的局部 GramDb 已析构、映射已解除，否则下面的 rename 会被
    // 以 FILE_SHARE_READ 打开的旧句柄拒绝。
    std::error_code rename_error;
    std::filesystem::rename(PartFile(model_id), ModelFile(model_id), rename_error);
    if (rename_error)
    {
        // 目标 .gram 的旧映射还在（.trash 场景下词典未松手）时 rename 会失败：进
        // Error 态，等映射释放后重试即可。与其他失败路径一致，.part 不留——重试
        // 反正会整个重下，留着没有收益。
        SetState(model_id, DownloadState::Error, 0, "模型落位失败");
        std::error_code cleanup;
        std::filesystem::remove(PartFile(model_id), cleanup);
        return;
    }

    // 署名义务随模型走：NOTICE 与模型同目录，注明来源与许可；各条目许可不同，
    // zh-moqi 的许可状态在 license_note 里如实说明。
    std::error_code size_error;
    const long long model_bytes = static_cast<long long>(std::filesystem::file_size(ModelFile(model_id), size_error));
    std::ofstream notice(ModelDirectory(model_id) / "NOTICE.md", std::ios::binary | std::ios::trunc);
    if (notice)
    {
        const std::string note = entry.license_note == nullptr ? std::string() : std::string(entry.license_note);
        notice << "# " << entry.display_name << "（" << model_id << "）\n\n"
               << "- 来源：" << SourceUrl(entry) << (imported ? "（用户自行下载后本地导入）" : "") << "\n"
               << "- 许可：" << entry.license << (note.empty() ? "" : "（" + note + "）") << "\n"
               << "- 文件：" << model_id << ".gram（" << model_bytes << " 字节）\n";
    }
    SetState(model_id, DownloadState::Idle, 100, {});
}

// 下载与导入的公共前置：目录内 id、已就绪不重做、单槽占位。占位成功返回 entry 并把该 id
// 置为 state；请求已被满足（已就绪，或同 id 同类任务已在进行）时 satisfied 置 true 并返回
// nullptr；其余拒绝返回 nullptr。
const CatalogEntry *ClaimSlot(const std::string &model_id, DownloadState state, bool &satisfied)
{
    satisfied = false;
    const CatalogEntry *entry = FindEntry(model_id);
    if (entry == nullptr)
        return nullptr;
    PurgeIfTrashed(model_id);
    std::error_code fs_error;
    // 已存在且通过格式校验的包不重做：落位只在格式校验之后发生，文件存在即曾通过校验，
    // 重做只会白白覆盖一份好包。逻辑已删除（.trash 在）的包不在此列：它们必须允许重新
    // 获取，哪怕旧 .gram 的物理删除还在等映射释放（小包可能在旧映射释放前就落位，
    // 落位失败进 Error 态，等映射释放后重试即可成功）。
    const bool trashed = std::filesystem::exists(ModelDirectory(model_id) / ".trash", fs_error) && !fs_error;
    if (!trashed && std::filesystem::exists(ModelFile(model_id), fs_error) && !fs_error)
    {
        satisfied = true;
        return nullptr;
    }
    std::lock_guard lock(g_mutex);
    // 单槽：同一时刻至多一个下载或导入，不做并发队列。同 id 同类请求幂等；其他 id
    // 忙碌时拒绝，页面在忙碌态禁用其余下载/导入按钮。
    for (const auto &[id, runtime] : g_states)
    {
        if (IsBusy(runtime.state))
        {
            satisfied = id == model_id && runtime.state == state;
            return nullptr;
        }
    }
    RuntimeState &runtime = g_states[model_id];
    runtime.state = state;
    runtime.progress = 0;
    runtime.error.clear();
    return entry;
}

} // namespace

std::string SourceUrl(const CatalogEntry &entry)
{
    return "https://" + Utf8FromWide(entry.host) + Utf8FromWide(entry.path);
}

std::map<std::string, ModelStatus> GetModelStatuses()
{
    std::map<std::string, ModelStatus> statuses;
    for (const auto &entry : Catalog())
    {
        PurgeIfTrashed(entry.id);
        std::error_code fs_error;
        // runtime 下载与错误态优先于磁盘：刚失败的落位比「旧的已验证文件还在」更值得
        // 看见。逻辑已删除（.trash 标记在）一律按未下载呈现，哪怕 .gram 还没物理消失。
        {
            std::lock_guard lock(g_mutex);
            const auto it = g_states.find(entry.id);
            if (it != g_states.end() && it->second.state == DownloadState::Downloading)
            {
                statuses[entry.id] = {"downloading", it->second.progress, {}};
                continue;
            }
            if (it != g_states.end() && it->second.state == DownloadState::Importing)
            {
                statuses[entry.id] = {"importing", 0, {}};
                continue;
            }
            if (it != g_states.end() && it->second.state == DownloadState::Error)
            {
                statuses[entry.id] = {"error", 0, it->second.error};
                continue;
            }
        }
        if (std::filesystem::exists(ModelDirectory(entry.id) / ".trash", fs_error) && !fs_error)
        {
            statuses[entry.id] = {"absent", 0, {}};
            continue;
        }
        // 落位发生在格式校验之后，所以「文件存在」即代表曾经通过校验。这里不再比对
        // 字节数：钉死它就等于钉死上游的每次重训。
        if (std::filesystem::exists(ModelFile(entry.id), fs_error) && !fs_error)
        {
            statuses[entry.id] = {"ready", 100, {}};
            continue;
        }
        statuses[entry.id] = {"absent", 0, {}};
    }
    return statuses;
}

// 手填的 id（自备模型、目录收录前就装好的包）不在目录里。八股是 octagram 这套格式的通称，
// 不属于任何一家，用它兜底比挂一个猜出来的品牌名诚实。
constexpr const char *kGenericBadge = "八股";

const char *BadgeForModel(const std::string &model_id)
{
    const CatalogEntry *entry = FindEntry(model_id);
    return entry == nullptr ? kGenericBadge : entry->badge;
}

bool StartDownload(const std::string &model_id)
{
    bool satisfied = false;
    const CatalogEntry *entry = ClaimSlot(model_id, DownloadState::Downloading, satisfied);
    if (entry == nullptr)
        return satisfied;
    std::thread(DownloadThread, *entry).detach();
    return true;
}

bool ImportModel(const std::string &model_id, const std::filesystem::path &source)
{
    std::error_code fs_error;
    if (!std::filesystem::is_regular_file(source, fs_error) || fs_error)
        return false;
    bool satisfied = false;
    const CatalogEntry *entry = ClaimSlot(model_id, DownloadState::Importing, satisfied);
    if (entry == nullptr)
        return satisfied;
    std::thread(ImportThread, *entry, source).detach();
    return true;
}

std::filesystem::path PromptForModelFile(HWND owner)
{
    // 设置窗口的 UI 线程早已为 WebView2 初始化过 STA，这里的 CoInitializeEx 只是配平引用计数。
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::filesystem::path selected;
    IFileOpenDialog *dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        const COMDLG_FILTERSPEC filters[] = {{L"octagram 语法模型 (*.gram)", L"*.gram"}, {L"所有文件", L"*.*"}};
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetTitle(L"选择已下载的 .gram 模型文件");
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options)))
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                selected = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    return selected;
}

bool DeleteModel(const std::string &model_id)
{
    if (FindEntry(model_id) == nullptr)
        return false;
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_states.find(model_id);
        if (it != g_states.end() && IsBusy(it->second.state))
            return false;
    }
    PurgeIfTrashed(model_id);
    // 删的是当前激活的包时先清激活再删文件：清空写入失败就原地返回 false，什么都没
    // 发生，用户重试即可；反过来先删文件再清空，一次写入失败就会留下「指向已删除包」
    // 的僵尸选中态。未选择（留空）时没有需要清的目标。
    if (model_id == GetConfiguredAssocSentenceCollocationModel() && !SetConfiguredAssocSentenceCollocationModel(""))
    {
        return false;
    }
    std::error_code error;
    // .gram 与 NOTICE.md 都在这个目录里，整目录删除即卸载；目录不存在时 remove_all
    // 无错返回，删除因此是幂等的。
    std::filesystem::remove_all(ModelDirectory(model_id), error);
    if (std::filesystem::exists(ModelFile(model_id), error))
    {
        // .gram 还被引擎的内存映射占着（激活后打过字就会映射，句柄不带
        // FILE_SHARE_DELETE），物理删除要等引用释放。落一个标记把删除降级为逻辑删除：
        // 状态按未下载呈现、不可选中；下面的 evict 摘掉缓存引用后，词典在下一次应用
        // 配置时松手，状态查询的重试清扫就能真正删掉文件。
        std::ofstream trash(ModelDirectory(model_id) / ".trash", std::ios::binary | std::ios::trunc);
    }
    // 引擎缓存里这个路径的映射引用摘掉：词典在下一次应用配置（击键）松手后，
    // 上面的 .trash 清扫就能真正物理删除残留的 .gram（不用等重启）。
    gram::shared_gram_db_evict(ModelFile(model_id));
    return true;
}

} // namespace collocation
