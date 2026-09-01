#include <string>
#include <vector>

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

#include "policy.h"
#include "suggestion.h"

namespace {

constexpr char kSuggestion[] = " — Tilde is working";

struct TildeState final : fcitx::InputContextProperty {
    std::string buffer;
    std::string remainingSuggestion;
};

void clearSuggestion(fcitx::InputContext *inputContext) {
    inputContext->inputPanel().reset();
    inputContext->updatePreedit();
    inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void showSuggestion(fcitx::InputContext *inputContext,
                    const std::string &buffer,
                    const std::string &remainingSuggestion) {
    fcitx::Text preedit;
    preedit.append(buffer);
    preedit.append(
        remainingSuggestion,
        {fcitx::TextFormatFlag::Bold, fcitx::TextFormatFlag::DontCommit});
    preedit.setCursor(buffer.size());

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
    explicit TildeEngine(Instance *instance) {
        instance->inputContextManager().registerProperty("tildeState",
                                                         &stateFactory_);
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

        switch (tilde::decide(!state->remainingSuggestion.empty(),
                              classify(event.key()))) {
        case tilde::Effect::AcceptNextWord: {
            const auto length =
                tilde::nextWordLength(state->remainingSuggestion);
            state->buffer.append(state->remainingSuggestion, 0, length);
            state->remainingSuggestion.erase(0, length);
            if (state->remainingSuggestion.empty()) {
                if (!state->buffer.empty() && state->buffer.back() != ' ') {
                    state->buffer.push_back(' ');
                }
                inputContext->commitString(state->buffer);
                state->buffer.clear();
                clearSuggestion(inputContext);
            } else {
                showSuggestion(inputContext, state->buffer,
                               state->remainingSuggestion);
            }
            event.filterAndAccept();
            return;
        }
        case tilde::Effect::AcceptFullSuggestion:
            inputContext->commitString(state->buffer +
                                       state->remainingSuggestion);
            state->buffer.clear();
            state->remainingSuggestion.clear();
            clearSuggestion(inputContext);
            event.filterAndAccept();
            return;
        case tilde::Effect::DismissSuggestion:
            inputContext->commitString(state->buffer);
            state->buffer.clear();
            state->remainingSuggestion.clear();
            clearSuggestion(inputContext);
            event.filterAndAccept();
            return;
        case tilde::Effect::ClearSuggestion:
            inputContext->commitString(state->buffer);
            state->buffer.clear();
            state->remainingSuggestion.clear();
            clearSuggestion(inputContext);
            return;
        case tilde::Effect::ShowSuggestion:
            state->buffer += printableText(event.key());
            state->remainingSuggestion = kSuggestion;
            showSuggestion(inputContext, state->buffer,
                           state->remainingSuggestion);
            event.filterAndAccept();
            return;
        case tilde::Effect::PassThrough:
            return;
        }
    }

    void reset(const InputMethodEntry &, InputContextEvent &event) override {
        auto *state = event.inputContext()->propertyFor(&stateFactory_);
        event.inputContext()->commitString(state->buffer);
        state->buffer.clear();
        state->remainingSuggestion.clear();
        clearSuggestion(event.inputContext());
    }

private:
    FactoryFor<TildeState> stateFactory_{
        [](InputContext &) { return new TildeState(); }};
};

class TildeFactory final : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        return new TildeEngine(manager->instance());
    }
};

} // namespace fcitx

FCITX_ADDON_FACTORY_V2(tilde, fcitx::TildeFactory);
