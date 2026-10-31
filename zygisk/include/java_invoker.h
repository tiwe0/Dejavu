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
 * Active Java method invocation (Lua API):
 *
 *   java.invoke(class_or_handle, method, jni_signature, ...args)
 *       - a string first argument selects a static method on that class
 *       - an integer first argument selects an instance method on a handle
 *   java.new(class_name, constructor_signature, ...args) -> handle
 *   java.release(handle) -> true
 *
 * Argument conversion is driven by the JNI signature: primitives accept the
 * matching Lua boolean/integer/number, Ljava/lang/String; accepts a Lua string,
 * nil becomes null, and other references accept an integer object handle
 * returned by java.new()/java.invoke().
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
    bool construct_jni(const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                       std::string *error);
    bool release_handle(long long handle, std::string *error);

    JNIEnv *current_env();
    jclass find_class(JNIEnv *env, const std::string &name, std::string *error);
    bool convert_arguments(JNIEnv *env, const std::vector<std::string> &parameters,
                           const dejavu_value *arguments, size_t argument_count,
                           ConvertedArguments *converted, std::string *error);
    jstring create_string(JNIEnv *env, const std::string &text, std::string *error);
    bool read_string(JNIEnv *env, jstring value, std::string *text, std::string *error);
    jlong register_object(JNIEnv *env, jobject local, std::string *error);
    jobject lookup_object(long long handle, std::string *error);
    std::string describe_exception(JNIEnv *env);
    bool call_primitive(JNIEnv *env, bool is_static, jclass class_object, jobject receiver,
                        jmethodID method, const std::vector<jvalue> &arguments,
                        const std::string &return_type, jvalue *result, std::string *error);
    bool convert_return(JNIEnv *env, const std::string &return_type, const jvalue &value,
                        dejavu_value *result, std::string *error);
    void cleanup(ConvertedArguments *converted, jclass class_object);

    static int lua_invoke(dejavu_control_session *, void *, const dejavu_value *, size_t,
                          dejavu_value *, char *, size_t);
    static int lua_new(dejavu_control_session *, void *, const dejavu_value *, size_t,
                       dejavu_value *, char *, size_t);
    static int lua_release(dejavu_control_session *, void *, const dejavu_value *, size_t,
                           dejavu_value *, char *, size_t);

    JavaVM *vm_ = nullptr;
    DejavuRuntime *runtime_ = nullptr;
    jobject class_loader_ = nullptr;
    mutable std::mutex mutex_;
    std::unordered_map<jlong, jobject> objects_;
    jlong next_handle_ = 1;
};
