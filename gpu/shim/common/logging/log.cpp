// Deception runner: see log.h.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "common/logging/log.h"

namespace Common::Log {
static const char* level_names[] = {"trace", "debug", "info", "warning", "error", "critical"};
static bool parse_level(std::string_view text, Level& out) {
    for (int i = 0; i < 6; ++i)
        if (text == level_names[i]) return out = Level(i), true;
    return false;
}
// DECEPTION_GPU_LOG: space-separated tokens, "level" for the default and "Class:level" for the
// classes starting with Class (e.g. "warning Lib.AvPlayer:trace"). Default: warning.
struct Filter {
    Level fallback = Level::warn;
    std::vector<std::pair<std::string, Level>> classes;
};
static const Filter& filter() {
    static const Filter parsed = [] {
        Filter f;
        const char* v = std::getenv("DECEPTION_GPU_LOG");
        std::string_view rest = v ? v : "";
        while (!rest.empty()) {
            const size_t end = rest.find(' ');
            const std::string_view token = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            const size_t colon = token.rfind(':');
            Level level;
            if (colon == std::string_view::npos) {
                if (parse_level(token, level)) f.fallback = level;
            } else if (parse_level(token.substr(colon + 1), level)) {
                f.classes.emplace_back(std::string(token.substr(0, colon)), level);
            }
        }
        return f;
    }();
    return parsed;
}
bool ShouldLog(std::string_view log_class, Level level) {
    const Filter& f = filter();
    for (const auto& [prefix, class_level] : f.classes)
        if (log_class.starts_with(prefix)) return level >= class_level;
    return level >= f.fallback;
}
void Write(std::string_view log_class, Level level, const char* file, int line, const char* function,
           std::string&& message) {
    static std::mutex mutex;
    static const char* names[] = {"Trace", "Debug", "Info", "Warning", "Error", "Critical"};
    const char* base = std::strrchr(file, '/');
    std::scoped_lock lock{mutex};
    std::fprintf(stderr, "GPU [%.*s] <%s> %s:%d %s: %s\n", int(log_class.size()), log_class.data(),
                 names[int(level)], base ? base + 1 : file, line, function, message.c_str());
}
} // namespace Common::Log
