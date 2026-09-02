#include "ollama_client.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <utility>

#include <curl/curl.h>
#include <json/json.h>

namespace {

constexpr std::size_t kMaximumSuggestionBytes = 160;
// Fallback wake interval when no interrupt() arrives; keeps a lost wakeup
// from stalling cancellation for long.
constexpr int kPollTimeoutMs = 100;

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
    const auto *value = std::getenv("OMATAB_KEEP_ALIVE");
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

// FIM is on by default because the default model supports it. Set
// OMATAB_FIM=0 for models without fill-in-the-middle tokens.
bool configuredFim() {
    const auto *value = std::getenv("OMATAB_FIM");
    if (!value || !*value) {
        return true;
    }
    return std::string_view(value) != "0" &&
           std::string_view(value) != "false";
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

namespace omatab {

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
        configuredInteger("OMATAB_NUM_PREDICT", 16, 1, 64));
    request["options"]["num_ctx"] = static_cast<Json::Int64>(
        configuredInteger("OMATAB_NUM_CTX", 8192, 1024, 32768));
    request["options"]["temperature"] =
        configuredNumber("OMATAB_TEMPERATURE", 0.2, 0.0, 2.0);
    request["options"]["top_p"] =
        configuredNumber("OMATAB_TOP_P", 0.9, 0.0, 1.0);
    request["options"]["stop"].append("\n");
    request["options"]["stop"].append("<|fim_pad|>");
    request["options"]["stop"].append("<|endoftext|>");

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, request);
}

namespace {

std::string_view trimView(std::string_view text) {
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

// Lowercases ASCII and collapses whitespace runs so an OCR line and a model
// suggestion compare equal despite spacing or case differences.
std::string foldForComparison(std::string_view text) {
    std::string folded;
    folded.reserve(text.size());
    bool pendingSpace = false;
    for (unsigned char c : trimView(text)) {
        if (std::isspace(c)) {
            pendingSpace = true;
            continue;
        }
        if (pendingSpace) {
            folded.push_back(' ');
            pendingSpace = false;
        }
        folded.push_back(static_cast<char>(std::tolower(c)));
    }
    return folded;
}

std::size_t wordCount(std::string_view text) {
    std::size_t words = 0;
    bool inWord = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            inWord = false;
        } else if (!inWord) {
            inWord = true;
            ++words;
        }
    }
    return words;
}

} // namespace

std::string suggestionRejection(std::string_view suggestion,
                                std::string_view visibleContext) {
    constexpr std::size_t kMinimumLength = 3;
    constexpr std::size_t kMaximumRun = 5;
    constexpr std::size_t kEchoMinimumWords = 3;

    const auto trimmed = trimView(suggestion);
    if (trimmed.size() < kMinimumLength) {
        return "too_short";
    }
    // Any non-ASCII byte counts as content so accented words and emoji pass.
    const bool hasContent = std::any_of(
        trimmed.begin(), trimmed.end(), [](unsigned char c) {
            return c >= 0x80 || std::isalnum(c);
        });
    if (!hasContent) {
        return "no_content";
    }
    std::size_t run = 0;
    char previous = '\0';
    for (char c : trimmed) {
        if (c == previous && !std::isspace(static_cast<unsigned char>(c))) {
            if (++run > kMaximumRun) {
                return "repeated_run";
            }
        } else {
            run = 1;
            previous = c;
        }
    }
    if (!visibleContext.empty() && wordCount(trimmed) >= kEchoMinimumWords) {
        const auto folded = foldForComparison(trimmed);
        std::size_t start = 0;
        while (start <= visibleContext.size()) {
            auto end = visibleContext.find('\n', start);
            if (end == std::string_view::npos) {
                end = visibleContext.size();
            }
            if (foldForComparison(
                    visibleContext.substr(start, end - start)) == folded) {
                return "screen_line_echo";
            }
            start = end + 1;
        }
    }
    return {};
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
        configuredInteger("OMATAB_NUM_PREDICT", 16, 1, 64));
    request["options"]["num_ctx"] = static_cast<Json::Int64>(
        configuredInteger("OMATAB_NUM_CTX", 8192, 1024, 32768));
    request["options"]["temperature"] =
        configuredNumber("OMATAB_TEMPERATURE", 0.2, 0.0, 2.0);
    request["options"]["top_p"] =
        configuredNumber("OMATAB_TOP_P", 0.9, 0.0, 1.0);
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

std::string limitToClause(std::string suggestion) {
    const auto isSentenceEnd = [](char c) {
        return c == '.' || c == '!' || c == '?';
    };
    const auto isClauseEnd = [](char c) {
        return c == ',' || c == ';' || c == ':';
    };
    const auto isClosing = [](char c) {
        return c == '"' || c == '\'' || c == ')' || c == ']' || c == '}';
    };
    const auto boundaryAfter = [&suggestion](std::size_t index) {
        return index + 1 >= suggestion.size() ||
               std::isspace(static_cast<unsigned char>(suggestion[index + 1]));
    };
    // "e.g. the", "U.S. army", "Dr. who": the mark belongs to an
    // abbreviation when the token before it is a single letter or already
    // dotted, or when the next word starts in lowercase.
    const auto looksLikeAbbreviation = [&suggestion](std::size_t markIndex,
                                                     std::size_t endIndex) {
        auto tokenStart = markIndex;
        while (tokenStart > 0 &&
               !std::isspace(
                   static_cast<unsigned char>(suggestion[tokenStart - 1]))) {
            --tokenStart;
        }
        const auto token = std::string_view(suggestion).substr(
            tokenStart, markIndex - tokenStart);
        if (token.size() == 1 && std::isalpha(static_cast<unsigned char>(token[0]))) {
            return true;
        }
        if (token.find('.') != std::string_view::npos) {
            return true;
        }
        constexpr std::array knownAbbreviations{
            "mr", "mrs", "ms", "dr", "prof", "sr", "jr", "st", "vs",
            "etc", "no", "fig", "approx", "dept", "inc", "ltd", "co"};
        std::string lowered(token);
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        if (std::find(knownAbbreviations.begin(), knownAbbreviations.end(),
                      lowered) != knownAbbreviations.end()) {
            return true;
        }
        auto next = endIndex + 1;
        while (next < suggestion.size() &&
               std::isspace(static_cast<unsigned char>(suggestion[next]))) {
            ++next;
        }
        return next < suggestion.size() &&
               std::islower(static_cast<unsigned char>(suggestion[next]));
    };

    bool sawWord = false;
    for (std::size_t i = 0; i < suggestion.size(); ++i) {
        const auto c = suggestion[i];
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            static_cast<unsigned char>(c) >= 0x80U) {
            sawWord = true;
            continue;
        }
        if (isSentenceEnd(c)) {
            auto end = i;
            while (end + 1 < suggestion.size() &&
                   isSentenceEnd(suggestion[end + 1])) {
                ++end;
            }
            while (end + 1 < suggestion.size() &&
                   isClosing(suggestion[end + 1])) {
                ++end;
            }
            if (boundaryAfter(end) &&
                !(c == '.' && looksLikeAbbreviation(i, end))) {
                suggestion.resize(end + 1);
                break;
            }
            i = end;
            continue;
        }
        if (sawWord && isClauseEnd(c) && boundaryAfter(i)) {
            suggestion.resize(i + 1);
            break;
        }
    }

    // An em dash starts a new clause; stop before it.
    if (const auto dash = suggestion.find("\xE2\x80\x94");
        dash != std::string::npos && sawWord) {
        suggestion.resize(dash);
        while (!suggestion.empty() &&
               std::isspace(static_cast<unsigned char>(suggestion.back()))) {
            suggestion.pop_back();
        }
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
    if (const auto *configuredModel = std::getenv("OMATAB_MODEL");
        configuredModel && *configuredModel) {
        model_ = configuredModel;
    }
    if (const auto *configuredContextModel =
            std::getenv("OMATAB_CONTEXT_MODEL");
        configuredContextModel && *configuredContextModel) {
        contextModel_ = configuredContextModel;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    multi_ = curl_multi_init();
}

OllamaClient::~OllamaClient() {
    if (multi_) {
        curl_multi_cleanup(multi_);
    }
}

void OllamaClient::interrupt() {
    if (multi_) {
        curl_multi_wakeup(multi_);
    }
}

OllamaResult OllamaClient::complete(std::string_view prefix,
                                    std::string_view suffix,
                                    const CancelPredicate &cancelled) {
    return perform(endpoint_, model_,
                   buildOllamaRequest(model_, prefix, suffix), cancelled);
}

OllamaResult OllamaClient::completeWithContext(
    std::string_view prefix, std::string_view suffix,
    std::string_view visibleContext, const CancelPredicate &cancelled) {
    auto result = perform(
        contextEndpoint_, contextModel_,
        buildOllamaContextRequest(contextModel_, prefix, suffix,
                                  visibleContext),
        cancelled);
    if (!result.suggestion.empty() && !configuredFim()) {
        result.suggestion =
            ensureInsertionBoundary(prefix, std::move(result.suggestion));
    }
    return result;
}

OllamaResult OllamaClient::perform(const std::string &endpoint,
                                   const std::string &model,
                                   std::string requestBody,
                                   const CancelPredicate &cancelled) {
    OllamaResult result;
    result.model = model;
    result.requestJson = std::move(requestBody);

    const auto isCancelled = [&cancelled] { return cancelled && cancelled(); };
    if (isCancelled()) {
        result.cancelled = true;
        result.error = "cancelled before request";
        return result;
    }
    if (!multi_) {
        result.error = "curl multi initialization failed";
        return result;
    }

    auto *curl = curl_easy_init();
    if (!curl) {
        result.error = "curl initialization failed";
        return result;
    }

    std::string responseBody;
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, result.requestJson.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(result.requestJson.size()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 150L);
    curl_easy_setopt(
        curl, CURLOPT_TIMEOUT_MS,
        configuredInteger("OMATAB_TIMEOUT_MS", 2500, 250, 10000));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

    const auto started = std::chrono::steady_clock::now();
    curl_multi_add_handle(multi_, curl);

    // Drive the transfer manually so a superseded request can be dropped
    // between steps. interrupt() wakes the poll early; the bounded timeout
    // is only a fallback.
    bool aborted = false;
    CURLMcode multiStatus = CURLM_OK;
    int running = 1;
    while (running) {
        multiStatus = curl_multi_perform(multi_, &running);
        if (multiStatus != CURLM_OK || !running) {
            break;
        }
        if (isCancelled()) {
            aborted = true;
            break;
        }
        multiStatus = curl_multi_poll(multi_, nullptr, 0, kPollTimeoutMs,
                                      nullptr);
        if (multiStatus != CURLM_OK) {
            break;
        }
        if (isCancelled()) {
            aborted = true;
            break;
        }
    }

    CURLcode status = CURLE_OK;
    bool completed = false;
    if (!aborted && multiStatus == CURLM_OK) {
        int remaining = 0;
        while (auto *message = curl_multi_info_read(multi_, &remaining)) {
            if (message->msg == CURLMSG_DONE &&
                message->easy_handle == curl) {
                status = message->data.result;
                completed = true;
            }
        }
    }
    curl_multi_remove_handle(multi_, curl);

    result.latencyMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();
    result.responseJson = responseBody;

    if (aborted) {
        result.cancelled = true;
        result.error = "cancelled";
    } else if (multiStatus != CURLM_OK) {
        result.error = curl_multi_strerror(multiStatus);
    } else if (!completed) {
        result.error = "transfer ended without completion status";
    } else if (status != CURLE_OK) {
        result.error = curl_easy_strerror(status);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.statusCode);
        if (result.statusCode == 200) {
            result.suggestion =
                limitToClause(parseOllamaSuggestion(responseBody));
        } else {
            result.error = "Ollama returned HTTP " +
                           std::to_string(result.statusCode);
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

} // namespace omatab
