#include "target_config.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

void check(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

void write_config(int directory_fd, const char *contents) {
    const int fd = openat(
        directory_fd,
        "config/targets.txt",
        O_WRONLY | O_CREAT | O_TRUNC,
        0600);
    if (fd < 0) {
        fail("open target config");
    }
    const size_t length = strlen(contents);
    if (write(fd, contents, length) != static_cast<ssize_t>(length)) {
        close(fd);
        fail("write target config");
    }
    close(fd);
}

}  // namespace

int main() {
    char path[] = "/tmp/dejavu-zygisk-targets.XXXXXX";
    if (mkdtemp(path) == nullptr) {
        fail("create temporary directory");
    }

    const int root_fd = open(path, O_RDONLY | O_DIRECTORY);
    if (root_fd < 0 || mkdirat(root_fd, "config", 0700) != 0) {
        fail("create config directory");
    }

    check(!target_config_matches(root_fd, "com.example.app"), "missing config rejects target");

    write_config(
        root_fd,
        "# targets\n"
        "\n"
        "  com.example.app  \r\n"
        "com.example.app:worker\n");
    check(target_config_matches(root_fd, "com.example.app"), "matches exact app process");
    check(
        target_config_matches(root_fd, "com.example.app:worker"),
        "matches exact worker process");
    check(!target_config_matches(root_fd, "com.example"), "rejects prefix");
    check(!target_config_matches(root_fd, "com.example.app:other"), "rejects other process");

    write_config(root_fd, "# no enabled targets\n");
    check(!target_config_matches(root_fd, "com.example.app"), "comment-only config rejects target");

    unlinkat(root_fd, "config/targets.txt", 0);
    unlinkat(root_fd, "config", AT_REMOVEDIR);
    close(root_fd);
    rmdir(path);
    puts("OK: exact Zygisk target selection");
    return 0;
}
