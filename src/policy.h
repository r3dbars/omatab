#pragma once

namespace tilde {

enum class Event {
    Printable,
    Tab,
    Escape,
    Editing,
    Other,
};

enum class Effect {
    PassThrough,
    ShowSuggestion,
    AcceptSuggestion,
    DismissSuggestion,
    ClearSuggestion,
};

constexpr Effect decide(bool suggestionVisible, Event event) {
    switch (event) {
    case Event::Printable:
        return Effect::ShowSuggestion;
    case Event::Tab:
        return suggestionVisible ? Effect::AcceptSuggestion
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
