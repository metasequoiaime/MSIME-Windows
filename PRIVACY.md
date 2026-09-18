# Privacy

Metasequoia IME for Windows converts keystrokes, composition text and candidates locally, using the shared C++ engine and the SQLite dictionaries installed under `%LOCALAPPDATA%\metasequoiaime\`. Local conversion never contacts the network. This document describes the optional online features that do, exactly what each one sends, how to turn each one off, and what the input method records on this machine.

Every claim below is checkable against this repository. Where a default matters, the file and key that set it are named.

## Network features

**Cloud candidates are enabled by default, and the installer asks before the first install.** `general.cloud_candidates` is `true` in the shipped `installer/default_config/config.default.toml`, and `server/src/config/ime_config.cpp` also defaults to `true` when the key is absent. A first install shows a wizard page naming this feature and its endpoint, with the box checked; clearing it writes `cloud_candidates = false` into the config that install creates. Upgrades skip that page, because by then the config belongs to the user. While a composition is active, the typed spelling is sent over HTTPS to Google's input-tools service (`https://inputtools.google.com/request`) to fetch one extra candidate. Google receives that spelling, the IP address and standard request metadata. No committed text, dictionary content, learned frequencies, settings or account data are sent. This is the one online feature that needs no credential, so it is the one that is actually live on a fresh install. Turn it off in 设置 → 输入 → 云候选, or set `cloud_candidates = false` under `[general]` in `%LOCALAPPDATA%\metasequoiaime\config.toml`.

**AI candidate suggestions require a token you supply.** `[ai_assistant] enabled` ships as `true`, but the shipped token values are placeholders and the runtime rejects placeholders, so no request is made until a real token is entered. Once configured, the segmented spelling and the surrounding composition context are sent to the provider's Chat Completions endpoint — DeepSeek, OpenAI, SiliconFlow or Groq, per `[ai_assistant] provider`. That provider's own privacy terms then apply. Set `enabled = false` to disable it outright.

**Candidate translation requires credentials you supply.** `[tencent_tmt] enabled` ships as `true` with placeholder credentials, so nothing is sent until real Tencent Cloud keys are entered. Once configured, candidate text that the local dictionaries could not gloss is sent to `https://tmt.tencentcloudapi.com`. Translations are display metadata and never change the text that is committed.

**Voice input requires a token you supply.** `[voice_input] voice_input` ships as `true` with a placeholder ASR token, so the hotkeys do nothing over the network until a real token is entered. Once configured, recorded audio is uploaded to the configured endpoint — Doubao (`wss://openspeech.bytedance.com/...`), OpenAI, SiliconFlow or Groq. If text polishing is enabled, the returned transcript is additionally sent to the configured Chat Completions endpoint.

**The update check runs only when you press the button.** 设置 → 关于 → 检查更新 fetches `https://msime.app/update.json` and compares the version. There is no background or scheduled update check, and the request carries no identifier beyond the IP address and a cache-busting timestamp.

## No telemetry

There is no analytics, telemetry or crash reporting of any kind, and no measured usage data ever leaves the machine. No third-party SDK for those purposes is linked, and there is no endpoint that receives usage data. This is verifiable: searching the `server/`, `windows/`, `ui/`, `ui-html/` and `installer/` trees for `telemetry`, `analytics`, `sentry`, `matomo`, `posthog`, `amplitude`, `mixpanel`, `crashpad` and `breakpad` returns nothing.

The one thing that measures how much you type is the input statistics described in the next section. They are counted and stored on this machine, they are never transmitted, and they keep no record of what was typed.

## Input statistics

Enabled by default. A first install shows a wizard page of its own naming this feature, with the box checked; clearing it writes `enabled = false` under `[statistics]` into the config that install creates. Upgrades skip that page rather than override your config.

The input method counts characters as they are committed to the focused application and keeps the totals in `%LOCALAPPDATA%\metasequoiaime\msime_stats.db`:

- the local date and the hour bucket;
- five counters per day — Chinese, English letters, digits, punctuation, and everything else;
- how much of that hour was spent actively typing. Gaps between commits of five seconds or less count as active typing; a longer gap is treated as a pause and adds no time.

It stores no typed content. The committed text, the spelling being composed, the candidates on screen, the application name and the window title are all absent from it: the text is classified inside the input method process, and only those five counters cross into the rest of the program. Nothing is uploaded either — this module makes no network request and is not wired into any feature that does. Reading the database in full tells you how many characters were typed on which day, in which hour and in which category, and nothing else.

Turn it off with the switch in 设置 → 统计, or by setting `enabled = false` under `[statistics]` in `%LOCALAPPDATA%\metasequoiaime\config.toml`. Turning it off stops new records and keeps the ones already stored. The same page trims records by age. It can keep only the most recent 30 days, 3 months, 6 months or 1 year and drop everything older, or keep everything — the retention window defaults to "keep everything", which trims nothing. Trimming is immediate and not reversible.

## Local data

Settings live in `%LOCALAPPDATA%\metasequoiaime\config.toml`. Learned word frequencies and user-created words are stored in `%LOCALAPPDATA%\metasequoiaime\msime_user.db`. Input statistics live in `%LOCALAPPDATA%\metasequoiaime\msime_stats.db` and hold counters only; the section above lists what is in them. Diagnostic logs are written to `%LOCALAPPDATA%\metasequoiaime\log\` and record process lifecycle and error conditions, not typed content. Clipboard history is disabled by default (`clipboard_history = false`); when enabled it records copied text locally and is cleared as soon as it is switched off.

**API tokens are stored in plain text in `config.toml`.** This differs from the other platforms — macOS and iOS use the Keychain, and Linux uses the desktop Secret Service — and it means anything able to read that file can read the tokens. Use provider tokens scoped to this purpose, and treat the file as a secret when backing up or syncing the profile directory.

All of these files sit in the current user's profile and are protected only by that account's permissions.

## Uninstall

Uninstalling deletes `%LOCALAPPDATA%\metasequoiaime` and `%PROGRAMDATA%\metasequoiaime` in full, including the user dictionary, settings, the input statistics database and any installed skins. Back up `msime_user.db` first if you want to keep learned words. An in-place upgrade behaves differently: it preserves `msime_user.db`, `msime_stats.db`, `config.toml`, `config.base.toml` and the `skins` directory, and replaces everything else.

## General

Metasequoia IME does not sell or share personal data, and installing or using it does not create an account. If a future release adds another network-backed feature, this notice must be updated before that feature ships.

Security or privacy concerns should be reported privately as described in the organisation [SECURITY.md](https://github.com/metasequoiaime/.github/blob/main/SECURITY.md).
