#pragma once

namespace tilde {

enum class Event {
    Printable,
    Tab,
    FullAccept,
    Escape,
    Editing,
    Other,
};

enum class Effect {
    PassThrough,
    ShowSuggestion,
    AcceptNextWord,
    AcceptFullSuggestion,
    DismissSuggestion,
    ClearSuggestion,
};

constexpr Effect decide(bool suggestionVisible, Event event) {
    switch (event) {
    case Event::Printable:
        return Effect::ShowSuggestion;
    case Event::Tab:
        return suggestionVisible ? Effect::AcceptNextWord
                                 : Effect::PassThrough;
    case Event::FullAccept:
        return suggestionVisible ? Effect::AcceptFullSuggestion
                                 : Effect::PassThrough;
    case Event::Escape:
        return suggestionVisible ? Effect::DismissSuggestion
                                 : Effect::PassThrough;
    case Event::Editing:
        return suggestionVisible ? Effect::ClearSuggestion
                                 : Effect::PassThrough;
    case Event::Other:
        return Effect::PassThrough;
    }
    return Effect::PassThrough;
}

} // namespace tilde
