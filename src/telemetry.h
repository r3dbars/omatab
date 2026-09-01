#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>

#include <json/json.h>

namespace omatab {

class TelemetryRecorder {
public:
    // Off until setEnabled(true). The log lives at OMATAB_LOG_PATH when set,
    // otherwise $XDG_STATE_HOME/omatab/events.jsonl.
    TelemetryRecorder();

    bool enabled() const;
    void setEnabled(bool enabled);
    const std::string &path() const;
    void record(Json::Value event);

private:
    std::string path_;
    std::size_t maximumBytes_ = 0;
    std::atomic<bool> enabled_{false};
    std::mutex mutex_;
};

} // namespace omatab
