#include "core/text.hpp"

#include <algorithm>

namespace hoardor::core {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

}

const std::vector<std::string>& default_articles() {
    static const std::vector<std::string> articles{"the ", "a ", "an "};
    return articles;
}

std::string trim(std::string_view text) {
    while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
    return std::string(text);
}

std::string sort_key(std::string_view text, const std::vector<std::string>& articles) {
    std::string lowered;
    lowered.reserve(text.size());
    for (char c : trim(text)) lowered.push_back(lower(c));
    std::string_view view = lowered;
    for (const std::string& article : articles) {
        // Only when something follows the article: "The" alone stays "the".
        if (view.size() > article.size() && view.starts_with(article)) {
            view.remove_prefix(article.size());
            break;
        }
    }

    std::string key;
    key.reserve(view.size() + 8);
    std::size_t i = 0;
    while (i < view.size()) {
        if (!is_digit(view[i])) {
            key.push_back(view[i++]);
            continue;
        }
        std::size_t end = i;
        while (end < view.size() && is_digit(view[end])) ++end;
        std::size_t start = i;
        while (start + 1 < end && view[start] == '0') ++start;  // "007" -> "7", "0" stays "0"
        const std::size_t length = std::min<std::size_t>(end - start, 0x7F);
        // The length byte (1..127) sorts below every printable character, so numbers come before
        // letters, and a shorter number before a longer one. It's below 0x80, so it never looks
        // like part of a UTF-8 sequence.
        key.push_back(static_cast<char>(length));
        key.append(view.substr(start, end - start));
        i = end;
    }
    return key;
}

std::vector<std::string> split_values(std::string_view text, std::string_view separators) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find_first_of(separators, start);
        if (end == std::string_view::npos) end = text.size();
        std::string part = trim(text.substr(start, end - start));
        if (!part.empty()) {
            const auto same = [&](const std::string& existing) {
                return existing.size() == part.size() &&
                       std::equal(existing.begin(), existing.end(), part.begin(),
                                  [](char a, char b) { return lower(a) == lower(b); });
            };
            if (std::none_of(out.begin(), out.end(), same)) out.push_back(std::move(part));
        }
        start = end + 1;
    }
    return out;
}

}
