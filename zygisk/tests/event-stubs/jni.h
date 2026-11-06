#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
using jboolean = unsigned char;
using jbyte = signed char;
using jchar = unsigned short;
using jshort = short;
using jint = int;
using jsize = int;
using jlong = long long;
using jfloat = float;
using jdouble = double;
using jobject = void *;
using jclass = void *;
using jstring = void *;
using jobjectArray = void *;
using jmethodID = void *;
union jvalue {
    jboolean z;
    jbyte b;
    jchar c;
    jshort s;
    jint i;
    jlong j;
    jfloat f;
    jdouble d;
    jobject l;
};
constexpr jboolean JNI_TRUE = 1;
struct JavaVM {};
struct JNIEnv {
    enum class Failure { none, find_class, string_length, string_region };
    std::u16string text;
    std::string modified;
    size_t regions = 0;
    size_t largest_region = 0;
    bool string_value = true;
    Failure failure = Failure::none;
    size_t fail_region = 1;
    bool pending_exception = false;
    size_t cleared_exceptions = 0;
    jclass FindClass(const char *) {
        if (failure == Failure::find_class) {
            pending_exception = true;
            return nullptr;
        }
        return this;
    }
    jboolean IsInstanceOf(jobject, jclass) {
        return string_value;
    }
    void DeleteLocalRef(jobject) {
    }
    void ExceptionClear() {
        pending_exception = false;
        ++cleared_exceptions;
    }
    jboolean ExceptionCheck() {
        return pending_exception;
    }
    jsize GetStringLength(jstring) {
        if (failure == Failure::string_length) {
            pending_exception = true;
            return 0;
        }
        return static_cast<jsize>(text.size());
    }
    void GetStringRegion(jstring, jsize start, jsize count, jchar *out) {
        ++regions;
        if (failure == Failure::string_region && regions == fail_region) {
            pending_exception = true;
            return;
        }
        largest_region = std::max(largest_region, static_cast<size_t>(count));
        std::memcpy(out, text.data() + start, static_cast<size_t>(count) * sizeof(jchar));
    }
    const char *GetStringUTFChars(jstring, void *) {
        return modified.c_str();
    }
    jsize GetStringUTFLength(jstring) {
        return static_cast<jsize>(modified.size());
    }
    void ReleaseStringUTFChars(jstring, const char *) {
    }
};
