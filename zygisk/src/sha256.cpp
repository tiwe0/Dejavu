#include "sha256.h"

#include <array>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

inline uint32_t rotate_right(uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

void sha256_transform(std::array<uint32_t, 8> *state, const uint8_t block[64]) {
    uint32_t words[64] = {};
    for (int index = 0; index < 16; ++index) {
        words[index] = (static_cast<uint32_t>(block[index * 4]) << 24) |
                       (static_cast<uint32_t>(block[index * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(block[index * 4 + 2]) << 8) |
                       static_cast<uint32_t>(block[index * 4 + 3]);
    }
    for (int index = 16; index < 64; ++index) {
        const uint32_t s0 = rotate_right(words[index - 15], 7) ^
                            rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3);
        const uint32_t s1 = rotate_right(words[index - 2], 17) ^
                            rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    uint32_t a = (*state)[0];
    uint32_t b = (*state)[1];
    uint32_t c = (*state)[2];
    uint32_t d = (*state)[3];
    uint32_t e = (*state)[4];
    uint32_t f = (*state)[5];
    uint32_t g = (*state)[6];
    uint32_t h = (*state)[7];
    for (int index = 0; index < 64; ++index) {
        const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + s1 + choice + kRoundConstants[index] + words[index];
        const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    (*state)[0] += a;
    (*state)[1] += b;
    (*state)[2] += c;
    (*state)[3] += d;
    (*state)[4] += e;
    (*state)[5] += f;
    (*state)[6] += g;
    (*state)[7] += h;
}

} // namespace

std::string sha256_hex(const void *data, size_t size) {
    std::array<uint32_t, 8> state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const auto *bytes = static_cast<const uint8_t *>(data);
    size_t offset = 0;
    while (size - offset >= 64) {
        sha256_transform(&state, bytes + offset);
        offset += 64;
    }
    uint8_t remainder[128] = {};
    const size_t tail = size - offset;
    if (tail != 0) {
        std::memcpy(remainder, bytes + offset, tail);
    }
    remainder[tail] = 0x80;
    const size_t padded_size = tail < 56 ? 64 : 128;
    const uint64_t bit_length = static_cast<uint64_t>(size) * 8;
    for (int index = 0; index < 8; ++index) {
        remainder[padded_size - 1 - index] = static_cast<uint8_t>(bit_length >> (index * 8));
    }
    sha256_transform(&state, remainder);
    if (padded_size == 128) {
        sha256_transform(&state, remainder + 64);
    }
    static const char kHex[] = "0123456789abcdef";
    std::string digest;
    digest.resize(64);
    for (int index = 0; index < 8; ++index) {
        for (int byte = 0; byte < 4; ++byte) {
            const uint8_t value = static_cast<uint8_t>(state[index] >> (24 - byte * 8));
            digest[static_cast<size_t>(index * 8 + byte * 2)] = kHex[value >> 4];
            digest[static_cast<size_t>(index * 8 + byte * 2 + 1)] = kHex[value & 0x0fu];
        }
    }
    return digest;
}

bool sha256_file_hex(const std::string &path, std::string *digest, std::string *error) {
    constexpr size_t kMaximumHashSize = 64 * 1024 * 1024;
    struct stat information = {};
    if (path.empty() || lstat(path.c_str(), &information) != 0 || !S_ISREG(information.st_mode)) {
        *error = "hash target must be a regular file";
        return false;
    }
    if (information.st_size < 0 ||
        static_cast<unsigned long long>(information.st_size) > kMaximumHashSize) {
        *error = "hash target is too large";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        *error = "unable to open file for hash verification";
        return false;
    }
    std::string content;
    content.reserve(static_cast<size_t>(information.st_size));
    content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (file.bad()) {
        *error = "unable to read file for hash verification";
        return false;
    }
    *digest = sha256_hex(content.data(), content.size());
    return true;
}

bool sha256_fd_hex(int fd, unsigned long long size, std::string *digest, std::string *error) {
    constexpr unsigned long long kMaximumHashSize = 64ULL * 1024 * 1024;
    if (fd < 0 || digest == nullptr || size > kMaximumHashSize) {
        *error = "invalid fd hash target";
        return false;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        *error = "unable to seek hash target";
        return false;
    }
    std::string content;
    content.reserve(static_cast<size_t>(size));
    char buffer[16384];
    unsigned long long remaining = size;
    while (remaining > 0) {
        const size_t wanted =
            remaining < sizeof(buffer) ? static_cast<size_t>(remaining) : sizeof(buffer);
        const ssize_t count = read(fd, buffer, wanted);
        if (count <= 0) {
            *error = "unable to read hash target";
            return false;
        }
        content.append(buffer, static_cast<size_t>(count));
        remaining -= static_cast<unsigned long long>(count);
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        *error = "unable to rewind hash target";
        return false;
    }
    *digest = sha256_hex(content.data(), content.size());
    return true;
}
