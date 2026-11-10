#include "agent_startup_scripts.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

#include "control_channel.h"

AgentScriptReadStatus agent_read_script_at(int directory_fd, const char *path, std::string *script,
                                           std::string *error) {
    script->clear();
    error->clear();
    // Nonblocking open ensures a configured FIFO cannot stall process startup before fstat.
    const int fd = openat(directory_fd, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        const int saved_errno = errno;
        error->assign(strerror(saved_errno));
        return saved_errno == ENOENT ? AgentScriptReadStatus::kMissing
                                     : AgentScriptReadStatus::kError;
    }
    struct stat status = {};
    if (fstat(fd, &status) != 0) {
        error->assign(strerror(errno));
        close(fd);
        return AgentScriptReadStatus::kError;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
        status.st_size > static_cast<off_t>(kDejavuMaximumControlScriptSize)) {
        error->assign("script must be a regular file of at most 1 MiB");
        close(fd);
        return AgentScriptReadStatus::kError;
    }
    std::string text(static_cast<size_t>(status.st_size), '\0');
    size_t offset = 0;
    while (offset < text.size()) {
        const ssize_t count = read(fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            error->assign(count == 0 ? "truncated startup script" : strerror(errno));
            close(fd);
            return AgentScriptReadStatus::kError;
        }
        offset += static_cast<size_t>(count);
    }
    close(fd);
    if (text.find('\0') != std::string::npos) {
        error->assign("startup script contains a NUL byte");
        return AgentScriptReadStatus::kError;
    }
    *script = std::move(text);
    return AgentScriptReadStatus::kLoaded;
}

AgentScriptReadStatus agent_read_early_hook_at(int directory_fd, const std::string &process,
                                               std::string *script, std::string *error) {
    if (process.empty() || process.size() > 128 ||
        process.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-") !=
            std::string::npos) {
        script->clear();
        error->assign("invalid early-hook process name");
        return AgentScriptReadStatus::kError;
    }
    const std::string path = "config/early-hooks/" + process + ".lua";
    return agent_read_script_at(directory_fd, path.c_str(), script, error);
}
