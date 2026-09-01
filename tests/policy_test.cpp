#include <iostream>

#include "policy.h"
#include "suggestion.h"

namespace {

int failures = 0;

void expect(const char *name, tilde::Effect actual, tilde::Effect expected) {
    if (actual == expected) {
        std::cout << "PASS " << name << '\n';
        return;
    }
    std::cerr << "FAIL " << name << '\n';
    ++failures;
}

} // namespace

int main() {
    using tilde::Effect;
    using tilde::Event;

    expect("printable shows suggestion", decide(false, Event::Printable),
           Effect::ShowSuggestion);
    expect("tab accepts visible suggestion", decide(true, Event::Tab),
           Effect::AcceptNextWord);
    expect("tab passes through without suggestion", decide(false, Event::Tab),
           Effect::PassThrough);
    expect("backtick accepts full visible suggestion",
           decide(true, Event::FullAccept), Effect::AcceptFullSuggestion);
    expect("backtick passes through without suggestion",
           decide(false, Event::FullAccept), Effect::PassThrough);
    expect("escape dismisses visible suggestion", decide(true, Event::Escape),
           Effect::DismissSuggestion);
    expect("escape passes through without suggestion", decide(false, Event::Escape),
           Effect::PassThrough);
    expect("editing clears visible suggestion", decide(true, Event::Editing),
           Effect::ClearSuggestion);
    expect("editing passes through without suggestion", decide(false, Event::Editing),
           Effect::PassThrough);
    expect("unrelated input passes through", decide(true, Event::Other),
           Effect::PassThrough);

    const std::string suggestion = " — Tilde is working";
    const auto first = tilde::nextWordLength(suggestion);
    if (suggestion.substr(0, first) == " — Tilde ") {
        std::cout << "PASS first word keeps leading punctuation and space\n";
    } else {
        std::cerr << "FAIL first word keeps leading punctuation and space\n";
        ++failures;
    }

    const auto second = tilde::nextWordLength("is working");
    if (std::string("is working").substr(0, second) == "is ") {
        std::cout << "PASS next word includes trailing space\n";
    } else {
        std::cerr << "FAIL next word includes trailing space\n";
        ++failures;
    }

    return failures == 0 ? 0 : 1;
}
