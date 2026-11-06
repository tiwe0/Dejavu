#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/*
 * Process-wide, non-blocking event sink for hook callbacks.
 *
 * Hook callbacks only serialize their payload and enqueue it; a dedicated
 * worker thread owns the socket and performs all I/O. Enqueueing never blocks
 * on the network: frames are dropped when the sink is inactive or when the
 * bounded queue is full.
 *
 * Wire format (compatible with the framework stream receiver): every record is
 * a 4-byte big-endian payload length followed by the payload, where the
 * payload is a 2-byte big-endian topic length, the topic bytes, and the UTF-8
 * data bytes.
 */
class HookEventSink {
  public:
    struct State;

    static constexpr size_t kMaximumPayloadBytes = 4u * 1024u * 1024u;
    static HookEventSink &instance();

    // Starts (or restarts) the sink. The endpoint is "host:port" for TCP.
    // The call returns immediately; the worker connects in the background.
    bool start(const std::string &endpoint, std::string *error);
    void stop();
    // Acquire an idle channel without replacing another caller's stream.
    // A lease survives connection failure until released; stale releases are inert.
    bool acquire(const std::string &endpoint, uint64_t *lease, std::string *error);
    bool release(uint64_t lease);
    bool active() const;

    // Enqueues one frame. Returns false when the frame was dropped.
    bool emit(const char *topic, const char *data, size_t size);

    // Account for serialization failures before a frame can be enqueued.
    void record_drop();
    std::string stats() const;

  private:
    HookEventSink();
    ~HookEventSink();
    HookEventSink(const HookEventSink &) = delete;
    HookEventSink &operator=(const HookEventSink &) = delete;

    State *state_;
};
