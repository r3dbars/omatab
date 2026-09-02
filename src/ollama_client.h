#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <curl/curl.h>

namespace omatab {

struct OllamaResult {
    std::string suggestion;
    std::string error;
    std::string model;
    std::string requestJson;
    std::string responseJson;
    double latencyMs = 0.0;
    long statusCode = 0;
    bool cancelled = false;
};

// The "balanced" catalog entry: a 4B base model with native FIM tokens.
inline constexpr const char *kDefaultModel =
    "hf.co/mradermacher/Qwen3.5-4B-Base-GGUF:Q8_0";

std::string buildOllamaRequest(std::string_view model,
                               std::string_view prefix,
                               std::string_view suffix = {});
std::string parseOllamaSuggestion(std::string_view responseBody);
std::string sanitizeSuggestion(std::string suggestion);
// Cuts a continuation down to the smallest useful unit: the rest of the
// current clause. Stops after sentence-ending punctuation, or after a comma,
// semicolon, or colon once at least one word has been offered. Keeps decimals
// and abbreviations intact by requiring whitespace or end of text after the
// mark.
std::string limitToClause(std::string suggestion);
// Last check before a suggestion is shown. Returns an empty string when the
// suggestion is worth showing, otherwise a short reason: "too_short" (under
// three characters), "no_content" (no letters or digits), "repeated_run"
// (the same character six or more times in a row), or "screen_line_echo"
// (a copy of an entire line of the visible screen, three words or longer).
// Reusing a phrase from the screen is deliberately allowed: replying "I
// would like to meet at seven" to "do you want to meet at seven?" is the
// point of screen context.
std::string suggestionRejection(std::string_view suggestion,
                                std::string_view visibleContext);
std::string buildOllamaContextRequest(std::string_view model,
                                      std::string_view prefix,
                                      std::string_view suffix,
                                      std::string_view visibleContext);
std::string ensureInsertionBoundary(std::string_view prefix,
                                    std::string suggestion);

class OllamaClient {
public:
    // Returns true when the in-flight request is no longer wanted. Polled by
    // the requesting thread between transfer steps; must be cheap.
    using CancelPredicate = std::function<bool()>;

    OllamaClient(
        std::string endpoint = "http://127.0.0.1:11434/api/generate",
        std::string model = kDefaultModel);
    ~OllamaClient();

    OllamaClient(const OllamaClient &) = delete;
    OllamaClient &operator=(const OllamaClient &) = delete;

    OllamaResult complete(std::string_view prefix,
                          std::string_view suffix = {},
                          const CancelPredicate &cancelled = {});
    OllamaResult completeWithContext(std::string_view prefix,
                                     std::string_view suffix,
                                     std::string_view visibleContext,
                                     const CancelPredicate &cancelled = {});

    // Wakes a request blocked in complete()/completeWithContext() so it
    // re-evaluates its cancel predicate immediately. Safe to call from any
    // thread while the client is alive.
    void interrupt();

private:
    OllamaResult perform(const std::string &endpoint,
                         const std::string &model, std::string requestBody,
                         const CancelPredicate &cancelled);

    std::string endpoint_;
    std::string model_;
    std::string contextEndpoint_ = "http://127.0.0.1:11434/api/generate";
    std::string contextModel_ = kDefaultModel;
    CURLM *multi_ = nullptr;
};

} // namespace omatab
