#include <cassert>
#include <cstdio>
#include <string>
#include "hook_api_runtime.h"
#include "hook_event_sink.h"

namespace {
std::string captured;
unsigned int emitted = 0;
unsigned int dropped = 0;
bool enabled = true;
void reset() { captured.clear(); emitted = 0; dropped = 0; }
void send(JNIEnv &env) {
    dejavu_hook_context context{};
    context.env = &env;
    assert(dejavu_hook_emit(&context, "test", &env) == DEJAVU_HOOK_OK);
}
}
HookEventSink::HookEventSink() : state_(nullptr) {}
HookEventSink::~HookEventSink() = default;
HookEventSink &HookEventSink::instance() { static HookEventSink sink; return sink; }
bool HookEventSink::emit(const char *, const char *data, size_t size) {
    captured.assign(data, size);
    ++emitted;
    return true;
}
// Defined without using production internals; serialisation is the real source.
void HookEventSink::record_drop() { ++dropped; }
bool HookEventSink::active() const { return enabled; }

int main() {
    JNIEnv env;
    env.text = u"中文 😀";
    env.text.push_back(0);
    env.modified = "\xe4\xb8\xad\xe6\x96\x87 \xed\xa0\xbd\xed\xb8\x80\xc0\x80";
    send(env);
    std::string expected = "中文 😀";
    expected.push_back(0);
    assert(emitted == 1 && captured == expected);
    std::puts("OK: hook events encode Chinese, supplementary characters and NUL as UTF-8");

    reset();
    env.text = {0xd800, 'a', 0xdc00, 0xd800, 0xdc00};
    send(env);
    assert(captured == "\xef\xbf\xbd" "a" "\xef\xbf\xbd\xf0\x90\x80\x80");
    std::puts("OK: unpaired surrogates become U+FFFD");

    reset();
    env.text = {'a', 0xd800};
    send(env);
    assert(emitted == 1 && dropped == 0 && captured == "a\xef\xbf\xbd");
    std::puts("OK: a trailing high surrogate becomes U+FFFD");

    reset();
    env.text.assign(1023, 'a');
    env.text.push_back(0xd83d);
    env.text.push_back(0xde00);
    send(env);
    assert(captured == std::string(1023, 'a') + "😀");
    assert(env.largest_region <= 1024);
    std::puts("OK: surrogate pairs crossing JNI regions stay intact");

    reset();
    env.text.assign(4u * 1024u * 1024u, 'a');
    env.regions = 0;
    send(env);
    assert(emitted == 1 && captured.size() == env.text.size());
    reset();
    env.text.push_back('a');
    env.regions = 0;
    send(env);
    assert(emitted == 0 && dropped == 1 && env.regions == 0);
    env.text.assign(2u * 1024u * 1024u, 0x4e2d);
    send(env);
    assert(emitted == 0 && dropped == 2);
    std::puts("OK: over-limit Java strings are dropped without whole-string JNI copies");

    reset();
    const size_t limit = HookEventSink::kMaximumPayloadBytes;
    env.text.assign(limit / 3, 0x4e2d);
    env.text.append(limit % 3, u'a');
    send(env);
    assert(emitted == 1 && dropped == 0 && captured.size() == limit);
    assert(captured.substr(0, 3) == "中" && captured.back() == 'a');
    reset();
    env.text.push_back(0x4e2d);
    send(env);
    assert(emitted == 0 && dropped == 1 && captured.empty());
    std::puts("OK: multibyte UTF-8 at the payload limit is accepted and excess is dropped");

    for (const auto failure : {JNIEnv::Failure::find_class, JNIEnv::Failure::string_length,
                               JNIEnv::Failure::string_region}) {
        reset();
        JNIEnv failing;
        failing.text.assign(2048, u'a');
        failing.failure = failure;
        failing.fail_region = 2; // Fail after a first region has already been converted.
        send(failing);
        assert(emitted == 0 && dropped == 1 && captured.empty());
        assert(!failing.pending_exception && failing.cleared_exceptions == 1);
        failing.failure = JNIEnv::Failure::none;
        failing.text = u"recovered 😀";
        send(failing);
        assert(emitted == 1 && dropped == 1 && captured == "recovered 😀");
        std::puts(failure == JNIEnv::Failure::find_class
            ? "OK: class lookup failure clears JNI exceptions and the next event recovers"
            : failure == JNIEnv::Failure::string_length
            ? "OK: length lookup failure clears JNI exceptions and the next event recovers"
            : "OK: mid-string JNI failure drops the partial payload and the next event recovers");
    }
    reset();
    enabled = false;
    env.text = u"inactive";
    env.regions = 0;
    send(env);
    assert(emitted == 0 && dropped == 1 && env.regions == 0);
    std::puts("OK: inactive event sinks skip Java serialization");
}
