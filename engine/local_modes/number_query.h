#pragma once

#include "../core/word_item.h"

#include <string>
#include <vector>

namespace metasequoia::local_modes
{
// `number_part` is the text after the leading V: one or more decimal digits
// with at most one decimal point, for example "123456", "1234.5" or "12.".
// A trailing decimal point is accepted so the composition stays valid while
// the fraction is still being typed.
bool is_number_text(const std::string &number_part);

// Renders the number as Chinese candidates: the financial (人民币大写) amount
// first, then the uppercase and lowercase readings, digit-by-digit readings
// and the Arabic form grouped by three (123,456) and by four digits (12,3456).
// The financial amount is omitted when the fraction has more than two digits
// (角 / 分 only).
std::vector<WordItem> query_number(const std::string &number_part, int limit = 8);
} // namespace metasequoia::local_modes
