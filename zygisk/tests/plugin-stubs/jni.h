#pragma once

using jobject = void *;
constexpr int JNI_OK = 0;
constexpr int JNI_VERSION_1_6 = 0x00010006;
struct JNIEnv {
    void DeleteGlobalRef(jobject) {}
    jobject NewGlobalRef(jobject value) { return value; }
};
struct JavaVM {
    int GetEnv(void **, int) { return -1; }
};
