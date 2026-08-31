#include <iostream>

#include "policy.h"

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
           Effect::AcceptSuggestion);
    expect("tab passes through without suggestion", decide(false, Event::Tab),
           Effect::PassThrough);
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

    return failures == 0 ? 0 : 1;
}
