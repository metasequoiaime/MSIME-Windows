// [network] 代理配置的解析：设置页输入与系统代理字符串都要规整成 WinHTTP 与 libcurl 都认的
// host:port，认不出来的一律当无效，绝不把半截地址交给网络栈。
#include "tests/includes/test_framework.h"
#include "src/config/ime_config.h"
#include "src/utils/network_proxy.h"

#include <string>

TEST_CASE(network_proxy_server_normalization)
{
    REQUIRE_EQ(NormalizeNetworkProxyServer("127.0.0.1:7890"), std::string("127.0.0.1:7890"));
    REQUIRE_EQ(NormalizeNetworkProxyServer("  http://127.0.0.1:7890/  "), std::string("127.0.0.1:7890"));
    REQUIRE_EQ(NormalizeNetworkProxyServer("HTTP://proxy.example.com:08080"), std::string("proxy.example.com:8080"));
    REQUIRE_EQ(NormalizeNetworkProxyServer("[::1]:1080"), std::string("[::1]:1080"));
    // WinHTTP 只会说 HTTP 代理：其他 scheme、缺端口、越界端口、带账号密码的都拒绝。
    REQUIRE(NormalizeNetworkProxyServer("").empty());
    REQUIRE(NormalizeNetworkProxyServer("socks5://127.0.0.1:1080").empty());
    REQUIRE(NormalizeNetworkProxyServer("https://127.0.0.1:7890").empty());
    REQUIRE(NormalizeNetworkProxyServer("127.0.0.1").empty());
    REQUIRE(NormalizeNetworkProxyServer("127.0.0.1:").empty());
    REQUIRE(NormalizeNetworkProxyServer(":7890").empty());
    REQUIRE(NormalizeNetworkProxyServer("127.0.0.1:0").empty());
    REQUIRE(NormalizeNetworkProxyServer("127.0.0.1:65536").empty());
    REQUIRE(NormalizeNetworkProxyServer("user:pass@127.0.0.1:7890").empty());
    REQUIRE(NormalizeNetworkProxyServer("127.0.0.1:7890/path").empty());
}

TEST_CASE(network_proxy_setter_rejects_invalid_values_before_writing)
{
    // 这些分支在写盘之前就返回，不需要隔离的配置根。
    REQUIRE(!SetConfiguredNetworkString("proxy_mode", "auto"));
    REQUIRE(!SetConfiguredNetworkString("proxy_server", "socks5://127.0.0.1:1080"));
    REQUIRE(!SetConfiguredNetworkString("proxy_server", "not a proxy"));
    REQUIRE(!SetConfiguredNetworkString("proxy_user", "anyone"));
}

TEST_CASE(network_proxy_picks_https_entry_from_windows_proxy_list)
{
    using NetworkProxy::PickHttpsProxyFromWindowsList;
    // 不分协议的单项最常见（系统设置里只填一个地址）。
    REQUIRE_EQ(PickHttpsProxyFromWindowsList(L"127.0.0.1:7890"), std::string("127.0.0.1:7890"));
    // 分协议列表：请求都是 HTTPS，优先 https=，没有再退回 http=；socks 项不可用。
    REQUIRE_EQ(PickHttpsProxyFromWindowsList(L"http=10.0.0.1:80;https=10.0.0.2:443;socks=10.0.0.3:1080"),
               std::string("10.0.0.2:443"));
    REQUIRE_EQ(PickHttpsProxyFromWindowsList(L"http=10.0.0.1:80 socks=10.0.0.3:1080"), std::string("10.0.0.1:80"));
    REQUIRE(PickHttpsProxyFromWindowsList(L"socks=10.0.0.3:1080").empty());
    REQUIRE(PickHttpsProxyFromWindowsList(L"").empty());
}
