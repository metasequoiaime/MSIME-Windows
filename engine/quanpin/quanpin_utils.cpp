#include "quanpin_utils.h"

#include "autocorrect_table.h"
#include "../common/helpcode_utils.h"
#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace quanpin
{
namespace
{
struct SparsePinyinFallbackEntry
{
    Segments replacement;
    bool append_suffix = true;
};

struct SparsePinyinFallbackRule
{
    const char *full;
    std::vector<SparsePinyinFallbackEntry> replacements;
};

std::vector<std::string> split(const std::string &text, char delimiter)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true)
    {
        const size_t pos = text.find(delimiter, start);
        if (pos == std::string::npos)
        {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
}

bool is_complete_pinyin_part(const std::string &part)
{
    if (part.empty())
    {
        return false;
    }

    return !cut_one_piece_greedy(part, true).empty();
}

Segments append_rest(const Segments &head, const Segments &segments)
{
    Segments combined = head;
    if (segments.size() > 1)
    {
        combined.insert(combined.end(), segments.begin() + 1, segments.end());
    }
    return combined;
}

const std::vector<SparsePinyinFallbackRule> &sparse_pinyin_fallback_rules()
{
    static const std::vector<SparsePinyinFallbackRule> kRules = {
        {"dia", {{Segments{"di", "a"}, true}, {Segments{"di"}, false}}},
        {"biang", {{Segments{"bi", "ang"}, true}, {Segments{"bi"}, false}}},
        {"gei", {{Segments{"ge"}, false}}},
        {"yo", {{Segments{"y"}, false}}},
    };
    return kRules;
}

} // namespace

const std::vector<std::string> &intact_pinyin_list()
{
    static const std::vector<std::string> kList = {
        "a",     "ai",     "an",    "ang",   "ao",    "ba",    "bai",   "ban",   "bang",  "bao",    "bei",   "ben",
        "beng",  "bi",     "bian",  "biang", "biao",  "bie",   "bin",   "bing",  "bo",    "bu",     "ca",    "cai",
        "can",   "cang",   "cao",   "ce",    "cen",   "ceng",  "cha",   "chai",  "chan",  "chang",  "chao",  "che",
        "chen",  "cheng",  "chi",   "chong", "chou",  "chu",   "chua",  "chuai", "chuan", "chuang", "chui",  "chun",
        "chuo",  "ci",     "cong",  "cou",   "cu",    "cuan",  "cui",   "cun",   "cuo",   "da",     "dai",   "dan",
        "dang",  "dao",    "de",    "dei",   "den",   "deng",  "di",    "dia",   "dian",  "diao",   "die",   "ding",
        "diu",   "dong",   "dou",   "du",    "duan",  "dui",   "dun",   "duo",   "e",     "ei",     "en",    "er",
        "fa",    "fan",    "fang",  "fei",   "fen",   "feng",  "fiao",  "fo",    "fou",   "fu",     "ga",    "gai",
        "gan",   "gang",   "gao",   "ge",    "gei",   "gen",   "geng",  "gong",  "gou",   "gu",     "gua",   "guai",
        "guan",  "guang",  "gui",   "gun",   "guo",   "ha",    "hai",   "han",   "hang",  "hao",    "he",    "hei",
        "hen",   "heng",   "hong",  "hou",   "hu",    "hua",   "huai",  "huan",  "huang", "hui",    "hun",   "huo",
        "ji",    "jia",    "jian",  "jiang", "jiao",  "jie",   "jin",   "jing",  "jiong", "jiu",    "ju",    "juan",
        "jue",   "jun",    "jv",    "jve",   "ka",    "kai",   "kan",   "kang",  "kao",   "ke",     "kei",   "ken",
        "keng",  "kong",   "kou",   "ku",    "kua",   "kuai",  "kuan",  "kuang", "kui",   "kun",    "kuo",   "la",
        "lai",   "lan",    "lang",  "lao",   "le",    "lei",   "leng",  "li",    "lia",   "lian",   "liang", "liao",
        "lie",   "lin",    "ling",  "liu",   "lo",    "long",  "lou",   "lu",    "luan",  "lue",    "lun",   "luo",
        "lv",    "lve",    "ma",    "mai",   "man",   "mang",  "mao",   "me",    "mei",   "men",    "meng",  "mi",
        "mian",  "miao",   "mie",   "min",   "ming",  "miu",   "mo",    "mou",   "mu",    "na",     "nai",   "nan",
        "nang",  "nao",    "ne",    "nei",   "nen",   "neng",  "ni",    "nian",  "niang", "niao",   "nie",   "nin",
        "ning",  "niu",    "nong",  "nou",   "nu",    "nuan",  "nue",   "nun",   "nuo",   "nv",     "nve",   "o",
        "ou",    "pa",     "pai",   "pan",   "pang",  "pao",   "pei",   "pen",   "peng",  "pi",     "pian",  "piao",
        "pie",   "pin",    "ping",  "po",    "pou",   "pu",    "qi",    "qia",   "qian",  "qiang",  "qiao",  "qie",
        "qin",   "qing",   "qiong", "qiu",   "qu",    "quan",  "que",   "qun",   "qv",    "qve",    "ran",   "rang",
        "rao",   "re",     "ren",   "reng",  "ri",    "rong",  "rou",   "ru",    "ruan",  "rui",    "run",   "ruo",
        "sa",    "sai",    "san",   "sang",  "sao",   "se",    "sen",   "seng",  "sha",   "shai",   "shan",  "shang",
        "shao",  "she",    "shei",  "shen",  "sheng", "shi",   "shou",  "shu",   "shua",  "shuai",  "shuan", "shuang",
        "shui",  "shun",   "shuo",  "si",    "song",  "sou",   "su",    "suan",  "sui",   "sun",    "suo",   "ta",
        "tai",   "tan",    "tang",  "tao",   "te",    "teng",  "ti",    "tian",  "tiao",  "tie",    "ting",  "tong",
        "tou",   "tu",     "tuan",  "tui",   "tun",   "tuo",   "wa",    "wai",   "wan",   "wang",   "wei",   "wen",
        "weng",  "wo",     "wu",    "xi",    "xia",   "xian",  "xiang", "xiao",  "xie",   "xin",    "xing",  "xiong",
        "xiu",   "xu",     "xuan",  "xue",   "xun",   "xv",    "xve",   "ya",    "yan",   "yang",   "yao",   "ye",
        "yi",    "yin",    "ying",  "yo",    "yong",  "you",   "yu",    "yuan",  "yue",   "yun",    "yv",    "yve",
        "za",    "zai",    "zan",   "zang",  "zao",   "ze",    "zei",   "zen",   "zeng",  "zha",    "zhai",  "zhan",
        "zhang", "zhao",   "zhe",   "zhei",  "zhen",  "zheng", "zhi",   "zhong", "zhou",  "zhu",    "zhua",  "zhuai",
        "zhuan", "zhuang", "zhui",  "zhun",  "zhuo",  "zi",    "zong",  "zou",   "zu",    "zuan",   "zui",   "zun",
        "zuo"};
    return kList;
}

const std::unordered_set<std::string> &intact_pinyin_set()
{
    static const std::unordered_set<std::string> kSet(intact_pinyin_list().begin(), intact_pinyin_list().end());
    return kSet;
}

const std::unordered_set<std::string> &prefix_pinyin_set()
{
    static const std::unordered_set<std::string> kSet = [] {
        std::unordered_set<std::string> result;
        for (const auto &item : intact_pinyin_list())
        {
            for (size_t i = 1; i <= item.size(); ++i)
            {
                result.insert(item.substr(0, i));
            }
        }
        return result;
    }();
    return kSet;
}

bool has_only_complete_pinyin_segments(const Segments &segments)
{
    if (segments.empty())
    {
        return false;
    }

    const auto &valid_pinyin = intact_pinyin_set();
    return std::all_of(segments.begin(), segments.end(),
                       [&](const std::string &segment) { return valid_pinyin.find(segment) != valid_pinyin.end(); });
}

bool looks_like_syllable_with_jianpin_tail(const std::string &pinyin)
{
    // Manual delimiters express user-intent boundaries and never enter the
    // correction path, so there is nothing for this guard to protect.
    if (pinyin.empty() || pinyin.find('\'') != std::string::npos)
    {
        return false;
    }

    const auto &valid_pinyin = intact_pinyin_set();
    static const size_t kMaxSyllableLength =
        std::max_element(intact_pinyin_list().begin(), intact_pinyin_list().end(),
                         [](const std::string &lhs, const std::string &rhs) { return lhs.size() < rhs.size(); })
            ->size();

    // Greedy longest-match scan: the input must reduce to one or more legal
    // syllables plus at most one trailing letter ("zheg" = zhe + g) to count
    // as jianpin intent. All-consonant strings match zero syllables and
    // return false on purpose: the engine has no multi-letter jianpin, so
    // correction is the only useful reading of e.g. "bqng" -> bang.
    size_t pos = 0;
    while (pos < pinyin.size())
    {
        const size_t max_len = std::min(kMaxSyllableLength, pinyin.size() - pos);
        size_t matched = 0;
        for (size_t len = max_len; len >= 1; --len)
        {
            if (valid_pinyin.find(pinyin.substr(pos, len)) != valid_pinyin.end())
            {
                matched = len;
                break;
            }
        }
        if (matched == 0)
        {
            break;
        }
        pos += matched;
    }
    if (pos == 0 || pinyin.size() - pos > 1)
    {
        return false;
    }
    if (pinyin.size() == pos)
    {
        // Fully reduced to legal syllables: a legitimate spelling, not a typo.
        return true;
    }
    // Exactly one trailing letter. It reads as jianpin intent only when it is a
    // plausible jianpin initial -- an initial consonant, as in "zheg" = zhe + g.
    // A bare vowel can never begin a jianpin syllable, so a complete syllable
    // followed by a lone vowel ("gau" = ga + u) is far more likely a
    // transposition typo (gua) than jianpin intent; let it reach correction.
    const char tail = pinyin.back();
    const bool tail_is_vowel = tail == 'a' || tail == 'e' || tail == 'i' || tail == 'o' || tail == 'u' || tail == 'v';
    return !tail_is_vowel;
}

SyllableGraph build_syllable_graph(const std::string &pinyin)
{
    SyllableGraph graph;
    graph.input_length = pinyin.size();
    graph.edges.resize(pinyin.size() + 1);
    if (pinyin.empty() || pinyin.find('\'') != std::string::npos)
    {
        return graph;
    }

    const auto &valid_pinyin = intact_pinyin_set();
    static const size_t kMaxSyllableLength =
        std::max_element(intact_pinyin_list().begin(), intact_pinyin_list().end(),
                         [](const std::string &lhs, const std::string &rhs) { return lhs.size() < rhs.size(); })
            ->size();

    for (size_t start = 0; start < pinyin.size(); ++start)
    {
        const size_t last_end = std::min(pinyin.size(), start + kMaxSyllableLength);
        for (size_t end = last_end; end > start; --end)
        {
            const std::string syllable = pinyin.substr(start, end - start);
            if (valid_pinyin.find(syllable) != valid_pinyin.end())
            {
                graph.edges[start].push_back(SyllableEdge{end, syllable});
            }
        }
    }

    std::vector<bool> reaches_end(pinyin.size() + 1, false);
    reaches_end[pinyin.size()] = true;
    for (size_t start = pinyin.size(); start-- > 0;)
    {
        auto &edges = graph.edges[start];
        edges.erase(std::remove_if(
                        edges.begin(), edges.end(),
                        [&](const SyllableEdge &edge) { return edge.end > pinyin.size() || !reaches_end[edge.end]; }),
                    edges.end());
        reaches_end[start] = !edges.empty();
    }
    return graph;
}

std::vector<Segments> enumerate_complete_segmentations(const SyllableGraph &graph, size_t path_limit)
{
    std::vector<Segments> result;
    if (path_limit == 0 || graph.input_length == 0 || graph.edges.size() != graph.input_length + 1)
    {
        return result;
    }

    Segments current;
    const auto enumerate = [&](auto &&self, size_t position) -> void {
        if (result.size() >= path_limit)
        {
            return;
        }
        if (position == graph.input_length)
        {
            result.push_back(current);
            return;
        }
        if (position >= graph.edges.size())
        {
            return;
        }

        for (const auto &edge : graph.edges[position])
        {
            current.push_back(edge.syllable);
            self(self, edge.end);
            current.pop_back();
            if (result.size() >= path_limit)
            {
                return;
            }
        }
    };
    enumerate(enumerate, 0);
    return result;
}

std::vector<std::string> cut_one_piece_greedy(const std::string &pinyin, bool intact_only)
{
    const auto &pinyin_set = intact_only ? intact_pinyin_set() : prefix_pinyin_set();
    std::vector<std::string> result;
    size_t index = 0;
    while (index < pinyin.size())
    {
        std::string matched;
        for (size_t end = pinyin.size(); end > index; --end)
        {
            const auto piece = pinyin.substr(index, end - index);
            if (pinyin_set.find(piece) != pinyin_set.end())
            {
                matched = piece;
                break;
            }
        }
        if (matched.empty())
        {
            return {};
        }
        result.push_back(matched);
        index += matched.size();
    }
    return result;
}

std::vector<std::string> cut_one_piece_min_segments(const std::string &pinyin, bool intact_only)
{
    const auto &pinyin_set = intact_only ? intact_pinyin_set() : prefix_pinyin_set();
    std::unordered_map<size_t, std::vector<std::string>> memo;
    std::unordered_set<size_t> visiting;

    const auto solve = [&](auto &&self, size_t index) -> std::vector<std::string> {
        if (index == pinyin.size())
        {
            return {};
        }

        if (const auto found = memo.find(index); found != memo.end())
        {
            return found->second;
        }

        if (!visiting.insert(index).second)
        {
            return {};
        }

        std::vector<std::string> best;
        bool has_best = false;

        for (size_t end = pinyin.size(); end > index; --end)
        {
            const auto piece = pinyin.substr(index, end - index);
            if (pinyin_set.find(piece) == pinyin_set.end())
            {
                continue;
            }

            std::vector<std::string> suffix;
            if (end < pinyin.size())
            {
                suffix = self(self, end);
                if (suffix.empty())
                {
                    continue;
                }
            }

            std::vector<std::string> candidate;
            candidate.reserve(1 + suffix.size());
            candidate.push_back(piece);
            candidate.insert(candidate.end(), suffix.begin(), suffix.end());

            if (!has_best || candidate.size() < best.size())
            {
                best = std::move(candidate);
                has_best = true;
                continue;
            }

            if (candidate.size() == best.size() && !candidate.empty() && !best.empty() &&
                candidate.front().size() < best.front().size())
            {
                best = std::move(candidate);
            }
        }

        visiting.erase(index);
        memo.emplace(index, has_best ? best : std::vector<std::string>{});
        return has_best ? best : std::vector<std::string>{};
    };

    return solve(solve, 0);
}

bool is_complete_pinyin_input(const std::string &pinyin)
{
    if (pinyin.empty())
    {
        return false;
    }

    if (pinyin.find('\'') == std::string::npos)
    {
        return is_complete_pinyin_part(pinyin);
    }

    for (const auto &part : split(pinyin, '\''))
    {
        if (!is_complete_pinyin_part(part))
        {
            return false;
        }
    }

    return true;
}

std::string to_google_spelling(const std::string &segmentation)
{
    // quanpin_query.cpp 的 canonical_syllable 把 ü 的两种写法归到词库存的那一种
    // （l/n 后面写 v：lve、nve）。Google 的两个解码器要的正好是另一种：本地
    // googlepinyinime-rev 的音节表来自 rawdict_utf16_65105_freq.txt，里面只有
    // nue/lue 和 nv/lv，没有 nve/lve，splparser 也不做 v→ü 转写；inputtools 云输入
    // 表现一致。把 nve'dai'dong'wu 原样送过去，nve 会被拆成 nv + e，「虐待动物」
    // 于是变成「女蛾黛动物」。j/q/x/y 后面它们同样只认 u 写法（ju、jue）。
    //
    // 按音节整体替换，认不出的原样透传：手打分隔符切出来的块可能不止一个音节，
    // 那种块交给解码器自己再切。
    static const std::unordered_map<std::string, std::string> kSpellings = {
        {"jv", "ju"},   {"qv", "qu"},   {"xv", "xu"},   {"yv", "yu"},   {"jve", "jue"},
        {"qve", "que"}, {"xve", "xue"}, {"yve", "yue"}, {"lve", "lue"}, {"nve", "nue"},
    };

    std::string result;
    result.reserve(segmentation.size());
    size_t chunk_start = 0;
    while (true)
    {
        const size_t separator = segmentation.find('\'', chunk_start);
        const size_t chunk_end = separator == std::string::npos ? segmentation.size() : separator;
        const std::string chunk = segmentation.substr(chunk_start, chunk_end - chunk_start);
        const auto found = kSpellings.find(chunk);
        result += found == kSpellings.end() ? chunk : found->second;
        if (separator == std::string::npos)
        {
            break;
        }
        result += '\'';
        chunk_start = separator + 1;
    }
    return result;
}

size_t detect_active_helpcode_length(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    const auto &input_with_cases = raw_input_with_cases.empty() ? raw_input : raw_input_with_cases;
    if (HelpcodeUtils::is_quanpin_double_help_mode(input_with_cases) && raw_input.size() >= 2 &&
        is_complete_pinyin_input(raw_input.substr(0, raw_input.size() - 2)))
    {
        return 2;
    }

    if (HelpcodeUtils::is_quanpin_single_help_mode(input_with_cases) && !raw_input.empty() &&
        is_complete_pinyin_input(raw_input.substr(0, raw_input.size() - 1)))
    {
        return 1;
    }

    return 0;
}

std::string strip_active_helpcodes(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    const size_t helpcode_length = detect_active_helpcode_length(raw_input, raw_input_with_cases);
    if (helpcode_length == 0 || raw_input.size() < helpcode_length)
    {
        return raw_input;
    }
    return raw_input.substr(0, raw_input.size() - helpcode_length);
}

std::string strip_active_helpcodes_with_cases(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    const auto &input_with_cases = raw_input_with_cases.empty() ? raw_input : raw_input_with_cases;
    const size_t helpcode_length = detect_active_helpcode_length(raw_input, raw_input_with_cases);
    if (helpcode_length == 0 || input_with_cases.size() < helpcode_length)
    {
        return input_with_cases;
    }
    return input_with_cases.substr(0, input_with_cases.size() - helpcode_length);
}

std::vector<Segments> sparse_pinyin_fallback_segments(const Segments &segments)
{
    if (segments.empty())
    {
        return {};
    }

    const auto &first = segments.front();
    for (const auto &rule : sparse_pinyin_fallback_rules())
    {
        if (first != rule.full)
        {
            continue;
        }

        std::vector<Segments> fallbacks;
        fallbacks.reserve(rule.replacements.size());
        for (const auto &replacement : rule.replacements)
        {
            fallbacks.push_back(replacement.append_suffix ? append_rest(replacement.replacement, segments)
                                                          : replacement.replacement);
        }
        return fallbacks;
    }

    return {};
}

namespace
{
constexpr size_t kMaxAutocorrectEdges = 3;
constexpr size_t kMaxAutocorrectInputLength = 64;
constexpr size_t kNoPredecessor = static_cast<size_t>(-1);

// Keys point at string literals in the generated tables (static storage), so views
// stay valid forever; Entry::correct is a 16-bit index into the generated
// kCorrectSyllables table, dereferenced once here so everything downstream
// (ranking, merging, queries) keeps plain string views. One wrong key maps to
// every correction target the tables offer: ambiguous variants are kept on purpose so the query layer can settle
// them with word frequency (CN 101133411 B M3). Tables are merged in weight
// order (transposition, deletion, insertion, neighbor), so target lists end up
// weight-sorted with array order breaking ties -- the deterministic "table
// order" of the ranking key.
struct CorrectionTarget
{
    std::string_view syllable;
    // OR of every correction type whose table offers this (wrong -> syllable)
    // pair. The tie-break weight is derived from the ENABLED subset at query
    // time (see min_enabled_correction_weight), never from a switched-off table.
    unsigned type_bit = 0;
    // Out-of-table generated pair (non-neighbor substitution / arbitrary-letter
    // insertion): flat expensive-tier weight (kAutocorrectGeneratedShapeWeight)
    // instead of the per-bit derivation; switch gating still uses type_bit.
    bool generated = false;
};

// Canonical per-type tie-break cost. Each correction type carries one fixed
// weight (header constants); this maps a single type bit back to it.
constexpr int correction_type_weight(const unsigned type_bit)
{
    switch (type_bit)
    {
    case kAutocorrectTransposition:
        return kAutocorrectTranspositionWeight;
    case kAutocorrectDeletion:
        return kAutocorrectDeletionWeight;
    case kAutocorrectInsertion:
        return kAutocorrectInsertionWeight;
    case kAutocorrectNeighbor:
        return kAutocorrectNeighborWeight;
    default:
        return 0;
    }
}

// Cheapest explanation among the correction types that are BOTH offered by a
// target and enabled by the caller. A pair shared by two tables is ranked by
// the min weight of the enabled ones only, so disabling a cheaper table lets
// the surviving (dearer) table set the cost instead of leaking the off table's.
int min_enabled_correction_weight(const unsigned enabled_bits)
{
    int best = std::numeric_limits<int>::max();
    for (const unsigned bit :
         {kAutocorrectTransposition, kAutocorrectDeletion, kAutocorrectInsertion, kAutocorrectNeighbor})
    {
        if (enabled_bits & bit)
        {
            best = std::min(best, correction_type_weight(bit));
        }
    }
    return best;
}

// QWERTY letter-key neighbors (finger-movement range, letter keys only), the
// same adjacency as server/scripts/generate_quanpin_autocorrect.py:42-69.
// Indexed by letter - 'a' (a..z order -- NOT keyboard order). Drift is
// self-healing only in one direction: a pair misclassified as "non-neighbor"
// that actually collides with the static table falls back to the cheaper
// static weight, but a neighbor misclassified as "far" simply skips
// generation -- so keep this table byte-identical to the generator's.
constexpr std::array<std::string_view, 26> kQwertyNeighbors = {
    "qwsz",   // a
    "vghn",   // b
    "xdfv",   // c
    "serfcx", // d
    "wrsd",   // e
    "drtgvc", // f
    "ftyhbv", // g
    "gyujbn", // h
    "uojk",   // i
    "huikmn", // j
    "jiolm",  // k
    "kop",    // l
    "njk",    // m
    "bhjm",   // n
    "ipkl",   // o
    "ol",     // p
    "wa",     // q
    "etdf",   // r
    "awdexz", // s
    "ryfg",   // t
    "yihj",   // u
    "cfgb",   // v
    "qeas",   // w
    "zsdc",   // x
    "tugh",   // y
    "asx",    // z
};

constexpr bool is_qwerty_neighbor(char key, char of_letter)
{
    const unsigned row = static_cast<unsigned>(of_letter - 'a');
    return row < kQwertyNeighbors.size() && kQwertyNeighbors[row].find(key) != std::string_view::npos;
}

// One out-of-table generated pair: wrong-form string plus the legal syllable it
// stands for. wrong is owned by the generated store (static lifetime) because
// the correction index refers to it through string_views.
struct GeneratedPair
{
    std::string wrong;
    std::string_view syllable;
    unsigned type_bit;
};

// Enumerate the out-of-table shapes over every legal syllable: non-neighbor
// substitutions and arbitrary-letter insertions. Transpositions and deletions
// are exhaustive in the static tables -- nothing to add. Wrong forms must be
// non-legal, at least three letters (shorter strings belong to the jianpin
// space) and at most six (the k-best piece scan never looks at longer pieces) --
// the same window as the static tables and the evaluation filter.
std::vector<GeneratedPair> build_generated_pairs()
{
    const auto &legal = intact_pinyin_set();
    std::vector<GeneratedPair> pairs;
    for (const std::string &syllable : intact_pinyin_list())
    {
        if (syllable.size() < 3)
        {
            continue; // same-length substitutions would violate the 3-letter floor
        }
        // 非相邻键替换：换上的字母既不是原字母也不是它的 QWERTY 邻键。
        for (size_t i = 0; i < syllable.size(); ++i)
        {
            for (char key = 'a'; key <= 'z'; ++key)
            {
                if (key == syllable[i] || is_qwerty_neighbor(key, syllable[i]))
                {
                    continue;
                }
                std::string wrong = syllable;
                wrong[i] = key;
                if (legal.find(wrong) != legal.end())
                {
                    continue;
                }
                pairs.push_back({std::move(wrong), syllable, kAutocorrectNeighbor});
            }
        }
        // 任意字母插入：插入字母不属于静态覆盖集（该位相邻字母及其邻键）。
        for (size_t i = 0; i <= syllable.size(); ++i)
        {
            const char left = i > 0 ? syllable[i - 1] : '\0';
            const char right = i < syllable.size() ? syllable[i] : '\0';
            for (char key = 'a'; key <= 'z'; ++key)
            {
                const bool statically_covered = key == left || key == right ||
                                                (left != '\0' && is_qwerty_neighbor(key, left)) ||
                                                (right != '\0' && is_qwerty_neighbor(key, right));
                if (statically_covered)
                {
                    continue;
                }
                std::string wrong = syllable;
                wrong.insert(i, 1, key);
                if (wrong.size() > 6 || legal.find(wrong) != legal.end())
                {
                    continue; // 7 letters sit outside the k-best piece scan window
                }
                pairs.push_back({std::move(wrong), syllable, kAutocorrectInsertion});
            }
        }
    }
    return pairs;
}

// Static lifetime: the correction index refers to `wrong` through string_views.
const std::vector<GeneratedPair> &generated_pairs()
{
    static const std::vector<GeneratedPair> pairs = build_generated_pairs();
    return pairs;
}

const std::unordered_map<std::string_view, std::vector<CorrectionTarget>> &correction_index()
{
    static const std::unordered_map<std::string_view, std::vector<CorrectionTarget>> kIndex = [] {
        std::unordered_map<std::string_view, std::vector<CorrectionTarget>> index;
        index.reserve((autocorrect::kTranspositionCount + autocorrect::kNeighborCount + autocorrect::kDeletionCount +
                       autocorrect::kInsertionCount) *
                      4 / 3);
        const auto add_table = [&](const autocorrect::Entry *entries, const std::size_t count,
                                   const unsigned type_bit) {
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::string_view correct = autocorrect::kCorrectSyllables[entries[i].correct];
                auto &targets = index[entries[i].wrong];
                // The generator guarantees (wrong, correct) pairs are unique
                // across tables; the merge below only defends against drift.
                // Only the type bits accumulate -- the tie-break weight is
                // recomputed from the enabled subset at query time, so a shared
                // pair never inherits a switched-off table's cheaper cost.
                const auto duplicate =
                    std::find_if(targets.begin(), targets.end(),
                                 [&](const CorrectionTarget &target) { return target.syllable == correct; });
                if (duplicate != targets.end())
                {
                    duplicate->type_bit |= type_bit;
                }
                else
                {
                    targets.push_back(CorrectionTarget{correct, type_bit});
                }
            }
        };
        add_table(autocorrect::kTranspositionEntries, autocorrect::kTranspositionCount, kAutocorrectTransposition);
        add_table(autocorrect::kDeletionEntries, autocorrect::kDeletionCount, kAutocorrectDeletion);
        add_table(autocorrect::kInsertionEntries, autocorrect::kInsertionCount, kAutocorrectInsertion);
        add_table(autocorrect::kNeighborEntries, autocorrect::kNeighborCount, kAutocorrectNeighbor);
        // Out-of-table generated shapes: appended AFTER the static tables so a
        // shared wrong key keeps its cheaper static targets first. A generated
        // pair colliding with a static pair is dropped -- the static table's
        // cheaper weight already covers it (self-heals engine/generator drift).
        for (const auto &pair : generated_pairs())
        {
            auto &targets = index[pair.wrong];
            const auto duplicate = std::find_if(targets.begin(), targets.end(), [&](const CorrectionTarget &target) {
                return target.syllable == pair.syllable;
            });
            if (duplicate == targets.end())
            {
                targets.push_back(CorrectionTarget{pair.syllable, pair.type_bit, true});
            }
        }
        return index;
    }();
    return kIndex;
}

// Edge taken to arrive at a hypothesis position. raw_length is the number of
// input letters consumed: equal to syllable.size() for legal syllables and
// same-length corrections (transposition / neighbor), one less for deletion
// corrections ("zhng" -> zhang), one more for insertion corrections
// ("shangg" -> shang).
struct AutocorrectEdge
{
    size_t raw_length = 0;
    std::string_view syllable;
    bool corrected = false;
    // Out-of-table generated pair (non-neighbor substitution / arbitrary-letter
    // insertion). Generated-containing hypotheses rank behind ALL static
    // hypotheses (see the finalize comparator) so in-table inputs keep their
    // baseline cut set.
    bool generated = false;
};

// One propagated cut hypothesis, ranked by (edge_count, weight, arrival):
// fewest corrections first (the least-intrusive contract), then the summed
// correction weights (only within the same edge count), then the deterministic
// generation order -- positions ascending, pieces longest-first, table order.
// prev_index points back into the predecessor position's final hypothesis
// list, which is frozen once finalized, so indices stay stable.
struct SearchHypothesis
{
    size_t edge_count = 0;
    size_t prev_index = kNoPredecessor;
    size_t arrival = 0;
    int weight = 0;
    // True once any generated edge participates in this path. Such hypotheses
    // rank behind every static hypothesis (see the finalize comparator) so
    // in-table inputs keep their baseline cut set.
    bool has_generated = false;
    AutocorrectEdge edge;
    // Order-sensitive rolling hash of the joined syllable sequence. Two
    // hypotheses reaching the same position share a sequence iff their hashes
    // match; dedup keys on this instead of materializing the full string per
    // node (which was O(length^2 * k) allocation on the keystroke path). A
    // 64-bit collision across the few thousand live hypotheses is ~1e-11, and
    // its only effect would be dropping one alternative reading -- the same
    // class of outcome the dedup itself produces.
    size_t seq_hash = 0;
};

// Fold one more syllable into a sequence hash (boost-style hash_combine over
// the predecessor hash so order and separators are encoded).
inline size_t extend_sequence_hash(const size_t seed, const std::string_view syllable)
{
    const size_t piece = std::hash<std::string_view>{}(syllable);
    return seed ^ (piece + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}
} // namespace

std::vector<AutocorrectCut> autocorrect_cut_kbest(const std::string &pinyin, const unsigned autocorrect_types,
                                                  const std::size_t k)
{
    // Contract (see header): no type enabled, manual delimiters, overlong
    // input, or k == 0 yield nothing. Every returned cut contains at least one
    // corrected edge; legal-only hypotheses at the final position are dropped
    // on arrival, so a fully legal input without any correction reading
    // returns {} -- the caller already owns the plain segmentation.
    if (k == 0 || autocorrect_types == 0 || pinyin.empty() || pinyin.size() > kMaxAutocorrectInputLength ||
        pinyin.find('\'') != std::string::npos)
    {
        return {};
    }

    const auto &valid_pinyin = intact_pinyin_set();
    const auto &index = correction_index();
    const size_t length = pinyin.size();

    // Top-k hypotheses per input position. All edges consume at least one raw
    // letter, so a single left-to-right sweep finalizes each position before
    // relaxing its out-edges -- a label-correcting pass without a queue.
    std::vector<std::vector<SearchHypothesis>> best(length + 1);
    std::vector<bool> finalized(length + 1, false);
    size_t arrival = 0;
    best[0].push_back(SearchHypothesis{}); // seed: zero edges, no predecessor

    const auto finalize = [&](const size_t position) {
        if (finalized[position])
        {
            return;
        }
        finalized[position] = true;
        auto &list = best[position];
        if (list.size() < 2)
        {
            return;
        }
        std::sort(list.begin(), list.end(), [](const SearchHypothesis &lhs, const SearchHypothesis &rhs) {
            // Generated-containing hypotheses rank behind ALL static ones
            // regardless of edge count: out-of-table shapes only step in when
            // the static space has no better explanation, and in-table inputs
            // must keep their baseline cut set (zero-regression by design).
            if (lhs.has_generated != rhs.has_generated)
            {
                return lhs.has_generated < rhs.has_generated;
            }
            if (lhs.edge_count != rhs.edge_count)
            {
                return lhs.edge_count < rhs.edge_count;
            }
            if (lhs.weight != rhs.weight)
            {
                return lhs.weight < rhs.weight;
            }
            return lhs.arrival < rhs.arrival;
        });
        // Same syllable sequence reached twice (different raw spans): keep the
        // best-ranked hypothesis, drop the rest before truncating to k. The list
        // is sorted by (edge_count, weight, arrival), NOT by sequence, so equal
        // sequences are not adjacent -- std::unique would miss them. Scan front
        // to back (best first) and drop any sequence hash already seen.
        std::unordered_set<size_t> seen_sequences;
        seen_sequences.reserve(list.size());
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const SearchHypothesis &hypothesis) {
                                      return !seen_sequences.insert(hypothesis.seq_hash).second;
                                  }),
                   list.end());
        if (list.size() > k)
        {
            list.resize(k);
        }
    };

    const auto extend = [&](const size_t end, const SearchHypothesis &parent, const std::size_t parent_index,
                            const AutocorrectEdge &edge, const int edge_weight) {
        SearchHypothesis child;
        child.edge_count = parent.edge_count + (edge.corrected ? 1 : 0);
        if (child.edge_count > kMaxAutocorrectEdges)
        {
            return;
        }
        // Legal-only hypotheses at the final position can never win a result
        // slot nor propagate further; dropping them here keeps k == 1
        // consistent with larger k (same first cut either way).
        if (end == length && child.edge_count == 0)
        {
            return;
        }
        child.prev_index = parent_index;
        child.arrival = arrival++;
        child.weight = parent.weight + edge_weight;
        child.edge = edge;
        child.has_generated = parent.has_generated || edge.generated;
        child.seq_hash = extend_sequence_hash(parent.seq_hash, edge.syllable);
        best[end].push_back(std::move(child));
    };

    for (size_t start = 0; start < length; ++start)
    {
        finalize(start);
        const auto &hypotheses = best[start];
        if (hypotheses.empty())
        {
            continue;
        }
        const size_t remaining = length - start;
        const size_t max_len = std::min<size_t>(remaining, 6);
        for (size_t len = max_len; len >= 1; --len)
        {
            const std::string_view piece(pinyin.data() + start, len);
            AutocorrectEdge edge;
            edge.raw_length = len;
            if (valid_pinyin.find(std::string(piece)) != valid_pinyin.end())
            {
                edge.syllable = piece;
                edge.corrected = false;
                for (size_t i = 0; i < hypotheses.size(); ++i)
                {
                    extend(start + len, hypotheses[i], i, edge, 0);
                }
                continue;
            }
            const auto found = index.find(piece);
            if (found == index.end())
            {
                continue;
            }
            // Every ambiguous target is a parallel edge; the enabled type
            // bits gate which tables may answer.
            for (const auto &target : found->second)
            {
                const unsigned enabled_bits = autocorrect_types & target.type_bit;
                if (enabled_bits == 0)
                {
                    continue;
                }
                edge.syllable = target.syllable;
                edge.corrected = true;
                edge.generated = target.generated;
                const int weight =
                    target.generated ? kAutocorrectGeneratedShapeWeight : min_enabled_correction_weight(enabled_bits);
                for (size_t i = 0; i < hypotheses.size(); ++i)
                {
                    extend(start + len, hypotheses[i], i, edge, weight);
                }
            }
        }
    }
    finalize(length);
    // Static-priority zero-regression (see the header contract): when the best
    // cut is pure static, drop every generated cut so every caller -- the
    // primary resolver, the head+tail composition retry, the k == 1 preedit
    // projection -- consumes exactly the baseline cut set; generated readings
    // surface only when no static cut explains the input at all. Living here
    // (not in each caller) is the point: a future consumer cannot forget it.
    if (!best[length].empty() && !best[length].front().has_generated)
    {
        best[length].erase(std::remove_if(best[length].begin(), best[length].end(),
                                          [](const SearchHypothesis &hypothesis) { return hypothesis.has_generated; }),
                           best[length].end());
    }

    // Rebuild each surviving end hypothesis by walking the predecessor chain.
    // raw_length (not syllable.size()) gives the raw span: deletion edges
    // consume one letter less, insertion edges one letter more, than the
    // syllable they produce.
    std::vector<AutocorrectCut> results;
    for (const auto &hypothesis : best[length])
    {
        AutocorrectCut cut;
        cut.edge_count = hypothesis.edge_count;
        cut.weight = hypothesis.weight;
        size_t position = length;
        const SearchHypothesis *current = &hypothesis;
        while (current->prev_index != kNoPredecessor)
        {
            const size_t segment_start = position - current->edge.raw_length;
            cut.segments.push_back(AutocorrectCutSegment{std::string(current->edge.syllable),
                                                         pinyin.substr(segment_start, current->edge.raw_length),
                                                         segment_start, current->edge.corrected});
            position = segment_start;
            current = &best[position][current->prev_index];
        }
        std::reverse(cut.segments.begin(), cut.segments.end());
        results.push_back(std::move(cut));
    }
    return results;
}

AutocorrectCut autocorrect_cut_detail(const std::string &pinyin, const unsigned autocorrect_types)
{
    // Single-hypothesis projection of the k-best search: same gating, same
    // ranking, cheapest path for the per-keystroke preedit consumer.
    const auto kbest = autocorrect_cut_kbest(pinyin, autocorrect_types, 1);
    return kbest.empty() ? AutocorrectCut{} : kbest.front();
}

Segments autocorrect_cut(const std::string &pinyin, const unsigned autocorrect_types)
{
    // Projection wrapper: existing callers and tests only need the corrected
    // syllable sequence, so keep the phase-1 signature working unchanged.
    const auto detail = autocorrect_cut_detail(pinyin, autocorrect_types);
    Segments result;
    result.reserve(detail.segments.size());
    for (const auto &segment : detail.segments)
    {
        result.push_back(segment.syllable);
    }
    return result;
}

} // namespace quanpin
