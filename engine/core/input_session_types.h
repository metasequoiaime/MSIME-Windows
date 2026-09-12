#pragma once
#include "scheme_type.h"
#include "word_item.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace metasequoia
{
// Editing intents a platform frontend maps its own key events onto. Printable input goes through
// handle_character instead, so this covers only the commands that act on an existing composition.
enum class Command
{
    Backspace,
    CommitCandidate,
    CommitRaw,
    Cancel,
    MoveLeft,
    MoveRight,
    MoveHome,
    MoveEnd,
    DeleteForward,
};

// Outcome of one dispatched key or selection. `handled` tells the frontend whether to swallow the
// event, and `commit` carries the text the frontend should insert into the client application.
struct KeyResult
{
    bool handled = false;
    std::optional<std::string> commit;
    std::optional<std::string> diagnostic;
};

enum class CandidateEdge
{
    FirstHan,
    LastHan,
};

enum class FrequencyAdjustmentMode
{
    Disabled,
    Pin,
    Halve,
    Linear,
    Promote,
};

struct FrequencyAdjustmentOptions
{
    FrequencyAdjustmentMode mode = FrequencyAdjustmentMode::Disabled;
    int trigger_count = 1;
    int linear_step = 1;
};

enum class LocalInputMode
{
    None,
    Unicode,
    DateTime,
    QuickPhrase,
    Emoji,
    Kaomoji,
    SuperJianpin,
    TemporaryEnglish,
    TemporaryJapanese,
    Number,
};

struct LocalModeOptions
{
    bool unicode = true;
    bool date_time = true;
    bool quick_phrase = true;
    bool emoji = true;
    bool kaomoji = true;
    bool super_jianpin = true;
    bool temporary_english = true;
    bool temporary_japanese = true;
    bool number = true;
};

struct EnglishInputOptions
{
    bool mixed_candidates = false;
    std::size_t minimum_prefix = 2;
};

struct MixedExpressiveOptions
{
    bool emoji_candidates = false;
    bool kaomoji_candidates = false;
};

struct WubiInputOptions
{
    // Answer an unmatched wubi code with quanpin candidates for the same letters. A code that the
    // table does know keeps its own candidates untouched, so this only ever appears where nothing
    // could be typed at all, and a fluent wubi typist never sees it.
    bool mixed_pinyin = false;
};

// Immutable description of the current composition for asynchronous providers. Frontends copy
// this value into a request and return it unchanged with the result; InputSession revalidates it
// against the live composition before changing candidates.
struct OnlineQuery
{
    SchemeType scheme = SchemeType::Quanpin;
    std::uint64_t generation = 0;
    std::string identity;
    std::string query_text;
    std::string cache_key;
    std::vector<std::string> pinyin_segments;
    bool cloud_eligible = false;
    bool ai_eligible = false;
    // Appended to retain source compatibility with older aggregate initializers.
    std::uint64_t session_id = 0;
};

} // namespace metasequoia
