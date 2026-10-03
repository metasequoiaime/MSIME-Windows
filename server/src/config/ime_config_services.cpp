// 在线服务配置：语音输入（识别与润色的提供商、凭证槽位、热键）、AI 助手、翻译服务（腾讯 TMT、自定义翻译、小牛翻译）
// 和这些服务共用的网络代理。
#include "config/ime_config_internal.h"
#include <algorithm>
#include <cctype>
#include <mutex>
#include <shared_mutex>
#include <string>
#include "voice-input/voice_providers.h"

using namespace ime_config_detail;

VoiceInputConfig GetConfiguredVoiceInput()
{
    return SnapshotVoiceInput();
}

bool SetConfiguredVoiceInputString(const std::string &key, const std::string &value)
{
    if (key == "language" && value != "zh-cn" && value != "en" && value != "auto")
        return false;
    if (key == "commit_mode" && value != "tsf" && value != "sendinput" && value != "ctrl_v")
        return false;
    if (key == "polish_prompt_id" && value != "cleanup" && value != "faithful" && value != "zh2en" &&
        value != "casual" && value != "custom_1" && value != "custom_2" && value != "custom_3")
        return false;

    // Fields are addressed by pointer-to-member rather than by a bare reference because the store has to happen under
    // the writer lock, while WriteConfiguredValue does file I/O and a cross-process wait that must never run with that
    // lock held.
    const auto persist = [](const std::string &toml_key, const std::string &toml_value,
                            std::string VoiceInputConfig::*field) {
        if (!WriteConfiguredValue("voice_input", toml_key, EscapeTomlBasicString(toml_value)))
            return false;
        std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
        g_voice_input.*field = toml_value;
        return true;
    };

    if (key.rfind("asr_token_", 0) == 0)
    {
        const std::string provider = key.substr(std::string("asr_token_").size());
        if (VoiceInput::AsrTokenSlotKey(provider) != key)
            return false;
        const std::string id = VoiceInput::NormalizeProviderId(provider);
        if (!WriteConfiguredValue("voice_input", key, EscapeTomlBasicString(value)))
            return false;
        std::string asr_provider;
        {
            std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
            g_voice_input.asr_tokens[id] = value;
            asr_provider = g_voice_input.asr_provider;
        }
        if (VoiceInput::NormalizeProviderId(asr_provider) == id)
            persist("asr_token", value, &VoiceInputConfig::asr_token);
        return true;
    }
    if (key.rfind("polish_token_", 0) == 0)
    {
        const std::string provider = key.substr(std::string("polish_token_").size());
        if (VoiceInput::PolishTokenSlotKey(provider) != key)
            return false;
        const std::string id = VoiceInput::NormalizeProviderId(provider);
        if (!WriteConfiguredValue("voice_input", key, EscapeTomlBasicString(value)))
            return false;
        std::string polish_provider;
        {
            std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
            g_voice_input.polish_tokens[id] = value;
            polish_provider = g_voice_input.polish_provider;
        }
        if (VoiceInput::NormalizeProviderId(polish_provider) == id)
            persist("polish_token", value, &VoiceInputConfig::polish_token);
        return true;
    }

    std::string VoiceInputConfig::*target = nullptr;
    if (key == "asr_provider")
        target = &VoiceInputConfig::asr_provider;
    else if (key == "asr_app_key")
        target = &VoiceInputConfig::asr_app_key;
    else if (key == "doubao_auth_mode")
        target = &VoiceInputConfig::doubao_auth_mode;
    else if (key == "asr_token")
        target = &VoiceInputConfig::asr_token;
    else if (key == "asr_endpoint")
        target = &VoiceInputConfig::asr_endpoint;
    else if (key == "asr_resource_id")
        target = &VoiceInputConfig::asr_resource_id;
    else if (key == "doubao_boosting_table_id")
        target = &VoiceInputConfig::doubao_boosting_table_id;
    else if (key == "asr_model")
        target = &VoiceInputConfig::asr_model;
    else if (key == "polish_provider")
        target = &VoiceInputConfig::polish_provider;
    else if (key == "polish_token")
        target = &VoiceInputConfig::polish_token;
    else if (key == "polish_endpoint")
        target = &VoiceInputConfig::polish_endpoint;
    else if (key == "polish_model")
        target = &VoiceInputConfig::polish_model;
    else if (key == "polish_prompt_id")
        target = &VoiceInputConfig::polish_prompt_id;
    else if (key == "polish_prompt")
        target = &VoiceInputConfig::polish_prompt;
    else if (key == "polish_prompt_custom_1")
        target = &VoiceInputConfig::polish_prompt_custom_1;
    else if (key == "polish_prompt_custom_2")
        target = &VoiceInputConfig::polish_prompt_custom_2;
    else if (key == "polish_prompt_custom_3")
        target = &VoiceInputConfig::polish_prompt_custom_3;
    else if (key == "language")
        target = &VoiceInputConfig::language;
    else if (key == "commit_mode")
        target = &VoiceInputConfig::commit_mode;
    if (!target || !WriteConfiguredValue("voice_input", key, EscapeTomlBasicString(value)))
        return false;
    {
        std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
        g_voice_input.*target = value;
    }
    if (key == "polish_prompt_custom_1")
    {
        WriteConfiguredValue("voice_input", "polish_prompt", EscapeTomlBasicString(""));
        std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
        g_voice_input.polish_prompt.clear();
    }
    if (key == "asr_token")
    {
        const std::string asr_provider = SnapshotVoiceInput().asr_provider;
        const std::string slot = VoiceInput::AsrTokenSlotKey(asr_provider);
        if (!slot.empty())
        {
            const std::string id = VoiceInput::NormalizeProviderId(asr_provider);
            {
                std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
                g_voice_input.asr_tokens[id] = value;
            }
            WriteConfiguredValue("voice_input", slot, EscapeTomlBasicString(value));
        }
    }
    else if (key == "polish_token")
    {
        const std::string polish_provider = SnapshotVoiceInput().polish_provider;
        const std::string slot = VoiceInput::PolishTokenSlotKey(polish_provider);
        if (!slot.empty())
        {
            const std::string id = VoiceInput::NormalizeProviderId(polish_provider);
            {
                std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
                g_voice_input.polish_tokens[id] = value;
            }
            WriteConfiguredValue("voice_input", slot, EscapeTomlBasicString(value));
        }
    }
    else if (key == "asr_provider")
    {
        persist("asr_token", VoiceInput::ResolveAsrToken(SnapshotVoiceInput()), &VoiceInputConfig::asr_token);
    }
    else if (key == "polish_provider")
    {
        persist("polish_token", VoiceInput::ResolvePolishToken(SnapshotVoiceInput()), &VoiceInputConfig::polish_token);
    }
    return true;
}

bool SetConfiguredVoiceInputBool(const std::string &key, bool value)
{
    bool VoiceInputConfig::*target = nullptr;
    if (key == "voice_input")
        target = &VoiceInputConfig::enabled;
    else if (key == "hotkey_ralt")
        target = &VoiceInputConfig::hotkey_ralt;
    else if (key == "hotkey_ctrl_f9")
        target = &VoiceInputConfig::hotkey_ctrl_f9;
    else if (key == "hotkey_ctrl_win")
        target = &VoiceInputConfig::hotkey_ctrl_win;
    else if (key == "hotkey_rctrl_ralt")
        target = &VoiceInputConfig::hotkey_rctrl_ralt;
    else if (key == "hotkey_hold_space_lock")
        target = &VoiceInputConfig::hotkey_hold_space_lock;
    else if (key == "start_sound")
        target = &VoiceInputConfig::start_sound;
    else if (key == "end_sound")
        target = &VoiceInputConfig::end_sound;
    else if (key == "mute_system_audio")
        target = &VoiceInputConfig::mute_system_audio;
    else if (key == "doubao_enable_itn")
        target = &VoiceInputConfig::doubao_enable_itn;
    else if (key == "doubao_enable_punc")
        target = &VoiceInputConfig::doubao_enable_punc;
    else if (key == "doubao_enable_ddc")
        target = &VoiceInputConfig::doubao_enable_ddc;
    else if (key == "notification_sound")
    {
        // Accept settings pages from older installations during a rolling update.
        if (!WriteConfiguredValue("voice_input", key, value ? "true" : "false"))
            return false;
        std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
        g_voice_input.start_sound = value;
        g_voice_input.end_sound = value;
        return true;
    }
    else if (key == "polish_text")
        target = &VoiceInputConfig::polish_text;
    else if (key == "stream_inline_preedit")
        target = &VoiceInputConfig::stream_inline_preedit;
    if (!target || !WriteConfiguredValue("voice_input", key, value ? "true" : "false"))
        return false;
    std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
    g_voice_input.*target = value;
    return true;
}

const AiAssistantConfig &GetConfiguredAiAssistant()
{
    return g_ai_assistant;
}

const TencentTmtConfig &GetConfiguredTencentTmt()
{
    return g_tencent_tmt;
}

bool SetConfiguredTencentTmtString(const std::string &key, const std::string &value)
{
    std::string *target = nullptr;
    if (key == "secret_id")
        target = &g_tencent_tmt.secret_id;
    else if (key == "secret_key")
        target = &g_tencent_tmt.secret_key;
    else if (key == "region")
        target = &g_tencent_tmt.region;
    else if (key == "target_language" && (value == "en" || value == "fr" || value == "ja" || value == "es" ||
                                          value == "ru" || value == "de" || value == "ko"))
        target = &g_tencent_tmt.target_language;
    if (!target || !WriteConfiguredValue("tencent_tmt", key, EscapeTomlBasicString(value)))
        return false;
    *target = value;
    return true;
}

const CustomTranslationConfig &GetConfiguredCustomTranslation()
{
    return g_custom_translation;
}

bool SetConfiguredCustomTranslationBool(const std::string &key, bool value)
{
    if (key != "enabled" || !WriteConfiguredValue("custom_translation", key, value ? "true" : "false"))
        return false;
    g_custom_translation.enabled = value;
    return true;
}

bool SetConfiguredCustomTranslationString(const std::string &key, const std::string &value)
{
    std::string *target = nullptr;
    if (key == "endpoint")
        target = &g_custom_translation.endpoint;
    else if (key == "api_key")
        target = &g_custom_translation.api_key;
    if (!target || !WriteConfiguredValue("custom_translation", key, EscapeTomlBasicString(value)))
        return false;
    *target = value;
    return true;
}

const NiuTransConfig &GetConfiguredNiuTrans()
{
    return g_niutrans;
}

bool SetConfiguredNiuTransBool(const std::string &key, bool value)
{
    if (key != "enabled" || !WriteConfiguredValue("niutrans", key, value ? "true" : "false"))
        return false;
    g_niutrans.enabled = value;
    return true;
}

bool SetConfiguredNiuTransString(const std::string &key, const std::string &value)
{
    std::string *target = nullptr;
    if (key == "app_id")
        target = &g_niutrans.app_id;
    else if (key == "apikey")
        target = &g_niutrans.apikey;
    if (!target || !WriteConfiguredValue("niutrans", key, EscapeTomlBasicString(value)))
        return false;
    *target = value;
    return true;
}

std::string NormalizeNetworkProxyServer(const std::string &value)
{
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    const auto begin = std::find_if(value.begin(), value.end(), not_space);
    const auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
    std::string text = begin < end ? std::string(begin, end) : std::string();
    std::string lowered = text;
    for (char &ch : lowered)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lowered.rfind("http://", 0) == 0)
        text.erase(0, std::string("http://").size());
    else if (lowered.find("://") != std::string::npos)
        return {};
    while (!text.empty() && text.back() == '/')
        text.pop_back();

    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == text.size())
        return {};
    const std::string host = text.substr(0, colon);
    const std::string port = text.substr(colon + 1);
    if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char ch) { return std::isdigit(ch); }))
        return {};
    const int port_number = std::stoi(port);
    if (port_number < 1 || port_number > 65535)
        return {};
    // 方括号 IPv6 字面量或主机名/IPv4；不收用户名密码（WinHTTP 的命名代理不认 user:pass@）。
    const bool bracketed = host.size() > 2 && host.front() == '[' && host.back() == ']' &&
                           std::all_of(host.begin() + 1, host.end() - 1,
                                       [](unsigned char ch) { return std::isxdigit(ch) || ch == ':' || ch == '.'; });
    const bool plain = std::all_of(host.begin(), host.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_';
    });
    if (!bracketed && !plain)
        return {};
    return host + ":" + std::to_string(port_number);
}

NetworkProxyConfig GetConfiguredNetworkProxy()
{
    std::lock_guard<std::mutex> lock(g_network_proxy_mutex);
    return g_network_proxy;
}

bool SetConfiguredNetworkString(const std::string &key, const std::string &value)
{
    std::string stored;
    if (key == "proxy_mode")
    {
        if (value != "system" && value != "none" && value != "custom")
            return false;
        stored = value;
    }
    else if (key == "proxy_server")
    {
        // 留空是合法的「未填写」；非空但解析不出 host:port 的一律拒绝，不把坏值写进配置。
        const bool blank = value.find_first_not_of(" \t") == std::string::npos;
        stored = blank ? std::string() : NormalizeNetworkProxyServer(value);
        if (!blank && stored.empty())
            return false;
    }
    else
    {
        return false;
    }
    if (!WriteConfiguredValue("network", key, EscapeTomlBasicString(stored)))
        return false;
    std::lock_guard<std::mutex> lock(g_network_proxy_mutex);
    (key == "proxy_mode" ? g_network_proxy.mode : g_network_proxy.server) = stored;
    return true;
}

bool SetConfiguredAiAssistantString(const std::string &key, const std::string &value)
{
    if (key == "prompt_id" && value != "custom_1" && value != "custom_2" && value != "custom_3")
        return false;
    if (key == "provider")
    {
        const std::string provider = VoiceInput::NormalizeProviderId(value);
        if (AiAssistantTokenSlotKey(provider).empty())
            return false;
        const std::string token = g_ai_assistant.tokens[provider];
        const std::string endpoint = g_ai_assistant.endpoints[provider];
        const std::string model = g_ai_assistant.models[provider];
        if (!WriteConfiguredValues(
                {{"ai_assistant", "endpoint_" + g_ai_assistant.provider,
                  EscapeTomlBasicString(g_ai_assistant.endpoint)},
                 {"ai_assistant", "model_" + g_ai_assistant.provider, EscapeTomlBasicString(g_ai_assistant.model)},
                 {"ai_assistant", "provider", EscapeTomlBasicString(provider)},
                 {"ai_assistant", "token", EscapeTomlBasicString(token)},
                 {"ai_assistant", "endpoint", EscapeTomlBasicString(endpoint)},
                 {"ai_assistant", "model", EscapeTomlBasicString(model)}}))
            return false;
        g_ai_assistant.provider = provider;
        g_ai_assistant.token = token;
        g_ai_assistant.endpoint = endpoint;
        g_ai_assistant.model = model;
        return true;
    }
    if (key == "endpoint" || key == "model")
    {
        const std::string escaped = EscapeTomlBasicString(value);
        if (!WriteConfiguredValues(
                {{"ai_assistant", key, escaped}, {"ai_assistant", key + "_" + g_ai_assistant.provider, escaped}}))
            return false;
        auto &slots = key == "endpoint" ? g_ai_assistant.endpoints : g_ai_assistant.models;
        slots[g_ai_assistant.provider] = value;
        (key == "endpoint" ? g_ai_assistant.endpoint : g_ai_assistant.model) = value;
        return true;
    }
    const auto persist = [](const std::string &toml_key, const std::string &toml_value, std::string &target) {
        if (!WriteConfiguredValue("ai_assistant", toml_key, EscapeTomlBasicString(toml_value)))
            return false;
        target = toml_value;
        return true;
    };

    if (key.rfind("token_", 0) == 0)
    {
        const std::string provider = key.substr(std::string("token_").size());
        if (AiAssistantTokenSlotKey(provider) != key)
            return false;
        if (!WriteConfiguredValue("ai_assistant", key, EscapeTomlBasicString(value)))
            return false;
        const std::string id = VoiceInput::NormalizeProviderId(provider);
        g_ai_assistant.tokens[id] = value;
        if (g_ai_assistant.provider == id)
            persist("token", value, g_ai_assistant.token);
        return true;
    }

    std::string *target = nullptr;
    if (key == "token")
        target = &g_ai_assistant.token;
    else if (key == "prompt_id")
        target = &g_ai_assistant.prompt_id;
    else if (key == "prompt_custom_1")
        target = &g_ai_assistant.prompt_custom_1;
    else if (key == "prompt_custom_2")
        target = &g_ai_assistant.prompt_custom_2;
    else if (key == "prompt_custom_3")
        target = &g_ai_assistant.prompt_custom_3;
    else if (key == "prompt")
        target = &g_ai_assistant.prompt;
    if (!target || !WriteConfiguredValue("ai_assistant", key, EscapeTomlBasicString(value)))
        return false;
    *target = value;
    if (key == "prompt_custom_1")
        WriteConfiguredValue("ai_assistant", "prompt", EscapeTomlBasicString(""));
    if (key == "prompt")
        g_ai_assistant.prompt_custom_1 = value;
    if (key == "prompt" || key == "prompt_id" || key.rfind("prompt_custom_", 0) == 0)
    {
        g_ai_assistant.prompt = g_ai_assistant.prompt_id == "custom_2"   ? g_ai_assistant.prompt_custom_2
                                : g_ai_assistant.prompt_id == "custom_3" ? g_ai_assistant.prompt_custom_3
                                                                         : g_ai_assistant.prompt_custom_1;
    }
    if (key == "token")
    {
        const std::string slot = AiAssistantTokenSlotKey(g_ai_assistant.provider);
        g_ai_assistant.tokens[g_ai_assistant.provider] = value;
        WriteConfiguredValue("ai_assistant", slot, EscapeTomlBasicString(value));
    }
    return true;
}

bool SetConfiguredAiAssistantBool(const std::string &key, bool value)
{
    if (key != "enabled" || !WriteConfiguredValue("ai_assistant", key, value ? "true" : "false"))
        return false;
    g_ai_assistant.enabled = value;
    return true;
}

bool SetConfiguredAiAssistantInt(const std::string &key, int value)
{
    if (key != "candidate_limit" || value < 1 || value > 10 ||
        !WriteConfiguredValue("ai_assistant", key, std::to_string(value)))
        return false;
    g_ai_assistant.candidate_limit = value;
    return true;
}
