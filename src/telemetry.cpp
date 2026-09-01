#include "telemetry.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::size_t kDefaultMaximumBytes = 50U * 1024U * 1024U;

std::size_t configuredMaximumBytes() {
    const auto *value = std::getenv("OMATAB_LOG_MAX_BYTES");
    if (!value || !*value) {
        return kDefaultMaximumBytes;
    }
    char *end = nullptr;
    errno = 0;
    const auto parsed = std::strtoull(value, &end, 10);
    constexpr unsigned long long minimum = 1024ULL * 1024ULL;
    constexpr unsigned long long maximum = 1024ULL * 1024ULL * 1024ULL;
    return errno == 0 && end && *end == '\0' && parsed >= minimum &&
                   parsed <= maximum
               ? static_cast<std::size_t>(parsed)
               : kDefaultMaximumBytes;
}

std::string utcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch()) %
                              1000;
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc);
    return std::string(buffer) + "." +
           (milliseconds.count() < 100 ? "0" : "") +
           (milliseconds.count() < 10 ? "0" : "") +
           std::to_string(milliseconds.count()) + "Z";
}

bool writeAll(int descriptor, const std::string &data) {
    std::size_t written = 0;
    while (written < data.size()) {
        const auto count =
            ::write(descriptor, data.data() + written, data.size() - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return true;
}

std::string defaultLogPath() {
    if (const auto *configuredPath = std::getenv("OMATAB_LOG_PATH");
        configuredPath && *configuredPath) {
        return configuredPath;
    }
    if (const auto *stateHome = std::getenv("XDG_STATE_HOME");
        stateHome && *stateHome) {
        return std::string(stateHome) + "/omatab/events.jsonl";
    }
    if (const auto *home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.local/state/omatab/events.jsonl";
    }
    return {};
}

} // namespace

namespace omatab {

TelemetryRecorder::TelemetryRecorder()
    : path_(defaultLogPath()), maximumBytes_(configuredMaximumBytes()) {}

bool TelemetryRecorder::enabled() const {
    return enabled_.load() && !path_.empty();
}

void TelemetryRecorder::setEnabled(bool enabled) {
    if (enabled && !path_.empty()) {
        std::error_code error;
        const auto directory = std::filesystem::path(path_).parent_path();
        std::filesystem::create_directories(directory, error);
        ::chmod(directory.c_str(), S_IRWXU);
    }
    enabled_.store(enabled);
}

const std::string &TelemetryRecorder::path() const { return path_; }

void TelemetryRecorder::record(Json::Value event) {
    if (!enabled()) {
        return;
    }

    event["timestamp"] = utcTimestamp();
    event["schema_version"] = 1;
    event["process_id"] = static_cast<Json::Int64>(::getpid());
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    auto line = Json::writeString(writer, event);
    line.push_back('\n');

    std::lock_guard<std::mutex> lock(mutex_);
    struct stat status {};
    if (::stat(path_.c_str(), &status) == 0 &&
        static_cast<std::size_t>(status.st_size) + line.size() >
            maximumBytes_) {
        const auto rotated = path_ + ".1";
        ::rename(path_.c_str(), rotated.c_str());
        ::chmod(rotated.c_str(), S_IRUSR | S_IWUSR);
    }

    const auto descriptor = ::open(path_.c_str(),
                                   O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                                   S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        return;
    }
    ::fchmod(descriptor, S_IRUSR | S_IWUSR);
    writeAll(descriptor, line);
    ::close(descriptor);
}

} // namespace omatab
