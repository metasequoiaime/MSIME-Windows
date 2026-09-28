#pragma once

#include "input_scheme.h"
#include <string>
#include <vector>

// JIS 106/109 direct-kana input (ATOK / MS-IME かな入力). Each physical key
// maps straight to one hiragana; raw_input therefore already holds kana and
// needs no romaji conversion.
class JapaneseKanaScheme : public IInputScheme
{
  public:
    void reset() override;
    void handle_key(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch) override;
    QueryRequest build_request() const override;
    std::string get_preedit() const override;
    SchemeType type() const override;
    void set_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases) override;

  private:
    std::string raw_input_; // hiragana (plus ー / ヴ) already, UTF-8
    std::vector<KeyStroke> key_strokes_;
};
