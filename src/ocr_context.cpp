#include "ocr_context.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>

#include <json/json.h>

namespace {

constexpr std::size_t kMaximumCommandOutput = 64 * 1024;
constexpr std::size_t kMaximumOcrContext = 4096;
constexpr auto kOcrCacheDuration = std::chrono::seconds(2);
constexpr long kDefaultMaximumOcrAgeMs = 5000;

// Oldest capture worth sending. Beyond this a request goes without OCR
// context while a refresh runs. Override with OMATAB_OCR_MAX_AGE_MS.
std::chrono::milliseconds configuredMaximumOcrAge() {
    const auto *value = std::getenv("OMATAB_OCR_MAX_AGE_MS");
    if (!value || !*value) {
        return std::chrono::milliseconds(kDefaultMaximumOcrAgeMs);
    }
    char *end = nullptr;
    errno = 0;
    const auto parsed = std::strtol(value, &end, 10);
    return errno == 0 && end && *end == '\0' && parsed >= 500 &&
                   parsed <= 60000
               ? std::chrono::milliseconds(parsed)
               : std::chrono::milliseconds(kDefaultMaximumOcrAgeMs);
}

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

namespace omatab {

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

namespace {

ActiveWindow hyprlandActiveWindow() {
    return parseActiveWindow(
        commandOutput("hyprctl activewindow -j 2>/dev/null"));
}

std::string captureWindowText(const ActiveWindow &window) {
    std::ostringstream command;
    command << "grim -g '" << window.x << ',' << window.y << ' '
            << window.width << 'x' << window.height
            << "' - | tesseract stdin stdout --oem 1 --psm 6 -l eng "
               "--dpi 180 -c preserve_interword_spaces=1 2>/dev/null";
    return normalizeOcr(commandOutput(command.str()));
}

} // namespace

OcrContextProvider::OcrContextProvider()
    : OcrContextProvider(hyprlandActiveWindow, captureWindowText,
                         kOcrCacheDuration, configuredMaximumOcrAge()) {}

OcrContextProvider::OcrContextProvider(WindowSource windowSource,
                                       Capture capture,
                                       std::chrono::milliseconds cacheDuration,
                                       std::chrono::milliseconds maximumAge)
    : windowSource_(std::move(windowSource)), capture_(std::move(capture)),
      cacheDuration_(cacheDuration), maximumAge_(maximumAge),
      thread_([this] { run(); }) {}

OcrContextProvider::~OcrContextProvider() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

OcrSnapshot OcrContextProvider::snapshot() {
    activeWindow_ = windowSource_();
    if (!ocrAllowedForWindow(activeWindow_)) {
        return {};
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    OcrSnapshot result;
    const bool sameWindow = activeWindow_.address == cachedAddress_;
    if (!sameWindow) {
        // Focus moved: what the previous window showed is irrelevant now.
        cachedAddress_.clear();
        cachedText_.clear();
        capturedAt_ = {};
    }
    const bool captured = sameWindow && capturedAt_ != decltype(capturedAt_){};
    const auto age = captured ? now - capturedAt_ : cacheDuration_ * 0;
    if (captured && !cachedText_.empty() && age < maximumAge_) {
        result.text = cachedText_;
        result.ageMs =
            std::chrono::duration<double, std::milli>(age).count();
    }

    const bool fresh = captured && age < cacheDuration_;
    if (!fresh) {
        // Coalesce: only the newest geometry matters if several requests
        // arrive while a capture is running.
        pendingWindow_ = activeWindow_;
        condition_.notify_all();
    }
    result.refreshing = refreshing_ || pendingWindow_.has_value();
    return result;
}

std::string OcrContextProvider::context() { return snapshot().text; }

void OcrContextProvider::refreshActiveWindow() {
    activeWindow_ = windowSource_();
}

const ActiveWindow &OcrContextProvider::activeWindow() const {
    return activeWindow_;
}

bool OcrContextProvider::waitForRefresh(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] {
        return stopping_ || (!refreshing_ && !pendingWindow_.has_value());
    });
}

void OcrContextProvider::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        condition_.wait(lock, [this] {
            return stopping_ || pendingWindow_.has_value();
        });
        if (stopping_) {
            break;
        }
        const auto window = std::move(*pendingWindow_);
        pendingWindow_.reset();
        refreshing_ = true;
        lock.unlock();

        auto text = capture_(window);
        const auto capturedAt = std::chrono::steady_clock::now();

        lock.lock();
        refreshing_ = false;
        cachedAddress_ = window.address;
        cachedText_ = std::move(text);
        capturedAt_ = capturedAt;
        condition_.notify_all();
    }
}

} // namespace omatab
