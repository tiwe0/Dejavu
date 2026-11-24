#pragma once

#include <jni.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dejavu.h"
#include "dejavu_runtime.h"

/*
 * Active Java method invocation and reflection primitives (Lua API):
 *
 *   java.load_dex(path) -> class_loader_handle
 *   java.invoke(class_or_handle, method, jni_signature, ...args)
 *       - a string first argument selects a static method on that class
 *       - "<loader_handle>:<class>" selects a class from a dex class loader
 *       - an integer first argument selects an instance method on a handle
 *   java.new(class_name, constructor_signature, ...args) -> handle
 *       - class_name may also be "<loader_handle>:<class>"
 *   java.release(handle) -> true
 *
 * Internal proxy primitives (wrapped by zygisk/config/resources.lua):
 *   java._invoke_as(handle, class, method, sig, ...args)
 *   java._reflect(class_or_handle) -> JSON metadata
 *   java._class_name(handle) -> runtime class name
 *   java._class_object(class) -> java.lang.Class handle
 *   java._is_instance(handle, class) -> boolean
 *   java._get_field(class_or_handle, name, jni_type[, declaring_class])
 *   java._set_field(class_or_handle, name, jni_type, value[, declaring_class])
 *   java._array_new(elem_jni, len_or_bytes[, loader_handle]) -> handle
 *   java._array_length(handle) -> integer
 *   java._array_get(handle, elem_jni, index) -> value
 *   java._array_set(handle, elem_jni, index, value) -> true
 *   java._array_bytes(handle) -> binary Lua string
 *
 * Argument conversion is driven by the JNI signature: primitives accept the
 * matching Lua boolean/integer/number, String/CharSequence/Object accept a Lua
 * string, nil becomes null, and other references accept an object handle.
 */
class JavaInvoker {
  public:
    JavaInvoker();
    ~JavaInvoker();

    JavaInvoker(const JavaInvoker &) = delete;
    JavaInvoker &operator=(const JavaInvoker &) = delete;

    bool initialize(DejavuRuntime *runtime, JavaVM *vm, jobject app_class_loader,
                    std::string *error);
    bool register_lua(dejavu_control_session *session, std::string *error);
    void shutdown();

  private:
    struct ConvertedArguments {
        std::vector<jvalue> values;
        std::vector<jobject> local_references;
    };

    bool invoke_jni(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                    std::string *error);
    bool invoke_as_jni(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                       std::string *error);
    bool construct_jni(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                       std::string *error);
    bool release_handle(long long handle, std::string *error);
    bool load_dex(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                  std::string *error);
    bool reflect_jni(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                     std::string *error);
    bool class_name_jni(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                        std::string *error);
    bool class_object_jni(const dejavu_value *arguments, size_t argument_count,
                          dejavu_value *result, std::string *error);
    bool is_instance_jni(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                         std::string *error);
    bool get_field_jni(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                       std::string *error);
    bool set_field_jni(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                       std::string *error);
    bool array_new_jni(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                       std::string *error);
    bool array_length_jni(const dejavu_value *arguments, size_t argument_count,
                          dejavu_value *result, std::string *error);
    bool array_get_jni(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                       std::string *error);
    bool array_set_jni(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                       std::string *error);
    bool array_bytes_jni(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                         std::string *error);
    bool resolve_class_target(const std::string &target, jlong *loader_handle,
                              std::string *class_name, std::string *error);
    jclass resolve_class(JNIEnv *env, const std::string &target, std::string *error);
    bool read_class_name(JNIEnv *env, jclass cls, std::string *name, std::string *error);
    bool read_object_class_name(JNIEnv *env, jobject obj, std::string *name, std::string *error);
    bool verify_instance_of(JNIEnv *env, jobject obj, jclass cls, const char *context,
                            std::string *error);

    JNIEnv *current_env();
    jclass find_class(JNIEnv *env, const std::string &name, std::string *error);
    jclass find_class_with_loader(JNIEnv *env, const std::string &name, jobject loader,
                                  std::string *error);
    bool convert_arguments(JNIEnv *env, const std::vector<std::string> &parameters,
                           const dejavu_value *arguments, size_t argument_count,
                           ConvertedArguments *converted, std::string *error,
                           jobject loader = nullptr);
    jstring create_string(JNIEnv *env, const std::string &text, std::string *error);
    bool read_string(JNIEnv *env, jstring value, std::string *text, std::string *error);
    jlong register_object(JNIEnv *env, jobject local, std::string *error);
    jobject lookup_object(JNIEnv *env, long long handle, std::string *error);
    std::string describe_exception(JNIEnv *env);
    bool call_primitive(JNIEnv *env, bool is_static, jclass class_object, jobject receiver,
                        jmethodID method, const std::vector<jvalue> &arguments,
                        const std::string &return_type, jvalue *result, std::string *error);
    bool convert_return(JNIEnv *env, const std::string &return_type, const jvalue &value,
                        dejavu_value *result, std::string *error);
    void cleanup(ConvertedArguments *converted, jclass class_object);

    static int lua_invoke(dejavu_control_session *, void *, const dejavu_value *, size_t,
                          dejavu_value *, char *, size_t);
    static int lua_invoke_as(dejavu_control_session *, void *, const dejavu_value *, size_t,
                             dejavu_value *, char *, size_t);
    static int lua_new(dejavu_control_session *, void *, const dejavu_value *, size_t,
                       dejavu_value *, char *, size_t);
    static int lua_release(dejavu_control_session *, void *, const dejavu_value *, size_t,
                           dejavu_value *, char *, size_t);
    static int lua_load_dex(dejavu_control_session *, void *, const dejavu_value *, size_t,
                            dejavu_value *, char *, size_t);
    static int lua_reflect(dejavu_control_session *, void *, const dejavu_value *, size_t,
                           dejavu_value *, char *, size_t);
    static int lua_class_name(dejavu_control_session *, void *, const dejavu_value *, size_t,
                              dejavu_value *, char *, size_t);
    static int lua_class_object(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                dejavu_value *, char *, size_t);
    static int lua_is_instance(dejavu_control_session *, void *, const dejavu_value *, size_t,
                               dejavu_value *, char *, size_t);
    static int lua_get_field(dejavu_control_session *, void *, const dejavu_value *, size_t,
                             dejavu_value *, char *, size_t);
    static int lua_set_field(dejavu_control_session *, void *, const dejavu_value *, size_t,
                             dejavu_value *, char *, size_t);
    static int lua_array_new(dejavu_control_session *, void *, const dejavu_value *, size_t,
                             dejavu_value *, char *, size_t);
    static int lua_array_length(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                dejavu_value *, char *, size_t);
    static int lua_array_get(dejavu_control_session *, void *, const dejavu_value *, size_t,
                             dejavu_value *, char *, size_t);
    static int lua_array_set(dejavu_control_session *, void *, const dejavu_value *, size_t,
                             dejavu_value *, char *, size_t);
    static int lua_array_bytes(dejavu_control_session *, void *, const dejavu_value *, size_t,
                               dejavu_value *, char *, size_t);

    JavaVM *vm_ = nullptr;
    DejavuRuntime *runtime_ = nullptr;
    jobject class_loader_ = nullptr;
    mutable std::mutex mutex_;
    std::unordered_map<jlong, jobject> objects_;
    jlong next_handle_ = 1;
};
