#include "ollama_client.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <utility>

#include <curl/curl.h>
#include <json/json.h>

namespace {

constexpr std::size_t kMaximumSuggestionBytes = 160;

long configuredInteger(const char *name, long fallback, long minimum,
                       long maximum) {
    const auto *value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char *end = nullptr;
    errno = 0;
    const auto parsed = std::strtol(value, &end, 10);
    return errno == 0 && end && *end == '\0' && parsed >= minimum &&
                   parsed <= maximum
               ? parsed
               : fallback;
}

double configuredNumber(const char *name, double fallback, double minimum,
                        double maximum) {
    const auto *value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char *end = nullptr;
    errno = 0;
    const auto parsed = std::strtod(value, &end);
    return errno == 0 && end && *end == '\0' && parsed >= minimum &&
                   parsed <= maximum
               ? parsed
               : fallback;
}

Json::Value configuredKeepAlive() {
    const auto *value = std::getenv("TILDE_KEEP_ALIVE");
    if (!value || !*value) {
        return "30m";
    }
    const std::string configured(value);
    if (configured == "-1") {
        return Json::Value(-1);
    }
    if (configured == "0") {
        return Json::Value(0);
    }
    return configured.find_first_not_of("-0123456789smh") == std::string::npos
               ? Json::Value(configured)
               : Json::Value("30m");
}

bool configuredFim() {
    const auto *value = std::getenv("TILDE_FIM");
    return value && (std::string_view(value) == "1" ||
                     std::string_view(value) == "true");
}

std::string fimPrompt(std::string_view prefix, std::string_view suffix) {
    return "<|fim_prefix|>" + std::string(prefix) + "<|fim_suffix|>" +
           std::string(suffix) + "<|fim_middle|>";
}

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
    if (configuredFim()) {
        request["prompt"] = fimPrompt(prefix, suffix);
    } else {
        request["prompt"] = std::string(prefix);
        if (!suffix.empty()) {
            request["suffix"] = std::string(suffix);
        }
    }
    request["raw"] = true;
    request["stream"] = false;
    request["keep_alive"] = configuredKeepAlive();
    request["options"]["num_predict"] = static_cast<Json::Int64>(
        configuredInteger("TILDE_NUM_PREDICT", 16, 1, 64));
    request["options"]["num_ctx"] = static_cast<Json::Int64>(
        configuredInteger("TILDE_NUM_CTX", 8192, 1024, 32768));
    request["options"]["temperature"] =
        configuredNumber("TILDE_TEMPERATURE", 0.2, 0.0, 2.0);
    request["options"]["top_p"] =
        configuredNumber("TILDE_TOP_P", 0.9, 0.0, 1.0);
    request["options"]["stop"].append("\n");
    request["options"]["stop"].append("<|fim_pad|>");
    request["options"]["stop"].append("<|endoftext|>");

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
    const auto contextualPrefix =
        "Reference text from the active window (background only):\n" +
        std::string(visibleContext) +
        "\n\nText currently being written (continue from its final character):\n" +
        std::string(prefix);
    if (configuredFim()) {
        request["prompt"] = fimPrompt(contextualPrefix, suffix);
    } else {
        request["prompt"] = contextualPrefix;
        if (!suffix.empty()) {
            request["suffix"] = std::string(suffix);
        }
    }
    request["raw"] = true;
    request["stream"] = false;
    request["keep_alive"] = configuredKeepAlive();
    request["options"]["num_predict"] = static_cast<Json::Int64>(
        configuredInteger("TILDE_NUM_PREDICT", 16, 1, 64));
    request["options"]["num_ctx"] = static_cast<Json::Int64>(
        configuredInteger("TILDE_NUM_CTX", 8192, 1024, 32768));
    request["options"]["temperature"] =
        configuredNumber("TILDE_TEMPERATURE", 0.2, 0.0, 2.0);
    request["options"]["top_p"] =
        configuredNumber("TILDE_TOP_P", 0.9, 0.0, 1.0);
    request["options"]["stop"].append("\n");
    request["options"]["stop"].append("<think>");
    request["options"]["stop"].append("<|fim_pad|>");
    request["options"]["stop"].append("<|endoftext|>");

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
    if (const auto thinking = suggestion.find("<think>");
        thinking != std::string::npos) {
        suggestion.erase(thinking);
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
    if (const auto *configuredModel = std::getenv("TILDE_MODEL");
        configuredModel && *configuredModel) {
        model_ = configuredModel;
    }
    if (const auto *configuredContextModel =
            std::getenv("TILDE_CONTEXT_MODEL");
        configuredContextModel && *configuredContextModel) {
        contextModel_ = configuredContextModel;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

OllamaResult OllamaClient::complete(std::string_view prefix,
                                    std::string_view suffix) const {
    OllamaResult result;
    result.model = model_;
    auto *curl = curl_easy_init();
    if (!curl) {
        result.error = "curl initialization failed";
        return result;
    }

    std::string responseBody;
    const auto requestBody = buildOllamaRequest(model_, prefix, suffix);
    result.requestJson = requestBody;
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(requestBody.size()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 150L);
    curl_easy_setopt(
        curl, CURLOPT_TIMEOUT_MS,
        configuredInteger("TILDE_TIMEOUT_MS", 2500, 250, 10000));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

    const auto status = curl_easy_perform(curl);
    double totalSeconds = 0.0;
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &totalSeconds);
    result.latencyMs = totalSeconds * 1000.0;
    result.responseJson = responseBody;
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
    result.model = contextModel_;
    auto *curl = curl_easy_init();
    if (!curl) {
        result.error = "curl initialization failed";
        return result;
    }

    std::string responseBody;
    const auto requestBody = buildOllamaContextRequest(
        contextModel_, prefix, suffix, visibleContext);
    result.requestJson = requestBody;
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, contextEndpoint_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(requestBody.size()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 150L);
    curl_easy_setopt(
        curl, CURLOPT_TIMEOUT_MS,
        configuredInteger("TILDE_TIMEOUT_MS", 2500, 250, 10000));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

    const auto status = curl_easy_perform(curl);
    double totalSeconds = 0.0;
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &totalSeconds);
    result.latencyMs = totalSeconds * 1000.0;
    result.responseJson = responseBody;
    if (status == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.statusCode);
        if (result.statusCode == 200) {
            result.suggestion = parseOllamaSuggestion(responseBody);
            if (!configuredFim()) {
                result.suggestion = ensureInsertionBoundary(
                    prefix, std::move(result.suggestion));
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
