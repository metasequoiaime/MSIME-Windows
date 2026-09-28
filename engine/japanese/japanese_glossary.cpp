#include "japanese_glossary.h"

#include <utf8/cpp17.h>

#include <unordered_map>
#include <algorithm>

namespace japanese
{
namespace
{
// Long-vowel-mark normalization (U+30FC ー) so model surface variants such as
// コンピュータ and コンピューター resolve to the same gloss entry.
std::string NormalizeLongVowel(std::string_view s)
{
    std::u32string codepoints = utf8::utf8to32(std::string(s));
    std::u32string kept;
    kept.reserve(codepoints.size());
    for (char32_t cp : codepoints)
    {
        if (cp != U'ー')
            kept.push_back(cp);
    }
    return utf8::utf32to8(kept);
}

// surface -> already-formatted preview. Built once; shared by every query.
const std::unordered_map<std::string, std::string> &GlossTable()
{
    static const std::unordered_map<std::string, std::string> table = [] {
        std::unordered_map<std::string, std::string> m;
        // Index both the raw surface and its long-vowel-normalized form so
        // model variants with/without ー (コンピューター / コンピュータ) match.
        const auto add = [&m](const std::string &surface, const std::string &gloss) {
            m.emplace(surface, gloss);
            m.emplace(NormalizeLongVowel(surface), gloss);
        };

        // Slang full forms carry no language tag.
        for (const auto &entry : InternetSlangEntries())
            add(entry.surface, entry.gloss);

        // Katakana loanwords: original word with source-language tag.
        for (const auto &loan : LoanwordEntries())
            add(loan.surface, loan.original + "\xEF\xBC\x88" + loan.lang + "\xEF\xBC\x89"); // （tag）

        return m;
    }();
    return table;
}
} // namespace

const std::vector<SlangEntry> &InternetSlangEntries()
{
    static const std::vector<SlangEntry> entries = {
        {"w", "w", "笑い（laugh）"},
        {"kwsk", "kwsk", "くわしくはこちら"},
        {"ktkr", "キタコレ", "来たこれ"},
        {"ggrks", "ggrks", "ググれカス"},
        {"wktk", "ワクテカ", "ワクワクテカテカ"},
        {"orz", "orz", "失意・落胆のポーズ"},
        {"akeome", "あけおめ", "あけましておめでとう"},
        {"kotoyoro", "ことよろ", "今年もよろしく"},
        {"otsu", "おつ", "お疲れさま"},
        {"mochituke", "もちつけ", "落ち着け"},
        {"ok", "おk", "オーケー（了解）"},
        {"jk", "jk", "女子高生"},
        {"ryo", "りょ", "了解"},
        {"ry", "ry", "略（りゃく）"},
        {"noshi", "ノシ", "さよなら（手を振る）"},
        {"up", "うp", "アップロード"},
        {"gg", "gg", "good game"},
        {"pien", "ぴえん", "悲しい・泣きたい気分"},
    };
    return entries;
}

const std::vector<LoanEntry> &LoanwordEntries()
{
    // Etymologies follow the commonly accepted source language of each
    // katakana word (many everyday "English-looking" loans actually entered
    // Japanese via Dutch, Portuguese, French or German).
    static const std::vector<LoanEntry> entries = {
        // ---- English ----
        {"パソコン", "personal computer", "英"},
        {"コンピューター", "computer", "英"},
        {"スマートフォン", "smartphone", "英"},
        {"スマホ", "smartphone", "英"},
        {"タブレット", "tablet", "英"},
        {"テレビ", "television / TV", "英"},
        {"ラジオ", "radio", "英"},
        {"カメラ", "camera", "英"},
        {"ビデオ", "video", "英"},
        {"エレベーター", "elevator", "英"},
        {"エスカレーター", "escalator", "英"},
        {"ケーキ", "cake", "英"},
        {"アイスクリーム", "ice cream", "英"},
        {"チョコレート", "chocolate", "英"},
        {"コーラ", "cola", "英"},
        {"ジュース", "juice", "英"},
        {"ミルク", "milk", "英"},
        {"チーズ", "cheese", "英"},
        {"バター", "butter", "英"},
        {"サラダ", "salad", "英"},
        {"スープ", "soup", "英"},
        {"ハンバーガー", "hamburger", "英"},
        {"サンドイッチ", "sandwich", "英"},
        {"ピザ", "pizza", "英"},
        {"パスタ", "pasta", "英"},
        {"レモン", "lemon", "英"},
        {"バナナ", "banana", "英"},
        {"オレンジ", "orange", "英"},
        {"ホテル", "hotel", "英"},
        {"スーパー", "supermarket", "英"},
        {"コンビニ", "convenience store", "英"},
        {"デパート", "department store", "英"},
        {"タクシー", "taxi", "英"},
        {"バス", "bus", "英"},
        {"トイレ", "toilet", "英"},
        {"シャワー", "shower", "英"},
        {"タオル", "towel", "英"},
        {"ベッド", "bed", "英"},
        {"ソファ", "sofa", "英"},
        {"テーブル", "table", "英"},
        {"ドア", "door", "英"},
        {"グラス", "glass", "英"},
        {"ボタン", "botão", "葡"},
        {"カーテン", "curtain", "英"},
        {"ランプ", "lamp", "英"},
        {"アパート", "apartment", "英"},
        {"エアコン", "air conditioner", "英"},
        {"コピー", "copy", "英"},
        {"プリンター", "printer", "英"},
        {"インターネット", "internet", "英"},
        {"メール", "email", "英"},
        {"ニュース", "news", "英"},
        {"ドラマ", "drama", "英"},
        {"アニメ", "animation", "英"},
        {"コンサート", "concert", "英"},
        {"チケット", "ticket", "英"},
        {"サービス", "service", "英"},
        {"デート", "date", "英"},
        {"キス", "kiss", "英"},
        {"ハグ", "hug", "英"},
        {"チャンス", "chance", "英"},
        {"ピンチ", "pinch", "英"},
        {"センター", "center", "英"},
        {"スタート", "start", "英"},
        {"ゴール", "goal", "英"},
        {"プレゼント", "present", "英"},
        {"メッセージ", "message", "英"},
        {"ポケット", "pocket", "英"},
        {"バッグ", "bag", "英"},
        {"メガネ", "glasses", "英"},
        {"シャツ", "shirt", "英"},
        {"スカート", "skirt", "英"},
        {"ズボン", "trousers", "英"},
        {"コート", "coat", "英"},
        {"セーター", "sweater", "英"},
        {"ブーツ", "boots", "英"},
        {"サッカー", "soccer", "英"},
        {"テニス", "tennis", "英"},
        {"ゴルフ", "golf", "英"},
        {"スキー", "ski", "英"},
        {"マラソン", "marathon", "英"},
        {"ジョギング", "jogging", "英"},
        {"ハイキング", "hiking", "英"},
        {"スポーツ", "sports", "英"},
        {"チーム", "team", "英"},
        {"ゲーム", "game", "英"},
        {"ジム", "gym", "英"},
        {"ヨガ", "yoga", "英"},
        {"ワイン", "wine", "英"},

        // ---- Dutch (the oldest European layer, via Dejima) ----
        {"コーヒー", "koffie", "荷"},
        {"ビール", "bier", "荷"},
        {"ガラス", "glas", "荷"},
        {"ゴム", "gom", "荷"},
        {"コップ", "kop", "荷"},
        {"ランドセル", "ransel", "荷"},
        {"ヨット", "jacht", "荷"},
        {"インキ", "inkt", "荷"},
        {"ペンキ", "pek / verf", "荷"},
        {"アルコール", "alcohol", "荷"},

        // ---- Portuguese (16th-century Nanban trade) ----
        {"パン", "pão", "葡"},
        {"タバコ", "tabaco", "葡"},
        {"カッパ", "capa", "葡"},
        {"カルタ", "carta", "葡"},
        {"カステラ", "castela", "葡"},
        {"コンペイトー", "confeito", "葡"},
        {"テンプラ", "tempero", "葡"},

        // ---- French ----
        {"レストラン", "restaurant", "法"},
        {"カフェ", "café", "法"},
        {"デビュー", "début", "法"},
        {"アンケート", "enquête", "法"},
        {"ルージュ", "rouge", "法"},
        {"ブティック", "boutique", "法"},
        {"デッサン", "dessin", "法"},
        {"アトリエ", "atelier", "法"},
        {"エチケット", "étiquette", "法"},
        {"ガレージ", "garage", "法"},
        {"マヨネーズ", "mayonnaise", "法"},
        {"クレヨン", "crayon", "法"},
        {"シフォン", "chiffon", "法"},
        {"ショー", "show", "英"},

        // ---- German ----
        {"アルバイト", "Arbeit", "德"},
        {"ガーゼ", "Gaze", "德"},
        {"リュックサック", "Rucksack", "德"},
        {"カルテ", "Karte", "德"},
        {"ボンベ", "Bombe", "德"},
        {"エネルギー", "Energie", "德"},
        {"ワクチン", "Vakzine", "德"},
        {"リハビリ", "Rehabilitation", "德"},
        {"メルヘン", "Märchen", "德"},
        {"ヒステリー", "Hysterie", "德"},
        {"タクト", "Takt", "德"},
    };
    return entries;
}

std::string LookUpCandidateGloss(std::string_view surface)
{
    const auto &table = GlossTable();
    if (auto it = table.find(std::string(surface)); it != table.end())
        return it->second;
    const std::string normalized = NormalizeLongVowel(surface);
    if (normalized != surface)
    {
        if (auto it = table.find(normalized); it != table.end())
            return it->second;
    }
    return {};
}
} // namespace japanese
