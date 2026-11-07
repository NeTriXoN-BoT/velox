// Presentation-only translations. Never translate commands, preference keys or user content.
#pragma once
#include <string>
namespace phone::i18n {
void language(int value);
int language();
const wchar_t *text(const wchar_t *value);
std::wstring tr(const std::wstring &value);
} // namespace phone::i18n
