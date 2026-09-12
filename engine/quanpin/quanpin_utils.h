#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace quanpin
{
using Segments = std::vector<std::string>;

struct SyllableEdge
{
    size_t end = 0;
    std::string syllable;
};

struct SyllableGraph
{
    size_t input_length = 0;
    std::vector<std::vector<SyllableEdge>> edges;
};

const std::vector<std::string> &intact_pinyin_list();
const std::unordered_set<std::string> &intact_pinyin_set();
const std::unordered_set<std::string> &prefix_pinyin_set();
bool has_only_complete_pinyin_segments(const Segments &segments);
SyllableGraph build_syllable_graph(const std::string &pinyin);
std::vector<Segments> enumerate_complete_segmentations(const SyllableGraph &graph, size_t path_limit = 32);
std::vector<std::string> cut_one_piece_greedy(const std::string &pinyin, bool intact_only);
std::vector<std::string> cut_one_piece_min_segments(const std::string &pinyin, bool intact_only);
bool is_complete_pinyin_input(const std::string &pinyin);
size_t detect_active_helpcode_length(const std::string &raw_input, const std::string &raw_input_with_cases);
std::string strip_active_helpcodes(const std::string &raw_input, const std::string &raw_input_with_cases);
std::string strip_active_helpcodes_with_cases(const std::string &raw_input, const std::string &raw_input_with_cases);
std::vector<Segments> sparse_pinyin_fallback_segments(const Segments &segments);

// Autocorrection type bits passed to autocorrect_cut. One mask lets the dictionary
// layer gate the whole feature with a single value while each type stays
// independently toggleable (quanpin.autocorrect_transposition / autocorrect_neighbor).
inline constexpr unsigned kAutocorrectTransposition = 1u << 0;
inline constexpr unsigned kAutocorrectNeighbor = 1u << 1;
// Deletion (one dropped letter) corrections. The server side wires this bit to
// the two existing autocorrect switches (either one on also enables deletion,
// design D2); it stays a separate bit so a future config key can gate it alone.
inline constexpr unsigned kAutocorrectDeletion = 1u << 2;
// Insertion (one extra letter, a repeated or QWERTY-neighbor key) corrections.
// Same linkage model as deletion: the legacy switches carry this bit too (design
// D4 of the insertion task); the bit stays independent for a future config key.
inline constexpr unsigned kAutocorrectInsertion = 1u << 3;

/**
 * Integer correction-edge weights (patent CN 101133411 B, [0052]).
 *
 * The patent prescribes probability-weighted edit distances trained on user
 * data (fig. 10); until such calibration data exists it explicitly allows
 * estimated constants, which is what these are. The prior: transpositions are
 * the most common typo, deletions slightly less, insertions next (real extra-key
 * slips are rarer than drops, and the neighbor/double-tap constraint keeps the
 * table small), neighbor substitutions carry the widest false-positive surface
 * (largest table), so they cost the most.
 * Calibration path: mine user_journal for corrected_from candidate hits.
 *
 * Ranking contract: the corrected-edge COUNT stays the primary sort key (the
 * least-intrusive-correction semantics), weights only break ties between cuts
 * with the same edge count. The engine never trades one extra correction for a
 * lower total weight.
 */
inline constexpr int kAutocorrectTranspositionWeight = 10;
inline constexpr int kAutocorrectDeletionWeight = 11;
// Between deletion and neighbor: insertion slips are estimated rarer than
// drops (patent [0052] estimated constant), but the constrained table yields
// a narrower false-positive surface than neighbor substitutions.
inline constexpr int kAutocorrectInsertionWeight = 12;
inline constexpr int kAutocorrectNeighborWeight = 13;

// One segment of an autocorrect-aware cut. syllable is the canonical (possibly
// table-corrected) text used for dictionary lookups; raw_text/start describe the
// original letters the segment consumed so the preedit can keep showing what the
// user typed while separators follow the actual cut positions. Deletion edges
// consume one letter less, insertion edges one letter more, than the syllable
// they produce, so raw_text may differ in length from syllable
// ("zhng" -> zhang, "shangg" -> shang).
struct AutocorrectCutSegment
{
    std::string syllable;
    std::string raw_text;
    size_t start = 0;
    bool corrected = false;
};

struct AutocorrectCut
{
    std::vector<AutocorrectCutSegment> segments;

    bool empty() const
    {
        return segments.empty();
    }
};

// Range-carrying variant of autocorrect_cut: identical gating (no type enabled,
// manual delimiters, overlong input) and identical cost model (it is the k=1
// projection of autocorrect_cut_kbest), but every segment also reports which raw
// letters it replaced.
AutocorrectCut autocorrect_cut_detail(const std::string &pinyin, unsigned autocorrect_types);

// Cuts the input into syllables, allowing at most kMaxAutocorrectEdges correction
// edges from the tables selected by autocorrect_types. Returns {} when no type is
// enabled, the input contains a manual delimiter, or no correction path exists.
// This is the k=1 projection of autocorrect_cut_kbest.
Segments autocorrect_cut(const std::string &pinyin, unsigned autocorrect_types);

// Up to k ranked correction cuts of the input: per-position top-k hypothesis
// propagation (label-correcting left-to-right sweep) over the same syllable
// graph and gating as autocorrect_cut, but keeping ambiguous table entries
// alive as parallel hypotheses. Ranking key: (corrected edge count, summed
// edge weight, generation order = table order); hypotheses explaining the same
// syllable sequence are deduplicated, keeping the best-ranked one. Every
// returned cut contains at least one corrected edge, so a fully legal input
// with no correction reading yields an empty vector (the caller owns the plain
// segmentation). This is the query-time disambiguation surface of CN 101133411
// B: ambiguity survives the cut layer and is settled by dictionary frequency.
// k is a policy knob: the production caller pins kAutocorrectCutKBest (see
// quanpin_dictionary.cpp) tuned by evaluation; tests use the small default
// below to keep their assertions focused.
std::vector<AutocorrectCut> autocorrect_cut_kbest(const std::string &pinyin, unsigned autocorrect_types,
                                                  std::size_t k = 3);

// True when the input reads as one or more legal syllables plus at most one trailing
// letter ("zheg" = zhe + g): a jianpin-intent shape the correction tables must not
// rewrite. Deliberately NOT true for all-consonant strings of 3+ letters: the engine
// has no multi-letter jianpin, so correction is the only useful reading of e.g.
// "bqng" -> bang. Inputs with manual delimiters return false; the correction path
// excludes them on its own.
bool looks_like_syllable_with_jianpin_tail(const std::string &pinyin);

} // namespace quanpin
