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
#include <fcitx-config/configuration.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/option.h>
#include <fcitx-config/rawconfig.h>
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

namespace omatab {

// Accept keys may be plain keys such as grave; Fcitx rejects modifier-less
// keys unless told otherwise.
const fcitx::KeyConstrainFlags kAcceptKeyFlags{
    fcitx::KeyConstrainFlag::AllowModifierLess};

// User settings. Stored at ~/.config/fcitx5/conf/omatab.conf and editable
// with `omatab` or fcitx5-configtool; changes apply live.
FCITX_CONFIGURATION(
    OmatabConfig,
    fcitx::KeyListOption fullAcceptKey{
        this, "FullAcceptKey", "Accept the whole suggestion",
        {fcitx::Key("Shift+Tab")}, fcitx::KeyListConstrain(kAcceptKeyFlags)};
    fcitx::Option<bool> screenContext{
        this, "ScreenContext",
        "Read text on the active window (OCR) for extra context", false};
    fcitx::Option<bool> telemetry{
        this, "Telemetry",
        "Record a private local log of suggestions for tuning", false};);

constexpr const char *kConfigFile = "conf/omatab.conf";

} // namespace omatab

namespace {

std::chrono::milliseconds predictionDelay() {
    constexpr long kDefaultDelay = 120;
    const auto *configured = std::getenv("OMATAB_DEBOUNCE_MS");
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
    if (const auto *configured = std::getenv("OMATAB_DISABLED_FILE");
        configured && *configured) {
        return configured;
    }
    if (const auto *stateHome = std::getenv("XDG_STATE_HOME");
        stateHome && *stateHome) {
        return std::filesystem::path(stateHome) / "omatab" / "disabled";
    }
    if (const auto *home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "state" / "omatab" /
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

struct OmatabState final : fcitx::InputContextProperty {
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
    double ocrAgeMs = -1.0;
    bool ocrRefreshing = false;
};

class CompletionWorker {
public:
    using ResultCallback =
        std::function<void(const CompletionJob &, omatab::OllamaResult)>;

    CompletionWorker(fcitx::Instance *instance, ResultCallback callback)
        : instance_(instance), callback_(std::move(callback)),
          thread_([this] { run(); }) {}

    ~CompletionWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        shutdown_.store(true);
        condition_.notify_one();
        client_.interrupt();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void setScreenContext(bool enabled) { screenContext_.store(enabled); }

    void request(fcitx::ICUUID inputContextId, std::uint64_t revision,
                 std::string prefix, std::string suffix) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = CompletionJob{std::move(inputContextId), revision,
                                     ++nextRequestId_, std::move(prefix),
                                     std::move(suffix), {}, {}, {}};
        }
        condition_.notify_one();
        // Any transfer still running belongs to an older keystroke; drop it
        // so the GPU moves on to this one instead of finishing stale work.
        client_.interrupt();
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
            const auto requestId = job.requestId;
            const auto superseded = [this, requestId] {
                return shutdown_.load() ||
                       nextRequestId_.load() != requestId;
            };
            // Answers from cache immediately; a slow capture refreshes in
            // the background for later requests. With screen context off,
            // only the window identity is looked up for telemetry.
            omatab::OcrSnapshot ocr;
            if (screenContext_.load()) {
                ocr = ocrContext_.snapshot();
            } else {
                ocrContext_.refreshActiveWindow();
            }
            const auto &activeWindow = ocrContext_.activeWindow();
            job.visibleContext = std::move(ocr.text);
            job.ocrAgeMs = ocr.ageMs;
            job.ocrRefreshing = ocr.refreshing;
            job.windowClass = activeWindow.windowClass;
            job.windowTitle = activeWindow.title;
            auto result =
                job.visibleContext.empty()
                    ? client_.complete(job.prefix, job.suffix, superseded)
                    : client_.completeWithContext(job.prefix, job.suffix,
                                                  job.visibleContext,
                                                  superseded);
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
    omatab::OcrContextProvider ocrContext_;
    omatab::OllamaClient client_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<CompletionJob> pending_;
    // Written under mutex_, read lock-free by the cancel predicate.
    std::atomic<std::uint64_t> nextRequestId_{0};
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> screenContext_{false};
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

omatab::ContextWindow contextFor(fcitx::InputContext *inputContext,
                                const std::string &trackedFallback) {
    const auto &surrounding = inputContext->surroundingText();
    const bool usableSurrounding =
        inputContext->capabilityFlags().test(
            fcitx::CapabilityFlag::SurroundingText) &&
        surrounding.isValid() && surrounding.cursor() == surrounding.anchor();
    return omatab::buildContextWindow(
        usableSurrounding ? surrounding.text() : std::string_view{},
        usableSurrounding ? surrounding.cursor() : 0, usableSurrounding,
        trackedFallback);
}

omatab::Event classify(const fcitx::Key &key,
                       const fcitx::KeyList &fullAcceptKeys) {
    // Shift+Tab may arrive as ISO_Left_Tab; normalize() folds that back.
    const auto normalized = key.normalize();
    if (normalized.checkKeyList(fullAcceptKeys)) {
        return omatab::Event::FullAccept;
    }
    if (key.check(FcitxKey_Tab)) {
        return omatab::Event::Tab;
    }
    if (key.check(FcitxKey_Escape)) {
        return omatab::Event::Escape;
    }
    if (key.check(FcitxKey_BackSpace) || key.check(FcitxKey_Return) ||
        key.check(FcitxKey_KP_Enter) || key.isCursorMove() ||
        key.states().testAny(fcitx::KeyStates{fcitx::KeyState::Ctrl,
                                             fcitx::KeyState::Alt,
                                             fcitx::KeyState::Super})) {
        return omatab::Event::Editing;
    }
    return isPlainPrintable(key) ? omatab::Event::Printable
                                 : omatab::Event::Other;
}

} // namespace

namespace fcitx {

class OmatabEngine final : public InputMethodEngine {
public:
    explicit OmatabEngine(Instance *instance) : instance_(instance) {
        readAsIni(config_, omatab::kConfigFile);
        ensureFullAcceptKey();
        telemetry_.setEnabled(*config_.telemetry);
        Json::Value startupEvent;
        startupEvent["type"] = "service_start";
        startupEvent["telemetry_enabled"] = telemetry_.enabled();
        startupEvent["screen_context"] = *config_.screenContext;
        startupEvent["full_accept_key"] =
            Key::keyListToString(*config_.fullAcceptKey);
        telemetry_.record(std::move(startupEvent));
        instance->inputContextManager().registerProperty("omatabState",
                                                         &stateFactory_);
        const std::weak_ptr<bool> lifetime = lifetime_;
        completionWorker_ = std::make_unique<CompletionWorker>(
            instance, [this, lifetime](const CompletionJob &job,
                                      omatab::OllamaResult result) {
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
                modelEvent["ocr_age_ms"] = job.ocrAgeMs;
                modelEvent["ocr_refreshing"] = job.ocrRefreshing;
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
                if (result.cancelled) {
                    modelEvent["outcome"] = "cancelled";
                    telemetry_.record(std::move(modelEvent));
                    return;
                }
                auto *inputContext = instance_->inputContextManager().findByUUID(
                    job.inputContextId);
                if (!inputContext) {
                    modelEvent["outcome"] = "input_context_gone";
                    telemetry_.record(std::move(modelEvent));
                    return;
                }
                auto *state = inputContext->propertyFor(&stateFactory_);
                if (!omatab::suggestionRequestIsCurrent(
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
                if (auto reason = omatab::suggestionRejection(
                        result.suggestion, job.visibleContext);
                    !reason.empty()) {
                    modelEvent["outcome"] = "filtered";
                    modelEvent["filter_reason"] = reason;
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
        completionWorker_->setScreenContext(*config_.screenContext);
    }

    const Configuration *getConfig() const override { return &config_; }

    void setConfig(const RawConfig &raw) override {
        config_.load(raw, true);
        safeSaveAsIni(config_, omatab::kConfigFile);
        applyConfig();
    }

    void reloadConfig() override {
        readAsIni(config_, omatab::kConfigFile);
        ensureFullAcceptKey();
        applyConfig();
    }

    ~OmatabEngine() override {
        lifetime_.reset();
        completionWorker_.reset();
    }

    std::vector<InputMethodEntry> listInputMethods() override {
        std::vector<InputMethodEntry> result;
        result.emplace_back("omatab", "Oma Tab", "en", "omatab");
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

        switch (omatab::decide(!state->remainingSuggestion.empty(),
                              classify(event.key(),
                                       *config_.fullAcceptKey))) {
        case omatab::Effect::AcceptNextWord: {
            ++state->revision;
            const auto length =
                omatab::nextWordLength(state->remainingSuggestion);
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
        case omatab::Effect::AcceptFullSuggestion:
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
        case omatab::Effect::DismissSuggestion:
            ++state->revision;
            recordAction("dismiss", *state);
            state->remainingSuggestion.clear();
            clearSuggestion(inputContext);
            clearSuggestionState(*state);
            event.filterAndAccept();
            return;
        case omatab::Effect::ClearSuggestion:
            ++state->revision;
            if (!state->remainingSuggestion.empty()) {
                recordAction("clear_editing", *state);
            }
            state->context.clear();
            state->remainingSuggestion.clear();
            clearSuggestion(inputContext);
            clearSuggestionState(*state);
            return;
        case omatab::Effect::ShowSuggestion: {
            const auto typed = printableText(event.key());
            if (!state->remainingSuggestion.empty()) {
                // A typed-over suggestion that began with the same character
                // was right; the user simply kept typing. Quality reports
                // separate that from a wrong suggestion.
                Json::Value detail;
                detail["typed"] = typed;
                detail["matched"] =
                    state->remainingSuggestion.compare(0, typed.size(),
                                                       typed) == 0;
                recordAction("typed_over", *state, {}, detail);
            }
            auto context = contextFor(inputContext, state->context);
            context.prefix = omatab::keepLastUtf8Bytes(context.prefix + typed,
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
        case omatab::Effect::PassThrough:
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
    // A hand-edited file can leave the key list empty, which would make the
    // whole suggestion unreachable. Fall back to the default binding.
    void ensureFullAcceptKey() {
        if (config_.fullAcceptKey->empty()) {
            config_.fullAcceptKey.setValue(KeyList{Key("Shift+Tab")});
        }
    }

    void applyConfig() {
        telemetry_.setEnabled(*config_.telemetry);
        if (completionWorker_) {
            completionWorker_->setScreenContext(*config_.screenContext);
        }
        Json::Value event;
        event["type"] = "config_applied";
        event["screen_context"] = *config_.screenContext;
        event["full_accept_key"] =
            Key::keyListToString(*config_.fullAcceptKey);
        telemetry_.record(std::move(event));
    }

    void recordAction(const char *type, const OmatabState &state,
                      const std::string &accepted = {},
                      const Json::Value &detail = Json::Value()) {
        Json::Value action;
        if (detail.isObject()) {
            for (const auto &key : detail.getMemberNames()) {
                action[key] = detail[key];
            }
        }
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

    static void clearSuggestionState(OmatabState &state) {
        state.originalSuggestion.clear();
        state.acceptedSuggestion.clear();
        state.suggestionId = 0;
    }

    Instance *instance_;
    omatab::OmatabConfig config_;
    FactoryFor<OmatabState> stateFactory_{
        [](InputContext &) { return new OmatabState(); }};
    std::shared_ptr<bool> lifetime_ = std::make_shared<bool>(true);
    omatab::TelemetryRecorder telemetry_;
    std::unique_ptr<CompletionWorker> completionWorker_;
};

class OmatabFactory final : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        return new OmatabEngine(manager->instance());
    }
};

} // namespace fcitx

FCITX_ADDON_FACTORY_V2(omatab, fcitx::OmatabFactory);
