#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <curl/curl.h>

namespace tilde {

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

std::string buildOllamaRequest(std::string_view model,
                               std::string_view prefix,
                               std::string_view suffix = {});
std::string parseOllamaSuggestion(std::string_view responseBody);
std::string sanitizeSuggestion(std::string suggestion);
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
        std::string model = "qwen2.5-coder:1.5b-base");
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
    std::string contextModel_ = "qwen2.5:1.5b";
    CURLM *multi_ = nullptr;
};

} // namespace tilde
