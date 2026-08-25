#include "target_config.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

namespace {

constexpr char kTargetsPath[] = "config/targets.txt";
constexpr size_t kMaximumConfigSize = 16 * 1024;

bool is_space(char value) {
    return value == ' ' || value == '\t' || value == '\r';
}

bool line_matches(const char *begin, const char *end, const char *process_name) {
    while (begin < end && is_space(*begin)) {
        ++begin;
    }
    while (end > begin && is_space(end[-1])) {
        --end;
    }
    if (begin == end || *begin == '#') {
        return false;
    }
    const size_t length = static_cast<size_t>(end - begin);
    return strlen(process_name) == length && memcmp(begin, process_name, length) == 0;
}

}  // namespace

bool target_config_matches(int module_dir_fd, const char *process_name) {
    char buffer[kMaximumConfigSize + 1];
    size_t used = 0;
    int config_fd;

    if (module_dir_fd < 0 || process_name == nullptr || process_name[0] == '\0') {
        return false;
    }
    config_fd = openat(module_dir_fd, kTargetsPath, O_RDONLY | O_CLOEXEC);
    if (config_fd < 0) {
        return false;
    }

    while (used < kMaximumConfigSize) {
        const ssize_t count = read(config_fd, buffer + used, kMaximumConfigSize - used);
        if (count < 0) {
            close(config_fd);
            return false;
        }
        if (count == 0) {
            break;
        }
        used += static_cast<size_t>(count);
    }
    if (used == kMaximumConfigSize) {
        char extra;
        if (read(config_fd, &extra, 1) != 0) {
            close(config_fd);
            return false;
        }
    }
    close(config_fd);
    buffer[used] = '\0';

    const char *line = buffer;
    const char *limit = buffer + used;
    while (line < limit) {
        const char *end = static_cast<const char *>(memchr(line, '\n', limit - line));
        if (end == nullptr) {
            end = limit;
        }
        if (line_matches(line, end, process_name)) {
            return true;
        }
        line = end < limit ? end + 1 : limit;
    }
    return false;
}
