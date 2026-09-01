#pragma once

#include <string>
#include <string_view>

namespace tilde {

struct OllamaResult {
    std::string suggestion;
    std::string error;
    long statusCode = 0;
};

std::string buildOllamaRequest(std::string_view model,
                               std::string_view prefix);
std::string parseOllamaSuggestion(std::string_view responseBody);
std::string sanitizeSuggestion(std::string suggestion);

class OllamaClient {
public:
    OllamaClient(
        std::string endpoint = "http://127.0.0.1:11434/api/generate",
        std::string model = "qwen2.5-coder:1.5b-base");

    OllamaResult complete(std::string_view prefix) const;

private:
    std::string endpoint_;
    std::string model_;
};

} // namespace tilde
