#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <string>

constexpr size_t kDejavuMaximumControlScriptSize = 1024 * 1024;
constexpr size_t kDejavuControlSocketNameCapacity = 64;
constexpr size_t kDejavuControlTokenSize = 32;

struct DejavuControlClientInfo {
    pid_t pid = -1;
    std::string process_name;
};

struct DejavuControlEndpoint {
    char socket_name[kDejavuControlSocketNameCapacity] = {};
    uint8_t token[kDejavuControlTokenSize] = {};
};

enum class DejavuControlRpcType : uint32_t {
    kExecuteRequest = 1,
    kExecuteResponse = 2,
    kReconnectRequest = 3,
    kReconnectResponse = 4,
};

constexpr int32_t DEJAVU_CONTROL_RPC_OK = 0;
constexpr int32_t DEJAVU_CONTROL_RPC_EXECUTION_ERROR = 1;
constexpr int32_t DEJAVU_CONTROL_RPC_UNAVAILABLE = 2;
constexpr int32_t DEJAVU_CONTROL_RPC_PROTOCOL_ERROR = 3;

struct DejavuControlRpcMessage {
    DejavuControlRpcType type = DejavuControlRpcType::kExecuteRequest;
    uint64_t request_id = 0;
    int32_t status = DEJAVU_CONTROL_RPC_OK;
    std::string payload;
};

bool dejavu_control_read_frame(int fd, std::string *payload, size_t maximum_size);
bool dejavu_control_write_frame(int fd, const void *payload, size_t payload_size);
bool dejavu_control_file_path(const std::string &process_name, std::string *path);
bool dejavu_control_write_client_info(
    int fd, pid_t pid, const std::string &process_name);
bool dejavu_control_read_client_info(int fd, DejavuControlClientInfo *info);
bool dejavu_control_write_endpoint(int fd, const DejavuControlEndpoint &endpoint);
bool dejavu_control_read_endpoint(int fd, DejavuControlEndpoint *endpoint);
bool dejavu_control_write_authentication(
    int fd, const DejavuControlEndpoint &endpoint);
bool dejavu_control_read_authentication(
    int fd, const DejavuControlEndpoint &endpoint);
bool dejavu_control_write_rpc(int fd, const DejavuControlRpcMessage &message);
bool dejavu_control_read_rpc(int fd, DejavuControlRpcMessage *message);
int dejavu_control_open_listener(DejavuControlEndpoint *endpoint);
int dejavu_control_connect(const DejavuControlEndpoint &endpoint);
