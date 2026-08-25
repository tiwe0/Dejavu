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
int dejavu_control_open_listener(DejavuControlEndpoint *endpoint);
int dejavu_control_connect(const DejavuControlEndpoint &endpoint);
