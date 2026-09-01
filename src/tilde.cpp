#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

#include "ollama_client.h"
#include "policy.h"
#include "suggestion.h"

namespace {

constexpr char kSuggestion[] = " — Tilde is working";

struct TildeState final : fcitx::InputContextProperty {
    std::string context;
    std::string remainingSuggestion;
    std::uint64_t revision = 0;
};

struct CompletionJob {
    fcitx::ICUUID inputContextId;
    std::uint64_t revision;
    std::uint64_t requestId;
    std::string prefix;
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
                 std::string prefix) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = CompletionJob{std::move(inputContextId), revision,
                                     ++nextRequestId_, std::move(prefix)};
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
                    lock, std::chrono::milliseconds(120), [this, observedRequest] {
                        return stopping_ || !pending_.has_value() ||
                               pending_->requestId != observedRequest;
                    })) {
                continue;
            }

            auto job = std::move(*pending_);
            pending_.reset();
            lock.unlock();
            auto result = client_.complete(job.prefix);
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
        instance->inputContextManager().registerProperty("tildeState",
                                                         &stateFactory_);
        const std::weak_ptr<bool> lifetime = lifetime_;
        completionWorker_ = std::make_unique<CompletionWorker>(
            instance, [this, lifetime](const CompletionJob &job,
                                      tilde::OllamaResult result) {
                if (lifetime.expired() || result.suggestion.empty()) {
                    return;
                }
                auto *inputContext = instance_->inputContextManager().findByUUID(
                    job.inputContextId);
                if (!inputContext) {
                    return;
                }
                auto *state = inputContext->propertyFor(&stateFactory_);
                if (!tilde::suggestionRequestIsCurrent(
                        state->revision, state->context, job.revision,
                        job.prefix)) {
                    return;
                }
                state->remainingSuggestion = std::move(result.suggestion);
                showSuggestion(inputContext, state->remainingSuggestion);
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
            if (state->remainingSuggestion.empty()) {
                clearSuggestion(inputContext);
            } else {
                showSuggestion(inputContext, state->remainingSuggestion);
            }
            event.filterAndAccept();
            return;
        }
        case tilde::Effect::AcceptFullSuggestion:
            ++state->revision;
            inputContext->commitString(state->remainingSuggestion);
            state->context += state->remainingSuggestion;
            state->remainingSuggestion.clear();
            clearSuggestion(inputContext);
            event.filterAndAccept();
            return;
        case tilde::Effect::DismissSuggestion:
            ++state->revision;
            state->remainingSuggestion.clear();
            clearSuggestion(inputContext);
            event.filterAndAccept();
            return;
        case tilde::Effect::ClearSuggestion:
            ++state->revision;
            state->context.clear();
            state->remainingSuggestion.clear();
            clearSuggestion(inputContext);
            return;
        case tilde::Effect::ShowSuggestion: {
            const auto typed = printableText(event.key());
            clearSuggestion(inputContext);
            state->context += typed;
            inputContext->commitString(typed);
            ++state->revision;
            state->remainingSuggestion = kSuggestion;
            showSuggestion(inputContext, state->remainingSuggestion);
            completionWorker_->request(inputContext->uuid(), state->revision,
                                       state->context);
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
        state->context.clear();
        state->remainingSuggestion.clear();
        clearSuggestion(event.inputContext());
    }

private:
    Instance *instance_;
    FactoryFor<TildeState> stateFactory_{
        [](InputContext &) { return new TildeState(); }};
    std::shared_ptr<bool> lifetime_ = std::make_shared<bool>(true);
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
