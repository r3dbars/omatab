#include "ollama_client.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

#include <curl/curl.h>
#include <json/json.h>

namespace {

constexpr std::size_t kMaximumSuggestionBytes = 160;

std::size_t appendResponse(char *data, std::size_t size, std::size_t count,
                           void *destination) {
    const auto bytes = size * count;
    static_cast<std::string *>(destination)->append(data, bytes);
    return bytes;
}

} // namespace

namespace tilde {

std::string buildOllamaRequest(std::string_view model,
                               std::string_view prefix,
                               std::string_view suffix) {
    Json::Value request;
    request["model"] = std::string(model);
    request["prompt"] = std::string(prefix);
    if (!suffix.empty()) {
        request["suffix"] = std::string(suffix);
    }
    request["stream"] = false;
    request["keep_alive"] = "30m";
    request["options"]["num_predict"] = 16;
    request["options"]["temperature"] = 0.2;
    request["options"]["top_p"] = 0.9;
    request["options"]["stop"].append("\n");

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, request);
}

std::string buildOllamaContextRequest(std::string_view model,
                                      std::string_view prefix,
                                      std::string_view suffix,
                                      std::string_view visibleContext) {
    Json::Value request;
    request["model"] = std::string(model);
    request["stream"] = false;
    request["think"] = false;
    request["keep_alive"] = "30m";
    request["options"]["num_predict"] = 16;
    request["options"]["temperature"] = 0.2;
    request["options"]["top_p"] = 0.9;
    request["options"]["stop"].append("\n");

    Json::Value system;
    system["role"] = "system";
    system["content"] =
        "You are an inline writing completion engine. Use the visible "
        "application context and text around the caret. Return only the short "
        "text that should be inserted at the caret. Never explain, quote the "
        "prompt, or add formatting.";
    request["messages"].append(system);

    Json::Value user;
    user["role"] = "user";
    user["content"] =
        "Visible active-window text:\n" + std::string(visibleContext) +
        "\n\nText before caret:\n" + std::string(prefix) +
        "\n\nText after caret:\n" + std::string(suffix) +
        "\n\nWrite only the continuation.";
    request["messages"].append(user);

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, request);
}

std::string sanitizeSuggestion(std::string suggestion) {
    suggestion.erase(
        std::remove(suggestion.begin(), suggestion.end(), '\r'),
        suggestion.end());
    if (const auto newline = suggestion.find('\n');
        newline != std::string::npos) {
        suggestion.erase(newline);
    }
    if (const auto specialToken = suggestion.find("<|");
        specialToken != std::string::npos) {
        suggestion.erase(specialToken);
    }
    if (suggestion.size() > kMaximumSuggestionBytes) {
        suggestion.resize(kMaximumSuggestionBytes);
    }
    if (suggestion.find_first_not_of(" \t") == std::string::npos) {
        suggestion.clear();
    }
    return suggestion;
}

std::string parseOllamaSuggestion(std::string_view responseBody) {
    Json::CharReaderBuilder reader;
    Json::Value response;
    std::string errors;
    std::istringstream input{std::string(responseBody)};
    if (!Json::parseFromStream(reader, input, &response, &errors) ||
        !response["response"].isString()) {
        return {};
    }
    return sanitizeSuggestion(response["response"].asString());
}

std::string ensureInsertionBoundary(std::string_view prefix,
                                    std::string suggestion) {
    if (prefix.empty() || suggestion.empty()) {
        return suggestion;
    }
    const auto left = static_cast<unsigned char>(prefix.back());
    const auto right = static_cast<unsigned char>(suggestion.front());
    if (!std::isspace(left) && !std::isspace(right) &&
        (std::isalnum(left) || left >= 0x80U) &&
        (std::isalnum(right) || right >= 0x80U)) {
        suggestion.insert(suggestion.begin(), ' ');
    }
    return suggestion;
}

OllamaClient::OllamaClient(std::string endpoint, std::string model)
    : endpoint_(std::move(endpoint)), model_(std::move(model)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

OllamaResult OllamaClient::complete(std::string_view prefix,
                                    std::string_view suffix) const {
    OllamaResult result;
    auto *curl = curl_easy_init();
    if (!curl) {
        result.error = "curl initialization failed";
        return result;
    }

    std::string responseBody;
    const auto requestBody = buildOllamaRequest(model_, prefix, suffix);
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(requestBody.size()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 150L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2500L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

    const auto status = curl_easy_perform(curl);
    if (status == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.statusCode);
        if (result.statusCode == 200) {
            result.suggestion = parseOllamaSuggestion(responseBody);
        } else {
            result.error = "Ollama returned HTTP " +
                           std::to_string(result.statusCode);
        }
    } else {
        result.error = curl_easy_strerror(status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

OllamaResult OllamaClient::completeWithContext(
    std::string_view prefix, std::string_view suffix,
    std::string_view visibleContext) const {
    OllamaResult result;
    auto *curl = curl_easy_init();
    if (!curl) {
        result.error = "curl initialization failed";
        return result;
    }

    std::string responseBody;
    const auto requestBody = buildOllamaContextRequest(
        contextModel_, prefix, suffix, visibleContext);
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, contextEndpoint_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(requestBody.size()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 150L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2500L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

    const auto status = curl_easy_perform(curl);
    if (status == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.statusCode);
        if (result.statusCode == 200) {
            Json::CharReaderBuilder reader;
            Json::Value response;
            std::string errors;
            std::istringstream input(responseBody);
            if (Json::parseFromStream(reader, input, &response, &errors) &&
                response["message"]["content"].isString()) {
                result.suggestion = ensureInsertionBoundary(
                    prefix, sanitizeSuggestion(
                                response["message"]["content"].asString()));
            }
        } else {
            result.error = "Ollama returned HTTP " +
                           std::to_string(result.statusCode);
        }
    } else {
        result.error = curl_easy_strerror(status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

} // namespace tilde
