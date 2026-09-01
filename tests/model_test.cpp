#include <iostream>
#include <sstream>
#include <string>

#include <json/json.h>

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
        tilde::buildOllamaRequest("test-model", "quoted \"prefix\"");
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
    expect(!parsedRequest["stream"].asBool(),
           "request disables streaming");
    expect(parsedRequest["options"]["num_predict"].asInt() == 16,
           "request caps output tokens");

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

    return failures == 0 ? 0 : 1;
}
