#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace tilde {

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

class OcrContextProvider {
public:
    std::string context();

private:
    std::string cachedAddress_;
    std::string cachedText_;
    std::chrono::steady_clock::time_point capturedAt_{};
};

} // namespace tilde
