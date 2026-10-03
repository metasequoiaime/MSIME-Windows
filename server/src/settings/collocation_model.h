#pragma once

// octagram 语法模型（八股文，.gram）的按需下载与状态查询。内置目录（Catalog）随签名安装包
// 走：收录社区模型包需要改代码发版，不引入动态目录源。下载按目录条目的 host/path 直链进行，
// 落到 <DataDir>/models/<id>/<id>.gram——确定性布局让引擎侧解析与安装器「models/ 整目录升级
// 保留、卸载删除」对新 id 自动成立。
//
// 完整性靠格式，不靠摘要。上游会原地重传（tag 不变、字节变）且没有不可变的资产 URL，把摘要
// 钉死就意味着每次重训都要改代码、发一个签名安装包，否则所有用户的下载一起失败——这个成本
// 消不掉。而重训只换权重，「Rime::Grammar/」魔数与双数组布局不变，所以这里只要求字节能被
// 引擎读取端打开（魔数 + 边界检查，engine/ngram/octagram/octagram_gram.cpp 的 GramDb::open）；
// 读取端不按文件自带的长度做堆分配，未知权重的字节是安全输入。传输完整性交给 HTTPS 加
// Content-Length 比对。
//
// 状态以磁盘为准（模型文件存在 = ready，落位发生在格式校验之后），下载态是各进程内存里的
// runtime 状态：独立设置进程与 Server 各自能看到自己的下载进度。同一时刻至多一个下载在飞
// （单网络槽），忙碌时对新 id 的下载请求直接拒绝，页面在下载态禁用其余下载按钮。

#include <Windows.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace collocation
{

// 推荐包（万象）的 id：目录首条目，只是展示与排序上的推荐位，不承载默认或回退语义——
// 激活值默认留空 = 未选择任何模型（整句加成关闭）。
inline constexpr char kRecommendedModelId[] = "wanxiang-lts-zh-hans";

// 内置目录的一个条目。host/path 是 HTTPS 直链（release 资产与 raw 文件都经 302 跳 CDN，
// 下载器显式放开重定向）；size_hint/license/license_note 进设置页展示与 NOTICE.md 署名；
// badge 是候选窗的整句来源标签（不含六角括号），随包公布，用户据此分辨各家模型。
struct CatalogEntry
{
    const char *id;
    const char *display_name;
    const char *badge;
    const wchar_t *host;
    const wchar_t *path;
    const char *size_hint;
    const char *license;
    const char *license_note; // 可为空串：无补充说明
};

// 编译期内置目录，首条目是推荐包。
const std::vector<CatalogEntry> &Catalog();

// 条目的 HTTPS 直链。设置页把它给用户在浏览器里下载（应用内下载慢时的备用通道），
// 同一个地址也写进 NOTICE.md 作来源署名。
std::string SourceUrl(const CatalogEntry &entry);

// 候选窗给该模型包的整句候选挂的来源标签（不含六角括号）。目录里没有的 id——手填进
// config.toml 的自备模型——落回八股：那是 octagram 这套格式的通称，不冒充任何一家。
const char *BadgeForModel(const std::string &model_id);

struct ModelStatus
{
    std::string state; // "absent" | "downloading" | "importing" | "ready" | "error"
    int progress = 0;  // 下载中 0-100；其余状态无意义
    std::string error; // state == "error" 时的人类可读原因
};

// 磁盘现状 + 本进程下载态，按目录条目逐 id 给出。轻量，可随配置快照频繁调用。
std::map<std::string, ModelStatus> GetModelStatuses();

// 幂等启动指定模型的下载：该 id 已在下载或已就绪时返回 true 不重复启动；同一时刻至多一个
// 下载在飞，其他 id 忙碌时返回 false。目录里没有的 id、无法创建目录等同步失败也返回
// false，原因记入该 id 的状态。完成/失败经状态查询可见。
bool StartDownload(const std::string &model_id);

// 把用户在浏览器里自行下载的 .gram 导入为指定条目：后台复制到模型目录后，与应用内下载走
// 同一套格式校验与原子落位，校验不过同样进 error 态、不留残件。与下载共用单槽（任一下载或
// 导入在进行时拒绝其他 id）；源文件不存在或不是普通文件、目录里没有该 id 时返回 false。
bool ImportModel(const std::string &model_id, const std::filesystem::path &source);

// 弹出系统「打开文件」对话框让用户选 .gram 文件，取消时返回空路径。须在 UI 线程调用。
std::filesystem::path PromptForModelFile(HWND owner);

// 删除已下载的模型目录（.gram 与 NOTICE.md 一并）。守卫：该 id 下载或导入中拒绝。删除当前
// 激活的包时激活值一并清空（未选择），整句加成随之关闭，重新下载后需重新激活。若 .gram
// 还被引擎的内存映射占用，物理删除推迟到映射释放（状态查询按未下载呈现，重启 Server
// 必然清掉）。目录里没有的 id 返回 false。
bool DeleteModel(const std::string &model_id);

} // namespace collocation
