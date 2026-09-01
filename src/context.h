#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace omatab {

struct ContextWindow {
    std::string prefix;
    std::string suffix;
    bool fromSurroundingText = false;
};

inline std::size_t utf8SequenceLength(unsigned char lead) {
    if ((lead & 0x80U) == 0) {
        return 1;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 1;
}

inline std::size_t byteOffsetForCharacters(std::string_view text,
                                           std::size_t characterOffset) {
    std::size_t byteOffset = 0;
    std::size_t characters = 0;
    while (byteOffset < text.size() && characters < characterOffset) {
        const auto length = utf8SequenceLength(
            static_cast<unsigned char>(text[byteOffset]));
        byteOffset += std::min(length, text.size() - byteOffset);
        ++characters;
    }
    return byteOffset;
}

inline std::string keepLastUtf8Bytes(std::string_view text,
                                     std::size_t maximumBytes) {
    if (text.size() <= maximumBytes) {
        return std::string(text);
    }
    auto start = text.size() - maximumBytes;
    while (start < text.size() &&
           (static_cast<unsigned char>(text[start]) & 0xC0U) == 0x80U) {
        ++start;
    }
    return std::string(text.substr(start));
}

inline std::string keepFirstUtf8Bytes(std::string_view text,
                                      std::size_t maximumBytes) {
    if (text.size() <= maximumBytes) {
        return std::string(text);
    }
    std::size_t end = 0;
    while (end < text.size()) {
        const auto length = utf8SequenceLength(
            static_cast<unsigned char>(text[end]));
        if (end + length > maximumBytes || end + length > text.size()) {
            break;
        }
        end += length;
    }
    return std::string(text.substr(0, end));
}

inline ContextWindow buildContextWindow(
    std::string_view surroundingText, std::size_t cursorCharacters,
    bool surroundingTextValid, std::string_view trackedFallback,
    std::size_t maximumPrefixBytes = 4096,
    std::size_t maximumSuffixBytes = 1024) {
    if (!surroundingTextValid) {
        return {keepLastUtf8Bytes(trackedFallback, maximumPrefixBytes), {},
                false};
    }

    const auto cursorByte =
        byteOffsetForCharacters(surroundingText, cursorCharacters);
    return {keepLastUtf8Bytes(surroundingText.substr(0, cursorByte),
                              maximumPrefixBytes),
            keepFirstUtf8Bytes(surroundingText.substr(cursorByte),
                               maximumSuffixBytes),
            true};
}

} // namespace omatab
