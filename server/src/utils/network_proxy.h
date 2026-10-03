#pragma once

// 出站请求的代理接入点。Server 侧所有联网路径（云输入、octagram 模型下载、AI 联想、翻译、
// 语音识别与润色、设置页的凭证测试）都经这里按 [network] 配置挑代理，不要在调用点各自
// WinHttpOpen / 依赖 libcurl 的默认值：
//
// - system：WinHTTP 用 AUTOMATIC_PROXY（读用户的系统代理与 PAC，旧代码的 DEFAULT_PROXY
//   只认 netsh winhttp，绝大多数机器上等于直连）；libcurl 本身不读系统设置，这里取当前用户
//   的静态代理（Internet 选项里填的那一项）补上，PAC 脚本不解析。
// - none：两边都显式直连，连环境变量里的代理也不用。
// - custom：用配置里的 HTTP 代理（host:port）。地址为空时按 system 处理。
//
// 每次请求现读配置：设置页改完立刻生效，不需要重启 Server。

#include <Windows.h>
#include <curl/curl.h>
#include <winhttp.h>

#include <string>

namespace NetworkProxy
{

// WinHttpOpen 的替身：按当前代理配置打开会话句柄，失败返回 nullptr。
HINTERNET OpenWinHttpSession(const wchar_t *user_agent);

// 给 curl 句柄设好 CURLOPT_PROXY / CURLOPT_NOPROXY。
void ApplyToCurl(CURL *curl);

// 从 Windows 代理字符串（"host:port" 或 "http=a:1;https=b:2;socks=c:3"）里挑出给 HTTPS
// 请求用的 HTTP 代理，返回 host:port；没有可用项时返回空串。暴露出来供测试。
std::string PickHttpsProxyFromWindowsList(const std::wstring &proxy_list);

} // namespace NetworkProxy
