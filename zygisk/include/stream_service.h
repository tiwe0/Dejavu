#pragma once

#ifndef DEJAVU_STREAM_TRANSPORT_TCP
#define DEJAVU_STREAM_TRANSPORT_TCP 1u
#define DEJAVU_STREAM_TRANSPORT_UDP 2u
#define DEJAVU_STREAM_TRANSPORT_KCP 3u
#endif

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

/*
 * Framework-level capture stream transports. Plugins open a stream through the
 * host "stream" interface; the agent owns the socket/KCP state and closes all
 * handles owned by a plugin when it is unloaded.
 */
class StreamService {
  public:
    StreamService();
    ~StreamService();

    StreamService(const StreamService &) = delete;
    StreamService &operator=(const StreamService &) = delete;

    struct Stream;

    int open(uint64_t owner_token, uint32_t transport, const std::string &endpoint,
             std::string *error);
    // Bind a TCP listener. The endpoint is host:port; use 0.0.0.0 to accept
    // connections from outside the app process (network permission required).
    int listen(uint64_t owner_token, uint32_t transport, const std::string &endpoint, int backlog,
               std::string *error);
    // An expected_owner other than zero atomically checks ownership with lookup.
    // Zero is reserved for internal callers that already hold trusted handles.
    // Accept one pending TCP connection. Returns a positive handle, 0 on
    // timeout, or -1 on error. Zero timeout polls without waiting.
    int accept(int listener_handle, int timeout_ms, uint64_t expected_owner = 0);
    // Read exactly one framed record into buffer (4-byte network-order length
    // prefix + payload, matching write()). Returns 1 with *size_read set, 0 on
    // timeout, or -1 on EOF/error/oversized record. timeout_ms < 0 blocks;
    // zero polls without waiting, and finite deadlines include read-lock waits.
    int read(int handle, void *buffer, size_t capacity, int timeout_ms, size_t *size_read,
             uint64_t expected_owner = 0);
    // TCP writes have a 100 ms deadline, including waiting for another writer.
    // A failed TCP transmission retires the stream; close it and open a new one.
    int write(int handle, const void *data, size_t size, uint64_t expected_owner = 0);
    int close(int handle, uint64_t expected_owner = 0);
    size_t close_owner(uint64_t owner_token);
    // Permanently reject new streams for this owner and close its existing ones.
    // Plugin owner tokens are unique for the lifetime of the service.
    size_t retire_owner(uint64_t owner_token);
    // True when handle exists and is owned by owner_token.
    bool owned_by(int handle, uint64_t owner_token) const;

  private:
    std::shared_ptr<Stream> find(int handle, uint64_t expected_owner = 0);
    int register_stream(const std::shared_ptr<Stream> &stream, uint64_t owner_token,
                        std::string *error);
    int open_socket(Stream *stream, uint32_t transport, const std::string &endpoint,
                    std::string *error);
    int open_kcp(Stream *stream, const std::string &endpoint, std::string *error);

    mutable std::mutex mutex_;
    std::map<int, std::shared_ptr<Stream>> streams_;
    std::map<int, uint64_t> owners_;
    std::set<uint64_t> retired_owners_;
    int next_handle_ = 1;
};
