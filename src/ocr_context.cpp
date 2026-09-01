#include "ocr_context.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>

#include <json/json.h>

namespace {

constexpr std::size_t kMaximumCommandOutput = 64 * 1024;
constexpr std::size_t kMaximumOcrContext = 4096;
constexpr auto kOcrCacheDuration = std::chrono::seconds(2);

std::string commandOutput(const std::string &command) {
    std::string output;
    auto *pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return output;
    }

    std::array<char, 4096> buffer{};
    while (output.size() < kMaximumCommandOutput) {
        const auto count = fread(buffer.data(), 1, buffer.size(), pipe);
        if (count == 0) {
            break;
        }
        output.append(buffer.data(), count);
    }
    pclose(pipe);
    return output;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char value) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(value)));
    });
    return value;
}

std::string normalizeOcr(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    while (!text.empty() && std::isspace(
                                static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    if (text.size() > kMaximumOcrContext) {
        text.erase(0, text.size() - kMaximumOcrContext);
    }
    return text;
}

} // namespace

namespace tilde {

ActiveWindow parseActiveWindow(std::string_view json) {
    ActiveWindow window;
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errors;
    std::istringstream input{std::string(json)};
    if (!Json::parseFromStream(reader, input, &root, &errors) ||
        !root["at"].isArray() || root["at"].size() != 2 ||
        !root["size"].isArray() || root["size"].size() != 2) {
        return window;
    }

    window.address = root["address"].asString();
    window.windowClass = root["class"].asString();
    window.title = root["title"].asString();
    window.x = root["at"][0].asInt();
    window.y = root["at"][1].asInt();
    window.width = root["size"][0].asInt();
    window.height = root["size"][1].asInt();
    window.valid = !window.address.empty() && window.width > 0 &&
                   window.height > 0 && window.width <= 16384 &&
                   window.height <= 16384;
    return window;
}

bool ocrAllowedForWindow(const ActiveWindow &window) {
    if (!window.valid) {
        return false;
    }
    const auto identity = lowercase(window.windowClass + " " + window.title);
    constexpr std::array blockedTerms{
        "1password", "bitwarden", "keepass", "seahorse", "pinentry",
        "polkit", "authentication", "password manager"};
    return std::none_of(blockedTerms.begin(), blockedTerms.end(),
                        [&identity](std::string_view term) {
                            return identity.find(term) != std::string::npos;
                        });
}

std::string OcrContextProvider::context() {
    const auto window = parseActiveWindow(
        commandOutput("hyprctl activewindow -j 2>/dev/null"));
    if (!ocrAllowedForWindow(window)) {
        return {};
    }

    const auto now = std::chrono::steady_clock::now();
    if (window.address == cachedAddress_ && !cachedText_.empty() &&
        now - capturedAt_ < kOcrCacheDuration) {
        return cachedText_;
    }

    std::ostringstream command;
    command << "grim -g '" << window.x << ',' << window.y << ' '
            << window.width << 'x' << window.height
            << "' - | tesseract stdin stdout --oem 1 --psm 6 -l eng "
               "--dpi 180 -c preserve_interword_spaces=1 2>/dev/null";
    cachedText_ = normalizeOcr(commandOutput(command.str()));
    cachedAddress_ = window.address;
    capturedAt_ = now;
    return cachedText_;
}

} // namespace tilde
