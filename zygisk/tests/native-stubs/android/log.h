#pragma once

#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6

extern "C" int __android_log_print(int priority, const char *tag, const char *format, ...);
extern "C" int __android_log_write(int priority, const char *tag, const char *text);
