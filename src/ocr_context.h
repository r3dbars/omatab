#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace omatab {

struct ActiveWindow {
    std::string address;
    std::string windowClass;
    std::string title;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool valid = false;
};

ActiveWindow parseActiveWindow(std::string_view json);
bool ocrAllowedForWindow(const ActiveWindow &window);

// What the caller gets back without waiting: whatever text is already known
// for the active window, plus how old it is. A refresh may be running.
struct OcrSnapshot {
    std::string text;
    // Milliseconds since the text was captured; negative when no text.
    double ageMs = -1.0;
    bool refreshing = false;
};

// Screenshot-plus-OCR is slow (about a second), so it never runs on the
// caller's thread. snapshot() answers immediately from cache and schedules a
// background refresh when the cache is missing or older than the cache
// duration. Text older than the maximum age is not served at all: the request
// goes without OCR context rather than with a stale picture of the screen. A
// change of active window discards the previous window's capture outright.
// The active-window lookup and safety check stay synchronous so a cached
// capture is never handed out for a window that is now blocked.
class OcrContextProvider {
public:
    using WindowSource = std::function<ActiveWindow()>;
    using Capture = std::function<std::string(const ActiveWindow &)>;

    OcrContextProvider();
    OcrContextProvider(WindowSource windowSource, Capture capture,
                       std::chrono::milliseconds cacheDuration,
                       std::chrono::milliseconds maximumAge);
    ~OcrContextProvider();

    OcrContextProvider(const OcrContextProvider &) = delete;
    OcrContextProvider &operator=(const OcrContextProvider &) = delete;

    OcrSnapshot snapshot();
    // Convenience wrapper returning only the text.
    std::string context();
    // Updates activeWindow() without capturing anything. Used when screen
    // context is switched off but telemetry still wants the window class.
    void refreshActiveWindow();
    const ActiveWindow &activeWindow() const;

    // Blocks until no refresh is running or pending, or the timeout passes.
    // Intended for tests and diagnostics.
    bool waitForRefresh(std::chrono::milliseconds timeout);

private:
    void run();

    WindowSource windowSource_;
    Capture capture_;
    std::chrono::milliseconds cacheDuration_;
    std::chrono::milliseconds maximumAge_;
    ActiveWindow activeWindow_;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<ActiveWindow> pendingWindow_;
    bool refreshing_ = false;
    bool stopping_ = false;
    std::string cachedAddress_;
    std::string cachedText_;
    std::chrono::steady_clock::time_point capturedAt_{};
    std::thread thread_;
};

} // namespace omatab
