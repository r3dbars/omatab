#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <json/json.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>

#include "context.h"
#include "ocr_context.h"
#include "ollama_client.h"
#include "policy.h"
#include "suggestion.h"
#include "telemetry.h"

namespace {

std::chrono::milliseconds predictionDelay() {
    constexpr long kDefaultDelay = 120;
    const auto *configured = std::getenv("TILDE_DEBOUNCE_MS");
    if (!configured || !*configured) {
        return std::chrono::milliseconds(kDefaultDelay);
    }
    char *end = nullptr;
    const auto parsed = std::strtol(configured, &end, 10);
    return end && *end == '\0' && parsed >= 0 && parsed <= 1000
               ? std::chrono::milliseconds(parsed)
               : std::chrono::milliseconds(kDefaultDelay);
}

std::filesystem::path disabledMarkerPath() {
    if (const auto *configured = std::getenv("TILDE_DISABLED_FILE");
        configured && *configured) {
        return configured;
    }
    if (const auto *stateHome = std::getenv("XDG_STATE_HOME");
        stateHome && *stateHome) {
        return std::filesystem::path(stateHome) / "tilde" / "disabled";
    }
    if (const auto *home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "state" / "tilde" /
               "disabled";
    }
    return {};
}

bool predictionsEnabled() {
    const auto marker = disabledMarkerPath();
    if (marker.empty()) {
        return true;
    }
    std::error_code error;
    const bool disabled = std::filesystem::exists(marker, error);
    return error ? true : !disabled;
}

struct TildeState final : fcitx::InputContextProperty {
    std::string context;
    std::string requestPrefix;
    std::string remainingSuggestion;
    std::string originalSuggestion;
    std::string acceptedSuggestion;
    std::uint64_t revision = 0;
    std::uint64_t suggestionId = 0;
};

struct CompletionJob {
    fcitx::ICUUID inputContextId;
    std::uint64_t revision;
    std::uint64_t requestId;
    std::string prefix;
    std::string suffix;
    std::string visibleContext;
    std::string windowClass;
    std::string windowTitle;
};

class CompletionWorker {
public:
    using ResultCallback =
        std::function<void(const CompletionJob &, tilde::OllamaResult)>;

    CompletionWorker(fcitx::Instance *instance, ResultCallback callback)
        : instance_(instance), callback_(std::move(callback)),
          thread_([this] { run(); }) {}

    ~CompletionWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void request(fcitx::ICUUID inputContextId, std::uint64_t revision,
                 std::string prefix, std::string suffix) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = CompletionJob{std::move(inputContextId), revision,
                                     ++nextRequestId_, std::move(prefix),
                                     std::move(suffix), {}, {}, {}};
        }
        condition_.notify_one();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            condition_.wait(lock,
                            [this] { return stopping_ || pending_.has_value(); });
            if (stopping_) {
                break;
            }

            const auto observedRequest = pending_->requestId;
            if (condition_.wait_for(
                    lock, predictionDelay(), [this, observedRequest] {
                        return stopping_ || !pending_.has_value() ||
                               pending_->requestId != observedRequest;
                    })) {
                continue;
            }

            auto job = std::move(*pending_);
            pending_.reset();
            lock.unlock();
            const auto visibleContext = ocrContext_.context();
            const auto &activeWindow = ocrContext_.activeWindow();
            job.visibleContext = visibleContext;
            job.windowClass = activeWindow.windowClass;
            job.windowTitle = activeWindow.title;
            auto result = visibleContext.empty()
                              ? client_.complete(job.prefix, job.suffix)
                              : client_.completeWithContext(
                                    job.prefix, job.suffix, visibleContext);
            instance_->eventDispatcher().schedule(
                [callback = callback_, job = std::move(job),
                 result = std::move(result)]() mutable {
                    callback(job, std::move(result));
                });
            lock.lock();
        }
    }

    fcitx::Instance *instance_;
    ResultCallback callback_;
    tilde::OcrContextProvider ocrContext_;
    tilde::OllamaClient client_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<CompletionJob> pending_;
    std::uint64_t nextRequestId_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};

void clearSuggestion(fcitx::InputContext *inputContext) {
    inputContext->inputPanel().reset();
    inputContext->updatePreedit();
    inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void showSuggestion(fcitx::InputContext *inputContext,
                    const std::string &remainingSuggestion) {
    fcitx::Text preedit;
    preedit.append(
        remainingSuggestion,
        {fcitx::TextFormatFlag::Bold, fcitx::TextFormatFlag::DontCommit});
    preedit.setCursor(0);

    if (inputContext->capabilityFlags().test(fcitx::CapabilityFlag::Preedit)) {
        inputContext->inputPanel().setClientPreedit(preedit);
    } else {
        inputContext->inputPanel().setPreedit(preedit);
    }

    inputContext->updatePreedit();
    inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

bool isPlainPrintable(const fcitx::Key &key) {
    const fcitx::KeyStates shortcutStates{
        fcitx::KeyState::Ctrl, fcitx::KeyState::Alt, fcitx::KeyState::Super};
    if (key.isModifier() ||
        key.states().testAny(shortcutStates)) {
        return false;
    }

    const auto text = fcitx::Key::keySymToUTF8(key.sym());
    return !text.empty() && text.front() >= ' ';
}

std::string printableText(const fcitx::Key &key) {
    return isPlainPrintable(key) ? fcitx::Key::keySymToUTF8(key.sym())
                                 : std::string{};
}

tilde::ContextWindow contextFor(fcitx::InputContext *inputContext,
                                const std::string &trackedFallback) {
    const auto &surrounding = inputContext->surroundingText();
    const bool usableSurrounding =
        inputContext->capabilityFlags().test(
            fcitx::CapabilityFlag::SurroundingText) &&
        surrounding.isValid() && surrounding.cursor() == surrounding.anchor();
    return tilde::buildContextWindow(
        usableSurrounding ? surrounding.text() : std::string_view{},
        usableSurrounding ? surrounding.cursor() : 0, usableSurrounding,
        trackedFallback);
}

tilde::Event classify(const fcitx::Key &key) {
    if (key.check(FcitxKey_Tab)) {
        return tilde::Event::Tab;
    }
    if (key.check(FcitxKey_grave) || key.check(FcitxKey_asciitilde)) {
        return tilde::Event::FullAccept;
    }
    if (key.check(FcitxKey_Escape)) {
        return tilde::Event::Escape;
    }
    if (key.check(FcitxKey_BackSpace) || key.check(FcitxKey_Return) ||
        key.check(FcitxKey_KP_Enter) || key.isCursorMove() ||
        key.states().testAny(fcitx::KeyStates{fcitx::KeyState::Ctrl,
                                             fcitx::KeyState::Alt,
                                             fcitx::KeyState::Super})) {
        return tilde::Event::Editing;
    }
    return isPlainPrintable(key) ? tilde::Event::Printable
                                 : tilde::Event::Other;
}

} // namespace

namespace fcitx {

class TildeEngine final : public InputMethodEngine {
public:
    explicit TildeEngine(Instance *instance) : instance_(instance) {
        Json::Value startupEvent;
        startupEvent["type"] = "service_start";
        startupEvent["telemetry_enabled"] = telemetry_.enabled();
        telemetry_.record(std::move(startupEvent));
        instance->inputContextManager().registerProperty("tildeState",
                                                         &stateFactory_);
        const std::weak_ptr<bool> lifetime = lifetime_;
        completionWorker_ = std::make_unique<CompletionWorker>(
            instance, [this, lifetime](const CompletionJob &job,
                                      tilde::OllamaResult result) {
                if (lifetime.expired()) {
                    return;
                }
                Json::Value modelEvent;
                modelEvent["type"] = "model_result";
                modelEvent["request_id"] =
                    static_cast<Json::UInt64>(job.requestId);
                modelEvent["revision"] =
                    static_cast<Json::UInt64>(job.revision);
                modelEvent["textbox_prefix"] = job.prefix;
                modelEvent["textbox_suffix"] = job.suffix;
                modelEvent["ocr_context"] = job.visibleContext;
                modelEvent["window_class"] = job.windowClass;
                modelEvent["window_title"] = job.windowTitle;
                modelEvent["model"] = result.model;
                modelEvent["request_json"] = result.requestJson;
                modelEvent["response_json"] = result.responseJson;
                modelEvent["suggestion"] = result.suggestion;
                modelEvent["error"] = result.error;
                modelEvent["http_status"] =
                    static_cast<Json::Int64>(result.statusCode);
                modelEvent["latency_ms"] = result.latencyMs;
                auto *inputContext = instance_->inputContextManager().findByUUID(
                    job.inputContextId);
                if (!inputContext) {
                    modelEvent["outcome"] = "input_context_gone";
                    telemetry_.record(std::move(modelEvent));
                    return;
                }
                auto *state = inputContext->propertyFor(&stateFactory_);
                if (!tilde::suggestionRequestIsCurrent(
                        state->revision, state->requestPrefix, job.revision,
                        job.prefix)) {
                    modelEvent["outcome"] = "stale";
                    telemetry_.record(std::move(modelEvent));
                    return;
                }
                if (result.suggestion.empty()) {
                    modelEvent["outcome"] = result.error.empty()
                                                  ? "empty"
                                                  : "request_error";
                    telemetry_.record(std::move(modelEvent));
                    return;
                }
                state->remainingSuggestion = std::move(result.suggestion);
                state->originalSuggestion = state->remainingSuggestion;
                state->acceptedSuggestion.clear();
                state->suggestionId = job.requestId;
                showSuggestion(inputContext, state->remainingSuggestion);
                modelEvent["outcome"] = "shown";
                telemetry_.record(std::move(modelEvent));
            });
    }

    ~TildeEngine() override {
        lifetime_.reset();
        completionWorker_.reset();
    }

    std::vector<InputMethodEntry> listInputMethods() override {
        std::vector<InputMethodEntry> result;
        result.emplace_back("tilde", "Tilde Linux Proof", "en", "tilde");
        return result;
    }

    void keyEvent(const InputMethodEntry &, KeyEvent &event) override {
        if (event.isRelease()) {
            return;
        }

        auto *inputContext = event.inputContext();
        auto *state = inputContext->propertyFor(&stateFactory_);

        if (!predictionsEnabled()) {
            if (!state->remainingSuggestion.empty()) {
                ++state->revision;
                recordAction("disabled", *state);
                state->remainingSuggestion.clear();
                clearSuggestion(inputContext);
                clearSuggestionState(*state);
            }
            return;
        }

        if (inputContext->capabilityFlags().test(
                fcitx::CapabilityFlag::PasswordOrSensitive)) {
            return;
        }

        switch (tilde::decide(!state->remainingSuggestion.empty(),
                              classify(event.key()))) {
        case tilde::Effect::AcceptNextWord: {
            ++state->revision;
            const auto length =
                tilde::nextWordLength(state->remainingSuggestion);
            auto accepted = state->remainingSuggestion.substr(0, length);
            state->remainingSuggestion.erase(0, length);
            if (state->remainingSuggestion.empty()) {
                if (!accepted.empty() && accepted.back() != ' ') {
                    accepted.push_back(' ');
                }
            }
            inputContext->commitString(accepted);
            state->context += accepted;
            state->acceptedSuggestion += accepted;
            recordAction("accept_word", *state, accepted);
            if (state->remainingSuggestion.empty()) {
                clearSuggestion(inputContext);
                clearSuggestionState(*state);
            } else {
                showSuggestion(inputContext, state->remainingSuggestion);
            }
            event.filterAndAccept();
            return;
        }
        case tilde::Effect::AcceptFullSuggestion:
            ++state->revision;
            state->acceptedSuggestion += state->remainingSuggestion;
            recordAction("accept_full", *state,
                         state->remainingSuggestion);
            inputContext->commitString(state->remainingSuggestion);
            state->context += state->remainingSuggestion;
            state->remainingSuggestion.clear();
            clearSuggestion(inputContext);
            clearSuggestionState(*state);
            event.filterAndAccept();
            return;
        case tilde::Effect::DismissSuggestion:
            ++state->revision;
            recordAction("dismiss", *state);
            state->remainingSuggestion.clear();
            clearSuggestion(inputContext);
            clearSuggestionState(*state);
            event.filterAndAccept();
            return;
        case tilde::Effect::ClearSuggestion:
            ++state->revision;
            if (!state->remainingSuggestion.empty()) {
                recordAction("clear_editing", *state);
            }
            state->context.clear();
            state->remainingSuggestion.clear();
            clearSuggestion(inputContext);
            clearSuggestionState(*state);
            return;
        case tilde::Effect::ShowSuggestion: {
            const auto typed = printableText(event.key());
            if (!state->remainingSuggestion.empty()) {
                recordAction("typed_over", *state);
            }
            auto context = contextFor(inputContext, state->context);
            context.prefix = tilde::keepLastUtf8Bytes(context.prefix + typed,
                                                      4096);
            clearSuggestion(inputContext);
            state->context += typed;
            inputContext->commitString(typed);
            ++state->revision;
            state->requestPrefix = context.prefix;
            state->remainingSuggestion.clear();
            clearSuggestionState(*state);
            Json::Value typingEvent;
            typingEvent["type"] = "printable_input";
            typingEvent["typed"] = typed;
            typingEvent["textbox_prefix"] = context.prefix;
            typingEvent["textbox_suffix"] = context.suffix;
            typingEvent["revision"] =
                static_cast<Json::UInt64>(state->revision);
            telemetry_.record(std::move(typingEvent));
            completionWorker_->request(inputContext->uuid(), state->revision,
                                       std::move(context.prefix),
                                       std::move(context.suffix));
            event.filterAndAccept();
            return;
        }
        case tilde::Effect::PassThrough:
            return;
        }
    }

    void reset(const InputMethodEntry &, InputContextEvent &event) override {
        auto *state = event.inputContext()->propertyFor(&stateFactory_);
        ++state->revision;
        if (!state->remainingSuggestion.empty()) {
            recordAction("reset", *state);
        }
        state->context.clear();
        state->remainingSuggestion.clear();
        clearSuggestion(event.inputContext());
        clearSuggestionState(*state);
    }

private:
    void recordAction(const char *type, const TildeState &state,
                      const std::string &accepted = {}) {
        Json::Value action;
        action["type"] = type;
        action["request_id"] =
            static_cast<Json::UInt64>(state.suggestionId);
        action["original_suggestion"] = state.originalSuggestion;
        action["accepted_piece"] = accepted;
        action["accepted_total"] = state.acceptedSuggestion;
        action["remaining_suggestion"] = state.remainingSuggestion;
        action["revision"] = static_cast<Json::UInt64>(state.revision);
        telemetry_.record(std::move(action));
    }

    static void clearSuggestionState(TildeState &state) {
        state.originalSuggestion.clear();
        state.acceptedSuggestion.clear();
        state.suggestionId = 0;
    }

    Instance *instance_;
    FactoryFor<TildeState> stateFactory_{
        [](InputContext &) { return new TildeState(); }};
    std::shared_ptr<bool> lifetime_ = std::make_shared<bool>(true);
    tilde::TelemetryRecorder telemetry_;
    std::unique_ptr<CompletionWorker> completionWorker_;
};

class TildeFactory final : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        return new TildeEngine(manager->instance());
    }
};

} // namespace fcitx

FCITX_ADDON_FACTORY_V2(tilde, fcitx::TildeFactory);
