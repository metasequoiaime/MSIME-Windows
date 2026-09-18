# Metasequoia IME for Windows

[中文 README](README.md) · [Website](https://msime.app) · [Docs](https://msime.app/docs/) · [Privacy](PRIVACY.md)

<!-- badges:start -->
[![CI](https://img.shields.io/github/actions/workflow/status/metasequoiaime/MSIME-Windows/ci.yml?branch=develop&label=CI)](https://github.com/metasequoiaime/MSIME-Windows/actions/workflows/ci.yml)
[![CodeQL](https://img.shields.io/github/actions/workflow/status/metasequoiaime/MSIME-Windows/codeql.yml?branch=develop&label=CodeQL)](https://github.com/metasequoiaime/MSIME-Windows/actions/workflows/codeql.yml)
[![Release](https://img.shields.io/github/v/release/metasequoiaime/MSIME-Windows?include_prereleases&label=release)](https://github.com/metasequoiaime/MSIME-Windows/releases)
[![Downloads](https://img.shields.io/github/downloads/metasequoiaime/MSIME-Windows/total?label=downloads)](https://github.com/metasequoiaime/MSIME-Windows/releases)
[![License](https://img.shields.io/github/license/metasequoiaime/MSIME-Windows)](LICENSE)
[![Stars](https://img.shields.io/github/stars/metasequoiaime/MSIME-Windows?style=flat)](https://github.com/metasequoiaime/MSIME-Windows/stargazers)
<!-- badges:end -->

A free and open-source Chinese and Japanese input method for Windows 10 and 11, built on TSF with no legacy IMM32 path.

This repository is the whole Windows product: the TSF text service DLL, the resident server, the conversion engine, a self-written native GUI framework, the HTML surfaces and the installer. The engine was imported from [MSIME-Engine](https://github.com/metasequoiaime/MSIME-Engine) and specialized for Windows; it now lives in `engine/` as a first-party component, so a plain clone builds the product. See [`engine/UPSTREAM.md`](engine/UPSTREAM.md).

**This is a public beta.** Expect rough edges and read the release notes before upgrading.

## Install

Download the installer from [Releases](https://github.com/metasequoiaime/MSIME-Windows/releases) or the [download page](https://msime.app/download/), run it, then switch with `Win + Space`.

Current builds are **not code signed**. SmartScreen will block them and uiAccess is unavailable, which means the candidate window cannot float above elevated applications. Verify the download with the SHA256 published on the download page.

You need the **Microsoft Visual C++ 2015–2022 Redistributable (x64)**. The server and settings app are 64-bit; the x86 redistributable is a separate package and does not substitute for it. If you cannot switch to the IME, the server keeps exiting, or the settings window closes immediately, install that first.

## What it does

- Chinese input: full pinyin, double pinyin (Xiaohe / Ziranma / Shoudao / Microsoft), Wubi 86
- Japanese input: romaji, with hiragana, katakana and dictionary candidates; also available as a temporary mode from Chinese
- Helpcode (形码) filtering on pinyin schemes: five tables, single- and double-code
- Optional cloud candidates, and AI suggestions via DeepSeek / OpenAI / SiliconFlow / Groq
- English-Chinese glosses in the vertical candidate window
- Mixed Chinese-English input, emoji and kaomoji, a dedicated English candidate mode
- Voice input with streaming recognition and optional cleanup
- Handwriting, on-screen keyboard, floating toolbar, clipboard history
- User dictionary management, candidate frequency tuning, skins with light and dark variants

## Privacy

An input method sees every keystroke, so the boundaries are stated explicitly in [PRIVACY.md](PRIVACY.md): which features reach the network, what each one sends, what the defaults are, and how to turn each off.

Three things worth knowing up front. **Cloud candidates are on by default** — the composition spelling goes to Google's input-tools service, and it is the only network feature that works without you supplying a credential. **API tokens are stored in plain text** in `config.toml`, unlike macOS and Linux which use the Keychain and Secret Service. **Input statistics are on by default** — they count characters as they are committed and keep the running totals on this machine; the first install shows a page of its own before enabling them, and 设置 → 统计 turns them off and trims them by age.

There is no telemetry, analytics or crash reporting of any kind, and no measured usage data ever leaves the machine.

## Building and contributing

[CONTRIBUTING.md](CONTRIBUTING.md) has the environment, the build order and what to run before opening a pull request. Contributions do not have to be code — dictionary entries, documentation, icons and compatibility testing all count, and the [recruiting page](https://github.com/metasequoiaime/.github/blob/main/RECRUITING.md) lists them by area.

Note that most issues, documentation and code comments are in Chinese. English pull requests and issues are welcome.

## Licence

GPL-3.0. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
