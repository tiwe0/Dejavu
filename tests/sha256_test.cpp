#include "sha256.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

int failures = 0;

void expect(const std::string &actual, const std::string &expected, const char *label) {
    if (actual == expected) {
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n  expected %s\n  actual   %s\n", label, expected.c_str(),
                 actual.c_str());
    ++failures;
}

} // namespace

int main() {
    expect(sha256_hex("", 0), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "empty");
    expect(sha256_hex("abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "abc");
    const char *nist = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    expect(sha256_hex(nist, std::char_traits<char>::length(nist)),
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "nist");
    const std::string large(1000000, 'a');
    expect(sha256_hex(large.data(), large.size()),
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "million-a");

    const char *path = "/tmp/dejavu-sha256-test.txt";
    {
        std::ofstream file(path, std::ios::binary);
        file << "abc";
    }
    std::string digest;
    std::string error;
    if (!sha256_file_hex(path, &digest, &error)) {
        std::fprintf(stderr, "FAIL: sha256_file_hex: %s\n", error.c_str());
        ++failures;
    } else {
        expect(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "file-abc");
    }
    std::remove(path);
    if (failures != 0) {
        return 1;
    }
    std::printf("OK: SHA-256 known answers\n");
    return 0;
}
