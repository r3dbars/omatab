#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <json/json.h>

#include "context.h"
#include "ocr_context.h"
#include "ollama_client.h"
#include "telemetry.h"

namespace {

int failures = 0;

void expect(bool condition, const char *name) {
    if (condition) {
        std::cout << "PASS " << name << '\n';
        return;
    }
    std::cerr << "FAIL " << name << '\n';
    ++failures;
}

} // namespace

int main() {
    const auto request =
        tilde::buildOllamaRequest("test-model", "quoted \"prefix\"",
                                  "later text");
    Json::CharReaderBuilder reader;
    Json::Value parsedRequest;
    std::string errors;
    std::istringstream input(request);
    expect(Json::parseFromStream(reader, input, &parsedRequest, &errors),
           "request is valid JSON");
    expect(parsedRequest["model"].asString() == "test-model",
           "request carries model");
    expect(parsedRequest["prompt"].asString() == "quoted \"prefix\"",
           "request safely carries prefix");
    expect(parsedRequest["suffix"].asString() == "later text",
           "request carries fill-in-the-middle suffix");
    expect(!parsedRequest["stream"].asBool(),
           "request disables streaming");
    expect(parsedRequest["raw"].asBool(),
           "request bypasses chat templates");
    expect(parsedRequest["options"]["num_predict"].asInt() == 16,
           "request caps output tokens");
    expect(parsedRequest["options"]["num_ctx"].asInt() == 8192,
           "request uses the tested context window");
    expect(parsedRequest["keep_alive"].asString() == "30m",
           "request uses the default model lease");

    setenv("TILDE_NUM_PREDICT", "12", 1);
    const auto tunedRequest = tilde::buildOllamaRequest("test-model", "text");
    Json::Value parsedTunedRequest;
    std::istringstream tunedInput(tunedRequest);
    expect(Json::parseFromStream(reader, tunedInput, &parsedTunedRequest,
                                 &errors) &&
               parsedTunedRequest["options"]["num_predict"].asInt() == 12,
           "runtime tuning overrides request parameters");
    unsetenv("TILDE_NUM_PREDICT");

    setenv("TILDE_KEEP_ALIVE", "-1", 1);
    const auto residentRequest =
        tilde::buildOllamaRequest("test-model", "text");
    Json::Value parsedResidentRequest;
    std::istringstream residentInput(residentRequest);
    expect(Json::parseFromStream(reader, residentInput,
                                 &parsedResidentRequest, &errors) &&
               parsedResidentRequest["keep_alive"].isInt() &&
               parsedResidentRequest["keep_alive"].asInt() == -1,
           "runtime setting keeps the model resident indefinitely");
    unsetenv("TILDE_KEEP_ALIVE");

    setenv("TILDE_FIM", "1", 1);
    const auto fimRequest = tilde::buildOllamaRequest(
        "test-model", "The quick brown fo", " jumped away.");
    Json::Value parsedFimRequest;
    std::istringstream fimInput(fimRequest);
    expect(Json::parseFromStream(reader, fimInput, &parsedFimRequest,
                                 &errors) &&
               parsedFimRequest["prompt"].asString() ==
                   "<|fim_prefix|>The quick brown fo<|fim_suffix|> jumped "
                   "away.<|fim_middle|>",
           "FIM request preserves the partial word and text after the caret");
    expect(!parsedFimRequest.isMember("suffix"),
           "FIM request bypasses Ollama's unsupported insert field");
    const auto fimContextRequest = tilde::buildOllamaContextRequest(
        "test-model", "partial wo", " after", "visible context");
    Json::Value parsedFimContextRequest;
    std::istringstream fimContextInput(fimContextRequest);
    expect(Json::parseFromStream(reader, fimContextInput,
                                 &parsedFimContextRequest, &errors) &&
               parsedFimContextRequest["prompt"].asString().find(
                   "partial wo<|fim_suffix|> after<|fim_middle|>") !=
                   std::string::npos,
           "OCR-aware FIM request completes at the exact caret");
    unsetenv("TILDE_FIM");

    const auto contextRequest = tilde::buildOllamaContextRequest(
        "context-model", "before caret", "after caret", "visible label");
    Json::Value parsedContextRequest;
    std::istringstream contextInput(contextRequest);
    expect(Json::parseFromStream(reader, contextInput, &parsedContextRequest,
                                 &errors),
           "context request is valid JSON");
    expect(parsedContextRequest["raw"].asBool(),
           "context request bypasses chat templates");
    expect(parsedContextRequest["prompt"].asString().find("visible label") !=
               std::string::npos,
           "context request carries visible OCR text");
    expect(parsedContextRequest["prompt"].asString().find("before caret") !=
               std::string::npos,
           "context request ends with textbox prefix");
    expect(parsedContextRequest["suffix"].asString() == "after caret",
           "context request carries textbox suffix");
    expect(tilde::ensureInsertionBoundary("we should", "continue") ==
               " continue",
           "context completion inserts a word boundary");
    expect(tilde::ensureInsertionBoundary("hello ", "world") == "world",
           "existing whitespace is preserved without duplication");

    expect(tilde::parseOllamaSuggestion(
               R"({"response":" continuation text","done":true})") ==
               " continuation text",
           "response extracts continuation");
    expect(tilde::parseOllamaSuggestion(
               R"({"response":" first line\nsecond line"})") ==
               " first line",
           "response is limited to one line");
    expect(tilde::parseOllamaSuggestion("not json").empty(),
           "invalid response is rejected");
    expect(tilde::sanitizeSuggestion("   ").empty(),
           "blank suggestion is rejected");
    expect(tilde::sanitizeSuggestion(" okay<|endoftext|>") == " okay",
           "model control tokens are removed");
    expect(tilde::sanitizeSuggestion(" useful text<think>internal") ==
               " useful text",
           "thinking spillover is removed");

    const auto structured = tilde::buildContextWindow(
        "Earlier text. Cursor here. Later text.", 26, true, "fallback", 4096,
        1024);
    expect(structured.fromSurroundingText,
           "valid surrounding text wins over fallback");
    expect(structured.prefix == "Earlier text. Cursor here.",
           "context preserves text before cursor");
    expect(structured.suffix == " Later text.",
           "context preserves text after cursor");

    const auto fallback =
        tilde::buildContextWindow("", 0, false, "tracked text", 4096, 1024);
    expect(!fallback.fromSurroundingText && fallback.prefix == "tracked text" &&
               fallback.suffix.empty(),
           "tracked context is used when surrounding text is unavailable");

    const auto unicode = tilde::buildContextWindow("a🙂b", 2, true, "", 4096,
                                                   1024);
    expect(unicode.prefix == "a🙂" && unicode.suffix == "b",
           "cursor offsets are converted from characters to UTF-8 bytes");

    expect(tilde::keepLastUtf8Bytes("ab🙂cd", 5) == "cd",
           "prefix truncation never starts inside UTF-8");

    const auto activeWindow = tilde::parseActiveWindow(
        R"({"address":"0xabc","class":"omawrite","title":"Notes","at":[10,20],"size":[800,600]})");
    expect(activeWindow.valid && activeWindow.x == 10 &&
               activeWindow.height == 600,
           "active window geometry is parsed");
    expect(tilde::ocrAllowedForWindow(activeWindow),
           "ordinary application allows OCR");

    const auto passwordWindow = tilde::parseActiveWindow(
        R"({"address":"0xdef","class":"1Password","title":"Vault","at":[0,0],"size":[800,600]})");
    expect(!tilde::ocrAllowedForWindow(passwordWindow),
           "password manager window blocks OCR");

    const auto telemetryPath =
        "/tmp/tilde-telemetry-test-" + std::to_string(getpid()) + ".jsonl";
    setenv("TILDE_LOG_PATH", telemetryPath.c_str(), 1);
    {
        tilde::TelemetryRecorder telemetry;
        Json::Value event;
        event["type"] = "test";
        event["private_text"] = "local only";
        telemetry.record(std::move(event));
    }
    struct stat telemetryStatus {};
    expect(stat(telemetryPath.c_str(), &telemetryStatus) == 0 &&
               (telemetryStatus.st_mode & 0777) == 0600,
           "telemetry file is owner-only");
    std::ifstream telemetryInput(telemetryPath);
    std::string telemetryLine;
    std::getline(telemetryInput, telemetryLine);
    expect(telemetryLine.find("local only") != std::string::npos,
           "telemetry records full event data");
    unlink(telemetryPath.c_str());
    unsetenv("TILDE_LOG_PATH");

    if (std::getenv("TILDE_LIVE_OCR_TEST")) {
        tilde::OcrContextProvider provider;
        const auto liveText = provider.context();
        expect(!liveText.empty(), "live active-window OCR returns text");
        std::cout << "INFO live OCR bytes=" << liveText.size() << '\n';
    }

    return failures == 0 ? 0 : 1;
}
