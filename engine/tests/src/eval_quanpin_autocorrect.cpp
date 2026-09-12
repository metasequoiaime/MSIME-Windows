//
// 全拼拼写纠错评测工具（独立 main，不并入 imetest）。
//
// 从真实 msime.db 采样 2–4 音节的高频词条，按错误率对音节独立注入
// 交换 / QWERTY 邻键 / 漏字 / 多字错误，测量目标词在 QuanpinDictionary 候选
// 列表中的召回名次（R@1 / R@3）、读音还原率与温态查询延迟（p50/p95）。
// 方法对标 Google 专利 CN 101133411 B 图 8/9 的注错率—召回曲线，是任务
// .trellis/tasks/09-12-quanpin-autocorrect-patent 与 09-13 insertion 任务的
// 量化验收载体。
//
// 注错的变体生成规则与 server/scripts/generate_quanpin_autocorrect.py
// 保持一致（交换相邻字母对 / QWERTY 邻键替换 / 删除一个字母 / 在相邻键
// 邻键或重复键位置插入一个字母，变体长度限制 3–6 且不落在合法音节上），
// 固定种子保证运行可复现。
//
// 用法（在仓库根目录执行）：
//   eval_quanpin_autocorrect --db <msime.db> [--samples N] [--seed S]
//       [--rates 10,25,50,100] [--model mixed|deletion|ambiguous|insertion]
//       [--resource <dir>] [--csv <path>] [--dump N]
//
// 报告分节：混合/漏字/多字模型输出注错率—召回曲线；漏字与多字模型额外输出
// gate breakdown（字典层两道门把样本切成简拼形/完整合法拼读/纠错可服务三类，
// 多字模型尤其需要：结尾单插入天然是简拼尾形状）；歧义模型分开输出可达集
// 指标与门保护集明细（保护集不计入 AC 分母）。
//
#include <Windows.h>
#include "contracts/dictionary/format.h"
#include "core/runtime_paths.h"
#include "quanpin/quanpin_dictionary.h"
#include "quanpin/quanpin_query.h"
#include "quanpin/quanpin_utils.h"
#include "sqlite3.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
using quanpin::Segments;

// 与 generate_quanpin_autocorrect.py 的 QWERTY_NEIGHBORS 逐键一致：仅字母键、
// 手指移动范围。两侧漂移会让注错模型脱离纠错表的假设空间，基线不可比。
const std::vector<std::pair<char, std::vector<char>>> &qwerty_neighbors()
{
    static const std::vector<std::pair<char, std::vector<char>>> kNeighbors = {
        {'q', {'w', 'a'}},
        {'w', {'q', 'e', 's', 'a'}},
        {'e', {'w', 'r', 's', 'd'}},
        {'r', {'e', 't', 'd', 'f'}},
        {'t', {'r', 'y', 'f', 'g'}},
        {'y', {'t', 'u', 'g', 'h'}},
        {'u', {'y', 'i', 'h', 'j'}},
        {'i', {'u', 'o', 'j', 'k'}},
        {'o', {'i', 'p', 'k', 'l'}},
        {'p', {'o', 'l'}},
        {'a', {'q', 'w', 's', 'z'}},
        {'s', {'a', 'w', 'd', 'e', 'x', 'z'}},
        {'d', {'s', 'e', 'r', 'f', 'c', 'x'}},
        {'f', {'d', 'r', 't', 'g', 'v', 'c'}},
        {'g', {'f', 't', 'y', 'h', 'b', 'v'}},
        {'h', {'g', 'y', 'u', 'j', 'n', 'b'}},
        {'j', {'h', 'u', 'i', 'k', 'm', 'n'}},
        {'k', {'j', 'i', 'o', 'l', 'm'}},
        {'l', {'k', 'o', 'p'}},
        {'z', {'a', 's', 'x'}},
        {'x', {'z', 's', 'd', 'c'}},
        {'c', {'x', 'd', 'f', 'v'}},
        {'v', {'c', 'f', 'g', 'b'}},
        {'b', {'v', 'g', 'h', 'n'}},
        {'m', {'n', 'j', 'k'}},
        {'n', {'b', 'h', 'j', 'm'}},
    };
    return kNeighbors;
}

enum class ErrorModel
{
    Mixed,
    Deletion,
    Ambiguous,
    Insertion,
};

enum class ErrorKind
{
    Transposition,
    Neighbor,
    Deletion,
    Insertion,
};

struct Options
{
    std::string db_path;
    std::filesystem::path resource = std::filesystem::path("server") / "assets" / "tables";
    int samples = 300;
    unsigned seed = 42;
    std::vector<int> rates = {10, 25, 50, 100};
    ErrorModel model = ErrorModel::Mixed;
    std::string model_name = "mixed";
    std::string csv_path;
    int dump = 0;
};

void usage_exit(const std::string &message)
{
    if (!message.empty())
    {
        std::cerr << "ERROR: " << message << "\n";
    }
    std::cerr << "usage: eval_quanpin_autocorrect --db <msime.db> [--samples N] [--seed S]\n"
              << "           [--rates 10,25,50,100] [--model mixed|deletion|ambiguous|insertion]\n"
              << "           [--resource <dir>] [--csv <path>] [--dump N]\n";
    std::exit(2);
}

std::vector<int> parse_rates(const std::string &text)
{
    std::vector<int> rates;
    std::istringstream stream(text);
    std::string token;
    while (std::getline(stream, token, ','))
    {
        if (token.empty())
        {
            continue;
        }
        try
        {
            const int rate = std::stoi(token);
            if (rate < 1 || rate > 100)
            {
                usage_exit("rate must be within 1..100: " + token);
            }
            rates.push_back(rate);
        }
        catch (const std::exception &)
        {
            usage_exit("invalid rate: " + token);
        }
    }
    if (rates.empty())
    {
        usage_exit("--rates produced no values");
    }
    return rates;
}

Options parse_options(int argc, char *argv[])
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        const auto value_or_exit = [&](std::string &target) {
            if (i + 1 >= argc)
            {
                usage_exit(argument + " requires a value");
            }
            target = argv[++i];
        };
        if (argument == "--db")
        {
            value_or_exit(options.db_path);
        }
        else if (argument == "--samples")
        {
            std::string value;
            value_or_exit(value);
            options.samples = std::stoi(value);
            if (options.samples < 1)
            {
                usage_exit("--samples must be positive");
            }
        }
        else if (argument == "--seed")
        {
            std::string value;
            value_or_exit(value);
            const unsigned long long seed = std::stoull(value);
            options.seed = static_cast<unsigned>(seed);
        }
        else if (argument == "--rates")
        {
            std::string value;
            value_or_exit(value);
            options.rates = parse_rates(value);
        }
        else if (argument == "--model")
        {
            value_or_exit(options.model_name);
            if (options.model_name == "mixed")
            {
                options.model = ErrorModel::Mixed;
            }
            else if (options.model_name == "deletion")
            {
                options.model = ErrorModel::Deletion;
            }
            else if (options.model_name == "ambiguous")
            {
                options.model = ErrorModel::Ambiguous;
            }
            else if (options.model_name == "insertion")
            {
                options.model = ErrorModel::Insertion;
            }
            else
            {
                usage_exit("unknown --model: " + options.model_name);
            }
        }
        else if (argument == "--resource")
        {
            std::string value;
            value_or_exit(value);
            options.resource = std::filesystem::path(value);
        }
        else if (argument == "--csv")
        {
            value_or_exit(options.csv_path);
        }
        else if (argument == "--dump")
        {
            std::string value;
            value_or_exit(value);
            options.dump = std::stoi(value);
            if (options.dump < 0)
            {
                usage_exit("--dump must be non-negative");
            }
        }
        else
        {
            usage_exit("unknown argument: " + argument);
        }
    }
    if (options.db_path.empty())
    {
        usage_exit("--db is required");
    }
    if (!std::filesystem::exists(options.db_path))
    {
        usage_exit("database not found: " + options.db_path);
    }
    if (!std::filesystem::exists(options.resource))
    {
        usage_exit("resource directory not found: " + options.resource.string() +
                   " (run from the repository root or pass --resource)");
    }
    return options;
}

// 一个采样词条：读音（带撇号切分）是 ground truth，word 是要召回的目标词。
struct Entry
{
    std::string key;
    std::string word;
    std::int64_t weight = 0;
    Segments syllables;
};

bool all_chars_cjk(const std::string &text)
{
    size_t i = 0;
    while (i < text.size())
    {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        unsigned code_point = 0;
        size_t length = 0;
        if ((lead & 0x80) == 0)
        {
            return false;
        }
        else if ((lead & 0xE0) == 0xC0)
        {
            code_point = lead & 0x1Fu;
            length = 2;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            code_point = lead & 0x0Fu;
            length = 3;
        }
        else if ((lead & 0xF8) == 0xF0)
        {
            code_point = lead & 0x07u;
            length = 4;
        }
        else
        {
            return false;
        }
        if (i + length > text.size())
        {
            return false;
        }
        for (size_t k = 1; k < length; ++k)
        {
            const unsigned char continuation = static_cast<unsigned char>(text[i + k]);
            if ((continuation & 0xC0) != 0x80)
            {
                return false;
            }
            code_point = (code_point << 6) | (continuation & 0x3Fu);
        }
        const bool cjk =
            (code_point >= 0x4E00 && code_point <= 0x9FFF) || (code_point >= 0x3400 && code_point <= 0x4DBF) ||
            (code_point >= 0xF900 && code_point <= 0xFAFF) || (code_point >= 0x20000 && code_point <= 0x2FFFF);
        if (!cjk)
        {
            return false;
        }
        i += length;
    }
    return !text.empty();
}

// 每表只取 weight 头部再全局归并：评测关注常用词的纠错召回，与专利
// 图 8/9 基于高频词条采样一致；池子放大到请求数的 12 倍以吸收低错误率
// 档位的高作废率（未注错或注错后仍合法的样本会被丢弃）。
std::vector<Entry> build_sample_pool(sqlite3 *db, const std::unordered_set<std::string> &legal, size_t limit)
{
    std::vector<Entry> pool;
    const std::string initials(metasequoia::dictionary_format::initials);
    const std::string per_table_limit = std::to_string(std::max<size_t>(limit, 6000));
    for (size_t count = 2; count <= 4; ++count)
    {
        for (const char initial : initials)
        {
            const std::string table = metasequoia::dictionary_format::quanpin_table(count, initial);
            if (table.empty())
            {
                continue;
            }
            // key/value 作排序决胜：同 weight 行在 SQLite 里顺序不稳定，没有
            // tiebreaker 时两次运行会采到不同样本，基线不可复现。
            const std::string sql = "SELECT key, value, weight FROM " + table +
                                    " ORDER BY weight DESC, key ASC, value ASC LIMIT " + per_table_limit;
            sqlite3_stmt *statement = nullptr;
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
            {
                continue;
            }
            while (sqlite3_step(statement) == SQLITE_ROW)
            {
                const auto *key_text = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
                const auto *value_text = reinterpret_cast<const char *>(sqlite3_column_text(statement, 1));
                if (key_text == nullptr || value_text == nullptr)
                {
                    continue;
                }
                Entry entry;
                entry.key = key_text;
                entry.word = value_text;
                entry.weight = sqlite3_column_int64(statement, 2);
                entry.syllables = quanpin::split_segments(entry.key);
                if (entry.syllables.size() != count)
                {
                    continue;
                }
                bool legal_key = true;
                for (const auto &syllable : entry.syllables)
                {
                    if (legal.count(syllable) == 0)
                    {
                        legal_key = false;
                        break;
                    }
                }
                if (!legal_key || !all_chars_cjk(entry.word))
                {
                    continue;
                }
                pool.push_back(std::move(entry));
            }
            sqlite3_finalize(statement);
        }
    }
    std::sort(pool.begin(), pool.end(), [](const Entry &a, const Entry &b) { return a.weight > b.weight; });
    if (pool.size() > limit)
    {
        pool.resize(limit);
    }
    return pool;
}

std::vector<std::string> raw_variants(ErrorKind kind, const std::string &syllable)
{
    std::vector<std::string> out;
    if (kind == ErrorKind::Transposition)
    {
        for (size_t i = 0; i + 1 < syllable.size(); ++i)
        {
            out.push_back(syllable.substr(0, i) + syllable[i + 1] + syllable[i] + syllable.substr(i + 2));
        }
    }
    else if (kind == ErrorKind::Neighbor)
    {
        for (size_t i = 0; i < syllable.size(); ++i)
        {
            for (const auto &[key, neighbors] : qwerty_neighbors())
            {
                if (key != syllable[i])
                {
                    continue;
                }
                for (const char neighbor : neighbors)
                {
                    out.push_back(syllable.substr(0, i) + neighbor + syllable.substr(i + 1));
                }
            }
        }
    }
    else if (kind == ErrorKind::Deletion)
    {
        for (size_t i = 0; i < syllable.size(); ++i)
        {
            out.push_back(syllable.substr(0, i) + syllable.substr(i + 1));
        }
    }
    else
    {
        // 插入：与生成器 insertion_variants 同款约束——位置锚定串首（参照右
        // 邻）、串尾（参照左邻）、中间（参照任一侧），插入字母必须是相邻键的
        // QWERTY 邻键或重复（双击）。
        for (size_t position = 0; position <= syllable.size(); ++position)
        {
            std::unordered_set<char> candidates;
            if (position > 0)
            {
                const char left = syllable[position - 1];
                candidates.insert(left);
                for (const auto &[key, neighbors] : qwerty_neighbors())
                {
                    if (key == left)
                    {
                        candidates.insert(neighbors.begin(), neighbors.end());
                    }
                }
            }
            if (position < syllable.size())
            {
                const char right = syllable[position];
                candidates.insert(right);
                for (const auto &[key, neighbors] : qwerty_neighbors())
                {
                    if (key == right)
                    {
                        candidates.insert(neighbors.begin(), neighbors.end());
                    }
                }
            }
            for (const char letter : candidates)
            {
                out.push_back(syllable.substr(0, position) + letter + syllable.substr(position));
            }
        }
    }
    return out;
}

// 生成器 add_variant 的同款过滤：落在合法音节、长度越界（<3 或 >6）或含
// 撇号的变体不进入用户错误模型——它们要么属于合法/简拼输入空间，要么超出
// 纠错表的键长假设。
std::vector<std::string> usable_variants(ErrorKind kind, const std::string &syllable,
                                         const std::unordered_set<std::string> &legal)
{
    std::vector<std::string> out;
    for (const auto &variant : raw_variants(kind, syllable))
    {
        if (variant == syllable || legal.count(variant) != 0 || variant.size() < 3 || variant.size() > 6 ||
            variant.find('\'') != std::string::npos)
        {
            continue;
        }
        out.push_back(variant);
    }
    return out;
}

// mixed = 交换 25% / 邻键 25% / 漏字 25% / 多字 25%；deletion = 100% 漏字；
// insertion = 100% 多字。选中的错误类型在该音节上无可用变体（如 2 字母音节
// 或 6 字母音节的插入变体超长）时保持原样，交由样本级作废逻辑处理，不回退
// 到其他类型，保证分布可解释。
std::string corrupt_syllable(const std::string &syllable, std::mt19937 &rng, ErrorModel model,
                             const std::unordered_set<std::string> &legal)
{
    ErrorKind kind = ErrorKind::Deletion;
    if (model == ErrorModel::Mixed)
    {
        const int roll = std::uniform_int_distribution<int>(1, 100)(rng);
        kind = roll <= 25   ? ErrorKind::Transposition
               : roll <= 50 ? ErrorKind::Neighbor
               : roll <= 75 ? ErrorKind::Deletion
                            : ErrorKind::Insertion;
    }
    else if (model == ErrorModel::Insertion)
    {
        kind = ErrorKind::Insertion;
    }
    const auto variants = usable_variants(kind, syllable, legal);
    if (variants.empty())
    {
        return syllable;
    }
    const size_t pick = std::uniform_int_distribution<size_t>(0, variants.size() - 1)(rng);
    return variants[pick];
}

struct RateReport
{
    int rate = 0;
    size_t samples = 0;
    size_t recall1 = 0;
    size_t recall3 = 0;
    size_t restored = 0;
    std::vector<double> warm_ms;
    struct GateBreakdown
    {
        size_t jianpin_shape = 0;
        size_t complete_legal = 0;
        size_t serviceable = 0;
        size_t serviceable_recall1 = 0;
        size_t serviceable_recall3 = 0;
    } gate;
};

// The dictionary layer only runs the correction BFS when neither guard fires:
// a jianpin-shaped input (one legal syllable plus at most one trailing letter)
// or an input whose base segmentation is already complete legal pinyin keeps
// the plain reading (resolve_series_query). The classification mirrors the
// dictionary predicates, so the report attributes recall to the same split the
// runtime makes.
enum class DictionaryGate
{
    JianpinShape,
    CompleteLegalPinyin,
    CorrectionServiceable,
};

DictionaryGate classify_dictionary_gate(const std::string &typo)
{
    if (quanpin::looks_like_syllable_with_jianpin_tail(typo))
    {
        return DictionaryGate::JianpinShape;
    }
    // get_or_compute_segments takes the front correction-mode cut as the base
    // segmentation; the completeness check runs on exactly that value.
    const auto cuts = quanpin::cut_pinyin_by_mode(typo, "correction");
    if (!cuts.empty() && quanpin::has_only_complete_pinyin_segments(cuts.front()))
    {
        return DictionaryGate::CompleteLegalPinyin;
    }
    return DictionaryGate::CorrectionServiceable;
}

double percentile(std::vector<double> values, double fraction)
{
    if (values.empty())
    {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t index = std::min(values.size() - 1, static_cast<size_t>(values.size() * fraction));
    return values[index];
}

int rank_of_word(const std::vector<WordItem> &candidates, const std::string &word)
{
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (candidates[i].word == word)
        {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

struct CaseOutcome
{
    std::string typo;
    const Entry *entry = nullptr;
    int rank = 0;
    bool restored = false;
    double warm_ms = 0.0;
    std::string top_candidates; // 前 3 个候选词，仅供 --dump 人工核对
};

// 单条样本：预热一次让缓存命中，再计时第二次查询作为温态延迟——与
// AC4「温态查询 p95 ≤ 25 ms」的口径一致（用户连续击键同键的场景）。
CaseOutcome evaluate_case(QuanpinDictionary &dictionary, const std::string &typo, const Entry &entry,
                          unsigned autocorrect_types)
{
    (void)dictionary.query(typo, "", autocorrect_types, {});
    const auto start = std::chrono::steady_clock::now();
    const auto candidates = dictionary.query(typo, "", autocorrect_types, {});
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);

    CaseOutcome outcome;
    outcome.typo = typo;
    outcome.entry = &entry;
    outcome.rank = rank_of_word(candidates, entry.word);
    outcome.warm_ms = static_cast<double>(elapsed.count()) / 1000.0;
    for (size_t i = 0; i < candidates.size() && i < 3; ++i)
    {
        if (i > 0)
        {
            outcome.top_candidates += " | ";
        }
        outcome.top_candidates += candidates[i].word;
    }

    const auto cut = quanpin::autocorrect_cut(typo, autocorrect_types);
    outcome.restored = !cut.empty() && quanpin::join_segments(cut) == entry.key;
    return outcome;
}

std::string percent(size_t part, size_t total)
{
    if (total == 0)
    {
        return "n/a";
    }
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << (100.0 * static_cast<double>(part) / static_cast<double>(total))
           << "%";
    return stream.str();
}

std::string milliseconds(double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value;
    return stream.str();
}

void append_report_row(std::ostream &stream, const RateReport &report)
{
    stream << "| " << report.rate << " | " << report.samples << " | " << percent(report.recall1, report.samples)
           << " | " << percent(report.recall3, report.samples) << " | " << percent(report.restored, report.samples)
           << " | " << milliseconds(percentile(report.warm_ms, 0.50)) << " | "
           << milliseconds(percentile(report.warm_ms, 0.95)) << " |\n";
}

void append_csv_row(std::ostream &stream, const std::string &model, const RateReport &report)
{
    stream << model << "," << report.rate << "," << report.samples << "," << percent(report.recall1, report.samples)
           << "," << percent(report.recall3, report.samples) << "," << percent(report.restored, report.samples) << ","
           << milliseconds(percentile(report.warm_ms, 0.50)) << "," << milliseconds(percentile(report.warm_ms, 0.95))
           << "\n";
}

void dump_outcome(const CaseOutcome &outcome)
{
    std::cout << "    typo=" << outcome.typo << " expect=" << outcome.entry->word << " (" << outcome.entry->key
              << ") rank=" << outcome.rank << (outcome.restored ? " restored" : "") << " top3=["
              << outcome.top_candidates << "]\n";
}

// Google 解码器以可写方式持有用户词典；把资源目录里的 user_dict.dat 暂存到
// 临时目录再交给 RuntimePaths，评测不能回写仓内 server/assets/tables。
std::filesystem::path stage_user_dictionary(const std::filesystem::path &resource)
{
    const auto staged = std::filesystem::temp_directory_path() / "msime-eval-user-dict";
    std::filesystem::create_directories(staged);
    const auto source = resource / metasequoia::assets::pinyin_user_dictionary;
    const auto target = staged / metasequoia::assets::pinyin_user_dictionary;
    std::error_code error;
    if (std::filesystem::exists(source))
    {
        std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
    }
    else
    {
        std::ofstream(target, std::ios::binary) << "";
    }
    return staged;
}

std::vector<RateReport> run_injected_model(QuanpinDictionary &dictionary, const std::vector<Entry> &pool,
                                           const Options &options, unsigned autocorrect_types)
{
    const auto &legal = quanpin::intact_pinyin_set();
    std::vector<RateReport> reports;
    for (const int rate : options.rates)
    {
        // 每档从同一种子重开随机流：单独跑一档与全量跑时该档样本完全一致。
        std::mt19937 rng(options.seed);
        std::uniform_real_distribution<double> chance(0.0, 1.0);

        RateReport report;
        report.rate = rate;
        size_t dumped = 0;
        for (size_t index = 0; index < pool.size() && report.samples < static_cast<size_t>(options.samples); ++index)
        {
            const Entry &entry = pool[index];
            std::string typo;
            for (const auto &syllable : entry.syllables)
            {
                if (chance(rng) < rate / 100.0)
                {
                    typo += corrupt_syllable(syllable, rng, options.model, legal);
                }
                else
                {
                    typo += syllable;
                }
            }
            if (quanpin::is_complete_pinyin_input(typo))
            {
                continue;
            }
            const auto outcome = evaluate_case(dictionary, typo, entry, autocorrect_types);
            ++report.samples;
            report.recall1 += outcome.rank == 1 ? 1 : 0;
            report.recall3 += outcome.rank >= 1 && outcome.rank <= 3 ? 1 : 0;
            report.restored += outcome.restored ? 1 : 0;
            report.warm_ms.push_back(outcome.warm_ms);
            if (options.model == ErrorModel::Deletion || options.model == ErrorModel::Insertion)
            {
                switch (classify_dictionary_gate(typo))
                {
                case DictionaryGate::JianpinShape:
                    ++report.gate.jianpin_shape;
                    break;
                case DictionaryGate::CompleteLegalPinyin:
                    ++report.gate.complete_legal;
                    break;
                case DictionaryGate::CorrectionServiceable:
                    ++report.gate.serviceable;
                    report.gate.serviceable_recall1 += outcome.rank == 1 ? 1 : 0;
                    report.gate.serviceable_recall3 += outcome.rank >= 1 && outcome.rank <= 3 ? 1 : 0;
                    break;
                }
            }
            if (dumped < static_cast<size_t>(options.dump))
            {
                dump_outcome(outcome);
                ++dumped;
            }
        }
        reports.push_back(std::move(report));
    }
    return reports;
}

struct AmbiguousCase
{
    const char *input;
    std::vector<std::string> syllables;
};

// 歧义样本集：一个错拼串在纠错空间里对应多个不同音节（现状生成器会整键
// 丢弃的那批）。目标词取各目标音节在单音节表里的头部词条。
const std::vector<AmbiguousCase> &ambiguous_cases()
{
    static const std::vector<AmbiguousCase> kCases = {
        {"ahan", {"shan", "zhan"}}, {"chn", {"chan", "chen"}},    {"bng", {"bang", "beng"}},  {"din", {"dian", "ding"}},
        {"shn", {"shan", "shen"}},  {"zhng", {"zhang", "zheng"}}, {"aang", {"sang", "wang"}},
    };
    return kCases;
}

std::vector<std::string> top_words_for_syllable(sqlite3 *db, const std::string &syllable, int limit)
{
    const std::string table = metasequoia::dictionary_format::quanpin_table(1, syllable.front());
    std::vector<std::string> words;
    if (table.empty())
    {
        return words;
    }
    const std::string sql = "SELECT value FROM " + table + " WHERE key = '" + syllable +
                            "' ORDER BY weight DESC, value ASC LIMIT " + std::to_string(limit);
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    {
        return words;
    }
    while (sqlite3_step(statement) == SQLITE_ROW && words.size() < static_cast<size_t>(limit))
    {
        const auto *value_text = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
        if (value_text != nullptr)
        {
            words.push_back(value_text);
        }
    }
    sqlite3_finalize(statement);
    return words;
}

struct AmbiguousTargetOutcome
{
    std::string input;
    std::string syllable;
    std::string word;
    int rank = 0;
};

struct AmbiguousReport
{
    RateReport reachable;
    RateReport gate_protected;
    std::vector<AmbiguousTargetOutcome> gate_protected_details;
};

// Ambiguous samples: one typo mapping to several syllables. Inputs the
// dictionary gates out are reported separately as AC3 guard evidence and stay
// out of the recall denominator, which otherwise measures the guards rather
// than the correction search. One target per syllable (its highest-weight
// word) replaces the old top-3 words per syllable: the old layout gave every
// input six targets competing for three slots (a 50% structural ceiling) and
// let the protected inputs distort the metric.
AmbiguousReport run_ambiguous_model(QuanpinDictionary &dictionary, sqlite3 *db, const Options &options,
                                    unsigned autocorrect_types)
{
    AmbiguousReport report;
    report.reachable.rate = 100;
    report.gate_protected.rate = 100;
    for (const auto &ambiguous : ambiguous_cases())
    {
        const bool gate_protected = classify_dictionary_gate(ambiguous.input) != DictionaryGate::CorrectionServiceable;
        RateReport &target_report = gate_protected ? report.gate_protected : report.reachable;

        struct Target
        {
            std::string syllable;
            std::string word;
        };
        std::vector<Target> targets;
        for (const auto &syllable : ambiguous.syllables)
        {
            auto words = top_words_for_syllable(db, syllable, 1);
            if (!words.empty())
            {
                targets.push_back(Target{syllable, std::move(words.front())});
            }
        }
        if (targets.empty())
        {
            continue;
        }

        (void)dictionary.query(ambiguous.input, "", autocorrect_types, {});
        const auto start = std::chrono::steady_clock::now();
        const auto candidates = dictionary.query(ambiguous.input, "", autocorrect_types, {});
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
        const double warm_ms = static_cast<double>(elapsed.count()) / 1000.0;
        // Primary correction reading, used for the restored column: for a
        // protected input this is the reading the guard deliberately ignores.
        const auto primary_reading =
            quanpin::join_segments(quanpin::autocorrect_cut(ambiguous.input, autocorrect_types));

        if (options.dump > 0)
        {
            std::cout << "    input=" << ambiguous.input << (gate_protected ? " [gate-protected]" : "");
        }
        for (const auto &target : targets)
        {
            const int rank = rank_of_word(candidates, target.word);
            ++target_report.samples;
            target_report.recall1 += rank == 1 ? 1 : 0;
            target_report.recall3 += rank >= 1 && rank <= 3 ? 1 : 0;
            target_report.restored += primary_reading == target.syllable ? 1 : 0;
            target_report.warm_ms.push_back(warm_ms);
            if (gate_protected)
            {
                report.gate_protected_details.push_back(
                    AmbiguousTargetOutcome{ambiguous.input, target.syllable, target.word, rank});
            }
            if (options.dump > 0)
            {
                std::cout << " [" << target.syllable << "->" << target.word << " rank=" << rank << "]";
            }
        }
        if (options.dump > 0)
        {
            std::cout << "\n";
        }
    }
    return report;
}
} // namespace

int main(int argc, char *argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    const Options options = parse_options(argc, argv);

    sqlite3 *sampling_db = nullptr;
    if (sqlite3_open_v2(options.db_path.c_str(), &sampling_db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        std::cerr << "ERROR: cannot open " << options.db_path << "\n";
        sqlite3_close(sampling_db);
        return 1;
    }

    // 纠错口径：四种类型全开（含阶段 4 的 insertion 位）。基线（types=3）采集于
    // 阶段 0；insertion 上线前后的对照在开关层面采集（同种子同模型，唯一差异是
    // insertion 位），数字见 09-13 任务 eval/report.md。
    const unsigned autocorrect_types = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor |
                                       quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
    const auto staged_user = stage_user_dictionary(options.resource);
    const metasequoia::RuntimePaths paths{options.resource, staged_user, staged_user, options.resource};
    QuanpinDictionary dictionary(options.db_path, paths);

    std::vector<RateReport> reports;
    AmbiguousReport ambiguous_report;
    bool ambiguous_run = false;
    if (options.model == ErrorModel::Ambiguous)
    {
        ambiguous_report = run_ambiguous_model(dictionary, sampling_db, options, autocorrect_types);
        ambiguous_run = true;
        // The main table carries the reachable (correction-serviceable) set;
        // the gate-protected set is AC3 guard evidence and must not enter the
        // recall denominator.
        reports.push_back(ambiguous_report.reachable);
    }
    else
    {
        const size_t pool_limit = static_cast<size_t>(options.samples) * 12;
        const auto pool = build_sample_pool(sampling_db, quanpin::intact_pinyin_set(), pool_limit);
        if (options.dump > 0)
        {
            std::cout << "sample pool: " << pool.size() << " entries (limit " << pool_limit << ")\n";
        }
        if (pool.empty())
        {
            std::cerr << "ERROR: sample pool is empty; is the database a quanpin msime.db?\n";
            sqlite3_close(sampling_db);
            return 1;
        }
        reports = run_injected_model(dictionary, pool, options, autocorrect_types);
    }

    std::ostringstream markdown;
    markdown << "# Quanpin autocorrect evaluation\n\n";
    markdown << "- model: " << options.model_name << "\n";
    markdown << "- db: " << options.db_path << "\n";
    markdown << "- resource: " << options.resource.string() << "\n";
    markdown << "- autocorrect_types: " << autocorrect_types << " (transposition|neighbor|deletion|insertion)\n";
    markdown << "- seed: " << options.seed << ", samples: " << options.samples << "\n\n";
    markdown << "| error% | samples | R@1 | R@3 | reading restored | p50 (ms) | p95 (ms) |\n";
    markdown << "|-------:|--------:|----:|----:|-----------------:|---------:|---------:|\n";
    for (const auto &report : reports)
    {
        append_report_row(markdown, report);
    }
    if (ambiguous_run)
    {
        std::string reachable_inputs;
        std::string protected_inputs;
        for (const auto &ambiguous : ambiguous_cases())
        {
            std::string &list = classify_dictionary_gate(ambiguous.input) == DictionaryGate::CorrectionServiceable
                                    ? reachable_inputs
                                    : protected_inputs;
            if (!list.empty())
            {
                list += ", ";
            }
            list += ambiguous.input;
        }
        markdown << "\n## Gate-protected inputs (AC3 guard evidence, excluded from the denominator)\n\n";
        markdown << "- reachable inputs (in the table above): " << reachable_inputs << "\n";
        markdown << "- gate-protected inputs: " << protected_inputs
                 << " (jianpin shape or already-complete legal pinyin; the dictionary keeps their plain reading)\n";
        markdown << "\n| input | syllable | top word | rank |\n";
        markdown << "|-------|----------|----------|-----:|\n";
        for (const auto &detail : ambiguous_report.gate_protected_details)
        {
            markdown << "| " << detail.input << " | " << detail.syllable << " | " << detail.word << " | " << detail.rank
                     << " |\n";
        }
        markdown << "\n- protected summary: R@1 "
                 << percent(ambiguous_report.gate_protected.recall1, ambiguous_report.gate_protected.samples)
                 << ", R@3 "
                 << percent(ambiguous_report.gate_protected.recall3, ambiguous_report.gate_protected.samples) << " ("
                 << ambiguous_report.gate_protected.samples << " targets; ranks above)\n";
    }
    if (options.model == ErrorModel::Deletion || options.model == ErrorModel::Insertion)
    {
        markdown << "\n## Gate breakdown (" << options.model_name << " model)\n\n";
        markdown
            << "The dictionary only feeds the correction BFS when neither guard in `resolve_series_query` fires: "
               "a jianpin-shaped input (one legal syllable + at most one trailing letter) or an input whose base "
               "segmentation is already complete legal pinyin keeps the plain reading. Shares are over the samples "
               "that entered the metric.\n\n";
        markdown << "| error% | samples | jianpin-shape | complete legal pinyin | correctable | correctable R@1 | "
                    "correctable R@3 |\n";
        markdown << "|-------:|--------:|--------------:|---------------------:|------------:|----------------:|-------"
                    "---------:|\n";
        for (const auto &report : reports)
        {
            markdown << "| " << report.rate << " | " << report.samples << " | " << report.gate.jianpin_shape << " ("
                     << percent(report.gate.jianpin_shape, report.samples) << ") | " << report.gate.complete_legal
                     << " (" << percent(report.gate.complete_legal, report.samples) << ") | " << report.gate.serviceable
                     << " (" << percent(report.gate.serviceable, report.samples) << ") | "
                     << percent(report.gate.serviceable_recall1, report.gate.serviceable) << " | "
                     << percent(report.gate.serviceable_recall3, report.gate.serviceable) << " |\n";
        }
    }
    std::cout << markdown.str();

    if (!options.csv_path.empty())
    {
        std::ofstream csv(options.csv_path, std::ios::binary);
        csv << "model,rate,samples,r1,r3,restored,p50_ms,p95_ms\n";
        for (const auto &report : reports)
        {
            append_csv_row(csv, options.model_name, report);
        }
        if (ambiguous_run)
        {
            append_csv_row(csv, "ambiguous-protected", ambiguous_report.gate_protected);
        }
        std::cout << "csv written: " << options.csv_path << "\n";
    }

    sqlite3_close(sampling_db);
    return 0;
}
