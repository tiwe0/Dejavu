#include "hook_compile_diagnostics.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[noreturn]] void fail(const char *message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void check(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

size_t count_substring(
    const std::string &text,
    const std::string &needle) {
    size_t count = 0;
    size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

}  // namespace

int main() {
    static constexpr char kPreamble[] =
        "typedef int dejavu_placeholder;\n"
        "#line 1 \"hook.c\"\n";

    check(dejavu_hook_preamble_line_count(kPreamble) == 2, "count preamble lines");

    const std::string syntax = dejavu_hook_format_compile_error(
        "<string>:5: error: ';' expected (got \"}\")",
        kPreamble);
    check(
        syntax.find("hook.c:3: error: ';' expected (got \"}\")") != std::string::npos,
        "rewrite generated source line");
    check(
        syntax.find("Hint: hook source is freestanding C;") != std::string::npos,
        "add syntax hint");

    const std::string jni = dejavu_hook_format_compile_error(
        "hook.c:7: error: unknown type name 'JNIEnv'",
        kPreamble);
    check(
        jni.find("hook.c:7: error: unknown type name 'JNIEnv'") != std::string::npos,
        "preserve hook.c line");
    check(
        jni.find("Hint: raw JNI headers and types are not available here;") !=
            std::string::npos,
        "add JNI hint");
    check(
        dejavu_hook_format_compile_error(
            "hook.c:9: error: unknown type name 'jarray'",
            kPreamble)
            .find("Hint: raw JNI headers and types are not available here;") !=
            std::string::npos,
        "match broader JNI identifiers");

    const std::string unresolved = dejavu_hook_format_compile_error(
        "<string>:8: warning: implicit declaration of function 'printf'\n"
        "undefined symbol 'printf'",
        kPreamble);
    check(
        unresolved.find("hook.c:6: warning: implicit declaration of function 'printf'") !=
            std::string::npos,
        "rewrite implicit declaration line");
    check(
        count_substring(
            unresolved,
            "Hint: only the injected Hook C ABI helpers and Dejavu-registered symbols are available;") == 1,
        "deduplicate unresolved symbol hint");

    const std::string preamble_error = dejavu_hook_format_compile_error(
        "<string>:2: error: bad preamble line",
        kPreamble);
    check(
        preamble_error.find("<string>:2: error: bad preamble line") != std::string::npos,
        "leave preamble lines unchanged");

    std::cout << "OK: hook compile diagnostics\n";
    return 0;
}
