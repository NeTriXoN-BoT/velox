#include "localization.hpp"
#include <algorithm>
#include <atomic>
#include <iterator>
#include <string_view>
namespace phone::i18n {
namespace {
std::atomic<int> selected{0};
struct Entry {
    std::wstring_view english, german;
};
// Sorted at generation time. The immutable table requires no allocation, locks
// or startup construction; every returned pointer has static lifetime.
constexpr Entry catalog[] = {
#include "localization.inc"
};
} // namespace
void language(int value) {
    selected.store(value == 1 ? 1 : 0, std::memory_order_relaxed);
}
int language() {
    return selected.load(std::memory_order_relaxed);
}
std::wstring tr(const std::wstring &value) {
    if (language() == 1 && value.starts_with(L"Phone ") && value.size() > 6 && value.size() < 12 &&
        std::all_of(value.begin() + 6, value.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
        return L"Handy " + value.substr(6);
    return text(value.c_str());
}
const wchar_t *text(const wchar_t *value) {
    if (!value || language() == 0)
        return value;
    std::wstring_view key(value);
    auto found = std::lower_bound(std::begin(catalog), std::end(catalog), key,
                                  [](const Entry &row, std::wstring_view input) { return row.english < input; });
    return found == std::end(catalog) || found->english != key ? value : found->german.data();
}
} // namespace phone::i18n
