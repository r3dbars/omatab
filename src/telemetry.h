#pragma once

#include <cstddef>
#include <mutex>
#include <string>

#include <json/json.h>

namespace tilde {

class TelemetryRecorder {
public:
    TelemetryRecorder();

    bool enabled() const;
    void record(Json::Value event);

private:
    std::string path_;
    std::size_t maximumBytes_ = 0;
    std::mutex mutex_;
};

} // namespace tilde
