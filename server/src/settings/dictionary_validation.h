#pragma once

#include "MetasequoiaImeEngine/quanpin/quanpin_query.h"

#include <cstddef>
#include <string>

namespace SettingsDictionary::Validation
{
bool NormalizeFullPinyin(const std::string &input, quanpin::Segments &segments, std::string &normalized,
                         std::size_t expected_syllables = 0);
bool ParseCodedImportLine(const std::string &line, std::string &word, std::string &code, int &weight,
                          std::string &message);
bool QuickPhraseFitsNamedPipe(const std::string &phrase);
} // namespace SettingsDictionary::Validation
