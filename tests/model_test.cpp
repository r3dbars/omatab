#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
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

// Listens on an ephemeral loopback port and never answers, so a client
// request stays in flight until it is cancelled or times out.
struct SilentServer {
    int descriptor = -1;
    unsigned short port = 0;

    SilentServer() {
        descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (descriptor < 0) {
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(descriptor, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) != 0 ||
            ::listen(descriptor, 4) != 0) {
            ::close(descriptor);
            descriptor = -1;
            return;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address),
                          &length) == 0) {
            port = ntohs(address.sin_port);
        }
    }

    ~SilentServer() {
        if (descriptor >= 0) {
            ::close(descriptor);
        }
    }

    std::string endpoint() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/api/generate";
    }
};

} // namespace

int main() {
    setenv("OMATAB_FIM", "0", 1);
    const auto request =
        omatab::buildOllamaRequest("test-model", "quoted \"prefix\"",
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

    setenv("OMATAB_NUM_PREDICT", "12", 1);
    const auto tunedRequest = omatab::buildOllamaRequest("test-model", "text");
    Json::Value parsedTunedRequest;
    std::istringstream tunedInput(tunedRequest);
    expect(Json::parseFromStream(reader, tunedInput, &parsedTunedRequest,
                                 &errors) &&
               parsedTunedRequest["options"]["num_predict"].asInt() == 12,
           "runtime tuning overrides request parameters");
    unsetenv("OMATAB_NUM_PREDICT");

    setenv("OMATAB_KEEP_ALIVE", "-1", 1);
    const auto residentRequest =
        omatab::buildOllamaRequest("test-model", "text");
    Json::Value parsedResidentRequest;
    std::istringstream residentInput(residentRequest);
    expect(Json::parseFromStream(reader, residentInput,
                                 &parsedResidentRequest, &errors) &&
               parsedResidentRequest["keep_alive"].isInt() &&
               parsedResidentRequest["keep_alive"].asInt() == -1,
           "runtime setting keeps the model resident indefinitely");
    unsetenv("OMATAB_KEEP_ALIVE");

    unsetenv("OMATAB_FIM");
    const auto fimRequest = omatab::buildOllamaRequest(
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
    const auto fimContextRequest = omatab::buildOllamaContextRequest(
        "test-model", "partial wo", " after", "visible context");
    Json::Value parsedFimContextRequest;
    std::istringstream fimContextInput(fimContextRequest);
    expect(Json::parseFromStream(reader, fimContextInput,
                                 &parsedFimContextRequest, &errors) &&
               parsedFimContextRequest["prompt"].asString().find(
                   "partial wo<|fim_suffix|> after<|fim_middle|>") !=
                   std::string::npos,
           "OCR-aware FIM request completes at the exact caret");
    expect(std::string(omatab::kDefaultModel).find("Qwen3.5-4B-Base") !=
               std::string::npos,
           "default model is the balanced Qwen 4B profile");
    setenv("OMATAB_FIM", "0", 1);

    const auto contextRequest = omatab::buildOllamaContextRequest(
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
    expect(omatab::ensureInsertionBoundary("we should", "continue") ==
               " continue",
           "context completion inserts a word boundary");
    expect(omatab::ensureInsertionBoundary("hello ", "world") == "world",
           "existing whitespace is preserved without duplication");

    expect(omatab::parseOllamaSuggestion(
               R"({"response":" continuation text","done":true})") ==
               " continuation text",
           "response extracts continuation");
    expect(omatab::parseOllamaSuggestion(
               R"({"response":" first line\nsecond line"})") ==
               " first line",
           "response is limited to one line");
    expect(omatab::parseOllamaSuggestion("not json").empty(),
           "invalid response is rejected");
    expect(omatab::sanitizeSuggestion("   ").empty(),
           "blank suggestion is rejected");
    expect(omatab::sanitizeSuggestion(" okay<|endoftext|>") == " okay",
           "model control tokens are removed");
    expect(omatab::sanitizeSuggestion(" useful text<think>internal") ==
               " useful text",
           "thinking spillover is removed");

    expect(omatab::limitToClause(" is working now. The next step is") ==
               " is working now.",
           "suggestion stops after the sentence ends");
    expect(omatab::limitToClause(" is working now, and I think we") ==
               " is working now,",
           "suggestion stops after the first clause");
    expect(omatab::limitToClause(" costs 3.5 million, roughly") ==
               " costs 3.5 million,",
           "decimal point is not a sentence end");
    expect(omatab::limitToClause(" e.g. the second one") ==
               " e.g. the second one",
           "abbreviation period is not a sentence end");
    expect(omatab::limitToClause(" in the U.S. and Canada") ==
               " in the U.S. and Canada",
           "dotted abbreviation is not a sentence end");
    expect(omatab::limitToClause(" said Dr. Jones. Then") ==
               " said Dr. Jones.",
           "sentence end after an abbreviation is still found");
    expect(omatab::limitToClause(" over 1,000 people came") ==
               " over 1,000 people came",
           "thousands separator is not a clause end");
    expect(omatab::limitToClause(", and then we left.") == ", and then we left.",
           "leading punctuation does not produce an empty clause");
    expect(omatab::limitToClause(" really?! I had no idea.") == " really?!",
           "repeated end marks stay together");
    expect(omatab::limitToClause(" \"done.\" Then she left") ==
               " \"done.\"",
           "closing quote stays with the sentence end");
    expect(omatab::limitToClause(" the plan \xE2\x80\x94 if it works") ==
               " the plan",
           "em dash starts a new clause");
    expect(omatab::limitToClause(".") == ".",
           "bare sentence end is a valid suggestion");

    const std::string screen =
        "Alex: Hey, do you want to meet at seven?\n"
        "auto mode on (shift+tab to cycle)\n"
        "90t-5.6-501 medium fast - ~/Work\n";
    expect(omatab::suggestionRejection(" I would like to meet at seven",
                                       screen)
               .empty(),
           "reusing a phrase from the screen is allowed");
    expect(omatab::suggestionRejection(" Sure, seven works for me", screen)
               .empty(),
           "ordinary reply passes the filter");
    expect(omatab::suggestionRejection(" is working now.", "") .empty(),
           "suggestion passes without screen context");
    expect(omatab::suggestionRejection(" e", screen) == "too_short",
           "single character suggestion is rejected");
    expect(omatab::suggestionRejection("?", screen) == "too_short",
           "lone punctuation is rejected");
    expect(omatab::suggestionRejection(" ...!", screen) == "no_content",
           "punctuation-only suggestion is rejected");
    expect(omatab::suggestionRejection(" 2222222222222222", screen) ==
               "repeated_run",
           "repeated character run is rejected");
    expect(omatab::suggestionRejection(" auto mode on (shift+tab to cycle)",
                                       screen) == "screen_line_echo",
           "copy of a whole screen line is rejected");
    expect(omatab::suggestionRejection("  90T-5.6-501   medium fast - ~/Work ",
                                       screen) == "screen_line_echo",
           "screen line echo ignores case and spacing");
    expect(omatab::suggestionRejection(" ~/Work", screen).empty(),
           "short fragment matching a screen line is allowed");
    expect(omatab::suggestionRejection(" caf\xC3\xA9", screen).empty(),
           "non-ASCII text counts as content");

    const auto structured = omatab::buildContextWindow(
        "Earlier text. Cursor here. Later text.", 26, true, "fallback", 4096,
        1024);
    expect(structured.fromSurroundingText,
           "valid surrounding text wins over fallback");
    expect(structured.prefix == "Earlier text. Cursor here.",
           "context preserves text before cursor");
    expect(structured.suffix == " Later text.",
           "context preserves text after cursor");

    const auto fallback =
        omatab::buildContextWindow("", 0, false, "tracked text", 4096, 1024);
    expect(!fallback.fromSurroundingText && fallback.prefix == "tracked text" &&
               fallback.suffix.empty(),
           "tracked context is used when surrounding text is unavailable");

    const auto unicode = omatab::buildContextWindow("a🙂b", 2, true, "", 4096,
                                                   1024);
    expect(unicode.prefix == "a🙂" && unicode.suffix == "b",
           "cursor offsets are converted from characters to UTF-8 bytes");

    expect(omatab::keepLastUtf8Bytes("ab🙂cd", 5) == "cd",
           "prefix truncation never starts inside UTF-8");

    const auto activeWindow = omatab::parseActiveWindow(
        R"({"address":"0xabc","class":"omawrite","title":"Notes","at":[10,20],"size":[800,600]})");
    expect(activeWindow.valid && activeWindow.x == 10 &&
               activeWindow.height == 600,
           "active window geometry is parsed");
    expect(omatab::ocrAllowedForWindow(activeWindow),
           "ordinary application allows OCR");

    const auto passwordWindow = omatab::parseActiveWindow(
        R"({"address":"0xdef","class":"1Password","title":"Vault","at":[0,0],"size":[800,600]})");
    expect(!omatab::ocrAllowedForWindow(passwordWindow),
           "password manager window blocks OCR");

    const auto telemetryPath =
        "/tmp/omatab-telemetry-test-" + std::to_string(getpid()) + ".jsonl";
    setenv("OMATAB_LOG_PATH", telemetryPath.c_str(), 1);
    {
        omatab::TelemetryRecorder telemetry;
        Json::Value silent;
        silent["type"] = "test";
        telemetry.record(silent);
        struct stat before {};
        expect(!telemetry.enabled() &&
                   stat(telemetryPath.c_str(), &before) != 0,
               "telemetry writes nothing until enabled");
        telemetry.setEnabled(true);
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
    unsetenv("OMATAB_LOG_PATH");

    {
        omatab::OllamaClient client("http://127.0.0.1:9/api/generate",
                                   "test-model");
        const auto skipped = client.complete("text", {}, [] { return true; });
        expect(skipped.cancelled && skipped.suggestion.empty(),
               "already-superseded request is skipped before sending");
        expect(skipped.statusCode == 0 && skipped.responseJson.empty(),
               "skipped request never reaches the network");
    }

    {
        SilentServer server;
        expect(server.port != 0, "silent test server is listening");
        setenv("OMATAB_TIMEOUT_MS", "5000", 1);
        omatab::OllamaClient client(server.endpoint(), "test-model");
        std::atomic<bool> superseded{false};
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            superseded.store(true);
            client.interrupt();
        });
        const auto started = std::chrono::steady_clock::now();
        const auto result = client.complete(
            "text", {}, [&superseded] { return superseded.load(); });
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();
        canceller.join();
        unsetenv("OMATAB_TIMEOUT_MS");
        expect(result.cancelled, "in-flight request reports cancellation");
        expect(result.suggestion.empty() && result.error == "cancelled",
               "cancelled request yields no suggestion");
        expect(elapsed >= 50 && elapsed < 1000,
               "cancellation returns promptly instead of waiting for timeout");
        std::cout << "INFO cancellation latency ms=" << elapsed << '\n';

        // The client must remain usable for the next request.
        std::atomic<bool> secondSuperseded{false};
        std::thread secondCanceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            secondSuperseded.store(true);
            client.interrupt();
        });
        const auto second = client.complete(
            "more", {}, [&secondSuperseded] { return secondSuperseded.load(); });
        secondCanceller.join();
        expect(second.cancelled, "client accepts a new request after cancel");
    }

    {
        std::atomic<int> captures{0};
        omatab::ActiveWindow fakeWindow;
        fakeWindow.address = "0x1";
        fakeWindow.windowClass = "editor";
        fakeWindow.width = 800;
        fakeWindow.height = 600;
        fakeWindow.valid = true;
        omatab::OcrContextProvider provider(
            [&fakeWindow] { return fakeWindow; },
            [&captures](const omatab::ActiveWindow &window) {
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                ++captures;
                return "seen in " + window.windowClass;
            },
            std::chrono::milliseconds(150), std::chrono::milliseconds(600));

        const auto started = std::chrono::steady_clock::now();
        const auto first = provider.snapshot();
        const auto firstMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();
        expect(first.text.empty() && first.ageMs < 0 && first.refreshing,
               "first OCR snapshot returns empty without waiting");
        expect(firstMs < 40, "OCR snapshot does not block on capture");

        expect(provider.waitForRefresh(std::chrono::seconds(2)),
               "background OCR capture finishes");
        const auto second = provider.snapshot();
        expect(second.text == "seen in editor" && second.ageMs >= 0 &&
                   !second.refreshing,
               "later snapshot serves the captured text from cache");
        expect(captures.load() == 1, "fresh cache does not recapture");

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto stale = provider.snapshot();
        expect(stale.text == "seen in editor" && stale.refreshing,
               "expired cache still serves old text while refreshing");
        provider.snapshot();
        expect(provider.waitForRefresh(std::chrono::seconds(2)) &&
                   captures.load() == 2,
               "overlapping refresh requests coalesce into one capture");

        // Wait past the maximum age: text is withheld, refresh scheduled.
        provider.waitForRefresh(std::chrono::seconds(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        const auto tooOld = provider.snapshot();
        expect(tooOld.text.empty() && tooOld.ageMs < 0 && tooOld.refreshing,
               "capture past the maximum age is withheld while refreshing");
        provider.waitForRefresh(std::chrono::seconds(2));
        expect(provider.snapshot().text == "seen in editor",
               "fresh capture after the age limit is served again");

        fakeWindow.address = "0x2";
        fakeWindow.windowClass = "1Password";
        const auto blocked = provider.snapshot();
        expect(blocked.text.empty() && !blocked.refreshing,
               "blocked window gets neither cached text nor a capture");

        fakeWindow.windowClass = "browser";
        const auto switched = provider.snapshot();
        expect(switched.text.empty() && switched.refreshing,
               "new window never receives another window's cached text");
        provider.waitForRefresh(std::chrono::seconds(2));
        expect(provider.snapshot().text == "seen in browser",
               "new window is captured in the background");
    }

    if (std::getenv("OMATAB_LIVE_OCR_TEST")) {
        omatab::OcrContextProvider provider;
        provider.snapshot();
        provider.waitForRefresh(std::chrono::seconds(10));
        const auto liveText = provider.context();
        expect(!liveText.empty(), "live active-window OCR returns text");
        std::cout << "INFO live OCR bytes=" << liveText.size() << '\n';
    }

    return failures == 0 ? 0 : 1;
}
