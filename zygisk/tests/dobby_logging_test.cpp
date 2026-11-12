#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

static std::string output;
static int Capture(const char *format, va_list arguments) {
    assert(std::strcmp(format, "%s") == 0);
    output = va_arg(arguments, const char *);
    return static_cast<int>(output.size());
}
#if defined(TEST_ANDROID_LOGGING)
extern "C" int __android_log_print(int, const char *, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int count = Capture(format, arguments);
    va_end(arguments);
    return count;
}
#define __ANDROID__ 1
#else
static int TestPrintf(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int count = Capture(format, arguments);
    va_end(arguments);
    return count;
}
#define printf TestPrintf
#endif
#undef __APPLE__
#include DOBBY_LOGGING_SOURCE

int main() {
    logger_log_impl(nullptr, LOG_LEVEL_ERROR, "%s", "payload 100% %p %s %% end");
    assert(output == "payload 100% %p %s %% end\n");
}
