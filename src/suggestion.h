#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace tilde {

constexpr bool isAsciiWordCharacter(unsigned char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') || character == '\'' ||
           character == '-';
}

inline std::size_t nextWordLength(std::string_view suggestion) {
    std::size_t cursor = 0;

    // Keep leading spaces and punctuation attached to the next actual word.
    while (cursor < suggestion.size() &&
           !std::isalnum(static_cast<unsigned char>(suggestion[cursor]))) {
        ++cursor;
    }

    while (cursor < suggestion.size() &&
           isAsciiWordCharacter(
               static_cast<unsigned char>(suggestion[cursor]))) {
        ++cursor;
    }

    // Accept punctuation attached to the word, but stop before another word.
    while (cursor < suggestion.size() &&
           !std::isalnum(static_cast<unsigned char>(suggestion[cursor])) &&
           !std::isspace(static_cast<unsigned char>(suggestion[cursor]))) {
        ++cursor;
    }

    while (cursor < suggestion.size() &&
           std::isspace(static_cast<unsigned char>(suggestion[cursor]))) {
        ++cursor;
    }

    return cursor == 0 ? suggestion.size() : cursor;
}

inline bool suggestionRequestIsCurrent(std::uint64_t currentRevision,
                                       std::string_view currentPrefix,
                                       std::uint64_t requestRevision,
                                       std::string_view requestPrefix) {
    return currentRevision == requestRevision && currentPrefix == requestPrefix;
}

} // namespace tilde
