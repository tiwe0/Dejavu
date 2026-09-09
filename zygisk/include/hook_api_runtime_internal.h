#pragma once

#include "hook_api_runtime.h"

namespace dejavu_hook_internal {

int mark_context_error(dejavu_hook_context *context, int status);
int check_context(dejavu_hook_context *context);
int check_argument(
    dejavu_hook_context *context,
    unsigned int index,
    char *kind,
    jobject *boxed);

int primitive_index(char kind);
bool is_integral_kind(char kind);
bool is_floating_kind(char kind);
bool integral_in_range(char kind, long long value);
bool is_object_kind(char kind);

jobject box_integral(dejavu_hook_context *context, char kind, long long value);
jobject box_floating(dejavu_hook_context *context, char kind, double value);
int unbox_integral(
    dejavu_hook_context *context,
    char kind,
    jobject boxed,
    long long *value);
int unbox_floating(
    dejavu_hook_context *context,
    char kind,
    jobject boxed,
    double *value);
int replace_result(dejavu_hook_context *context, jobject result);

void clear_local(JNIEnv *env, jobject value);
jobject resolve_class(
    dejavu_hook_context *context,
    const char *class_name,
    bool *failed);
int copy_string_mutf8(
    dejavu_hook_context *context,
    jstring value,
    char *buffer,
    size_t capacity,
    size_t *size);
jstring new_string_mutf8(
    dejavu_hook_context *context,
    const char *data,
    size_t size);
int value_to_jvalue(
    dejavu_hook_context *context,
    char kind,
    const dejavu_hook_value &value,
    jvalue *out,
    jobject *temporary);

}  // namespace dejavu_hook_internal
