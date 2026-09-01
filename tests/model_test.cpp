#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include <json/json.h>

#include "context.h"
#include "ocr_context.h"
#include "ollama_client.h"

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
    expect(parsedRequest["options"]["num_predict"].asInt() == 16,
           "request caps output tokens");

    const auto contextRequest = tilde::buildOllamaContextRequest(
        "context-model", "before caret", "after caret", "visible label");
    Json::Value parsedContextRequest;
    std::istringstream contextInput(contextRequest);
    expect(Json::parseFromStream(reader, contextInput, &parsedContextRequest,
                                 &errors),
           "context request is valid JSON");
    expect(parsedContextRequest["messages"].size() == 2,
           "context request carries system and user messages");
    expect(parsedContextRequest["messages"][1]["content"]
                   .asString()
                   .find("visible label") != std::string::npos,
           "context request carries visible OCR text");
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

    if (std::getenv("TILDE_LIVE_OCR_TEST")) {
        tilde::OcrContextProvider provider;
        const auto liveText = provider.context();
        expect(!liveText.empty(), "live active-window OCR returns text");
        std::cout << "INFO live OCR bytes=" << liveText.size() << '\n';
    }

    return failures == 0 ? 0 : 1;
}
