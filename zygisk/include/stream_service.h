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
    int write(int handle, const void *data, size_t size);
    int close(int handle);
    size_t close_owner(uint64_t owner_token);

  private:
    std::shared_ptr<Stream> find(int handle);
    int open_socket(Stream *stream, uint32_t transport, const std::string &endpoint,
                    std::string *error);
    int open_kcp(Stream *stream, const std::string &endpoint, std::string *error);

    mutable std::mutex mutex_;
    std::map<int, std::shared_ptr<Stream>> streams_;
    std::map<int, uint64_t> owners_;
    int next_handle_ = 1;
};
