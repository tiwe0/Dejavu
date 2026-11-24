#include "java_invoker.h"

#include <android/log.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "cJSON.h"

namespace {

constexpr jlong kMaximumObjectHandles = 8192;
constexpr jint kModifierStatic = 0x0008;
constexpr jint kModifierBridge = 0x0040;
constexpr jint kModifierSynthetic = 0x1000;
constexpr size_t kMaximumArrayLength = 16 * 1024 * 1024;
constexpr size_t kMaximumByteArrayRead = 64 * 1024 * 1024;

template <typename T> class ScopedLocalRef {
  public:
    ScopedLocalRef(JNIEnv *env, T ref = nullptr) : env_(env), ref_(ref) {
    }
    ~ScopedLocalRef() {
        reset();
    }
    ScopedLocalRef(const ScopedLocalRef &) = delete;
    ScopedLocalRef &operator=(const ScopedLocalRef &) = delete;
    void reset(T next = nullptr) {
        if (ref_ != nullptr && env_ != nullptr) {
            env_->DeleteLocalRef(ref_);
        }
        ref_ = next;
    }
    T get() const {
        return ref_;
    }
    T release() {
        T out = ref_;
        ref_ = nullptr;
        return out;
    }
    explicit operator bool() const {
        return ref_ != nullptr;
    }

  private:
    JNIEnv *env_ = nullptr;
    T ref_ = nullptr;
};

std::string descriptor_to_dotted_class(const std::string &descriptor) {
    if (descriptor.size() >= 3 && descriptor.front() == 'L' && descriptor.back() == ';') {
        std::string out = descriptor.substr(1, descriptor.size() - 2);
        for (char &ch : out) {
            if (ch == '/') {
                ch = '.';
            }
        }
        return out;
    }
    if (!descriptor.empty() && descriptor.front() == '[') {
        std::string out = descriptor;
        for (char &ch : out) {
            if (ch == '/') {
                ch = '.';
            }
        }
        return out;
    }
    return {};
}

void copy_error(char *error, size_t error_size, const std::string &message) {
    if (error != nullptr && error_size != 0) {
        snprintf(error, error_size, "%s", message.c_str());
    }
}

std::string value_string(const dejavu_value &value) {
    return value.string_value == nullptr ? std::string()
                                         : std::string(value.string_value, value.string_size);
}

bool set_owned_string(dejavu_value *value, const std::string &text) {
    char *copy = static_cast<char *>(malloc(text.size() + 1));
    if (copy == nullptr) {
        return false;
    }
    memcpy(copy, text.data(), text.size());
    copy[text.size()] = '\0';
    value->type = DEJAVU_VALUE_STRING;
    value->owned = 1;
    value->string_value = copy;
    value->string_size = text.size();
    return true;
}

bool is_string_descriptor(const std::string &descriptor) {
    return descriptor == "Ljava/lang/String;";
}

bool is_string_accepting_descriptor(const std::string &descriptor) {
    return descriptor == "Ljava/lang/String;" || descriptor == "Ljava/lang/CharSequence;" ||
           descriptor == "Ljava/lang/Object;";
}

std::string java_type_name_to_jni(const std::string &name) {
    if (name == "void") {
        return "V";
    }
    if (name == "boolean") {
        return "Z";
    }
    if (name == "byte") {
        return "B";
    }
    if (name == "char") {
        return "C";
    }
    if (name == "short") {
        return "S";
    }
    if (name == "int") {
        return "I";
    }
    if (name == "long") {
        return "J";
    }
    if (name == "float") {
        return "F";
    }
    if (name == "double") {
        return "D";
    }
    std::string internal = name;
    for (char &ch : internal) {
        if (ch == '.') {
            ch = '/';
        }
    }
    if (!internal.empty() && internal[0] == '[') {
        return internal;
    }
    return "L" + internal + ";";
}

bool is_reference_descriptor(const std::string &descriptor) {
    return !descriptor.empty() && (descriptor[0] == 'L' || descriptor[0] == '[');
}

bool integer_in_range(long long value, long long minimum, long long maximum) {
    return value >= minimum && value <= maximum;
}

bool parse_type_descriptor(const std::string &signature, size_t *offset, bool allow_void,
                           std::string *descriptor, std::string *error) {
    std::string text;
    while (*offset < signature.size() && signature[*offset] == '[') {
        text.push_back('[');
        if (text.size() > 255) {
            *error = "JNI signature has too many array dimensions";
            return false;
        }
        ++*offset;
    }
    if (*offset >= signature.size()) {
        *error = "JNI signature ended before a type descriptor";
        return false;
    }
    const char kind = signature[*offset];
    if (kind == 'V') {
        if (!allow_void || !text.empty()) {
            *error = "void is not valid in this JNI signature position";
            return false;
        }
        ++*offset;
        *descriptor = "V";
        return true;
    }
    if (strchr("ZBCSIJFD", kind) != nullptr) {
        text.push_back(kind);
        ++*offset;
        *descriptor = std::move(text);
        return true;
    }
    if (kind == 'L') {
        const size_t end = signature.find(';', *offset);
        if (end == std::string::npos || end == *offset + 1) {
            *error = "unterminated object descriptor in JNI signature";
            return false;
        }
        text.append(signature, *offset, end - *offset + 1);
        *offset = end + 1;
        *descriptor = std::move(text);
        return true;
    }
    *error = "unsupported JNI signature descriptor";
    return false;
}

bool parse_jni_signature(const std::string &signature, std::vector<std::string> *parameters,
                         std::string *return_type, std::string *error) {
    if (signature.empty() || signature[0] != '(') {
        *error = "JNI signature must start with '('";
        return false;
    }
    size_t offset = 1;
    parameters->clear();
    while (offset < signature.size() && signature[offset] != ')') {
        std::string descriptor;
        if (!parse_type_descriptor(signature, &offset, false, &descriptor, error)) {
            return false;
        }
        parameters->push_back(std::move(descriptor));
    }
    if (offset >= signature.size() || signature[offset] != ')') {
        *error = "JNI signature is missing ')'";
        return false;
    }
    ++offset;
    if (!parse_type_descriptor(signature, &offset, true, return_type, error)) {
        return false;
    }
    if (offset != signature.size()) {
        *error = "JNI signature has trailing characters";
        return false;
    }
    return true;
}

bool utf8_to_utf16(const std::string &input, std::vector<jchar> *output, std::string *error) {
    output->clear();
    output->reserve(input.size());
    size_t offset = 0;
    while (offset < input.size()) {
        const unsigned char first = static_cast<unsigned char>(input[offset]);
        uint32_t codepoint = 0;
        size_t length = 0;
        if (first < 0x80) {
            codepoint = first;
            length = 1;
        } else if ((first & 0xE0) == 0xC0) {
            codepoint = first & 0x1F;
            length = 2;
        } else if ((first & 0xF0) == 0xE0) {
            codepoint = first & 0x0F;
            length = 3;
        } else if ((first & 0xF8) == 0xF0) {
            codepoint = first & 0x07;
            length = 4;
        } else {
            *error = "invalid UTF-8 lead byte";
            return false;
        }
        if (offset + length > input.size()) {
            *error = "truncated UTF-8 sequence";
            return false;
        }
        for (size_t index = 1; index < length; ++index) {
            const unsigned char continuation = static_cast<unsigned char>(input[offset + index]);
            if ((continuation & 0xC0) != 0x80) {
                *error = "invalid UTF-8 continuation byte";
                return false;
            }
            codepoint = (codepoint << 6) | (continuation & 0x3F);
        }
        static const uint32_t kMinimum[] = {0, 0, 0x80, 0x800, 0x10000};
        if (codepoint < kMinimum[length]) {
            *error = "overlong UTF-8 sequence";
            return false;
        }
        if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            *error = "invalid UTF-8 code point";
            return false;
        }
        if (codepoint >= 0x10000) {
            codepoint -= 0x10000;
            output->push_back(static_cast<jchar>(0xD800 + (codepoint >> 10)));
            output->push_back(static_cast<jchar>(0xDC00 + (codepoint & 0x3FF)));
        } else {
            output->push_back(static_cast<jchar>(codepoint));
        }
        offset += length;
    }
    return true;
}

bool utf16_to_utf8(const jchar *input, size_t length, std::string *output, std::string *error) {
    output->clear();
    output->reserve(length);
    for (size_t index = 0; index < length; ++index) {
        uint32_t codepoint = input[index];
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            if (index + 1 >= length || input[index + 1] < 0xDC00 || input[index + 1] > 0xDFFF) {
                *error = "invalid UTF-16 surrogate pair";
                return false;
            }
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (input[++index] - 0xDC00);
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            *error = "unexpected UTF-16 low surrogate";
            return false;
        }
        if (codepoint < 0x80) {
            output->push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            output->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint < 0x10000) {
            output->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            output->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }
    return true;
}

} // namespace

JavaInvoker::JavaInvoker() = default;

JavaInvoker::~JavaInvoker() {
    shutdown();
}

bool JavaInvoker::initialize(DejavuRuntime *runtime, JavaVM *vm, jobject app_class_loader,
                             std::string *error) {
    if (runtime == nullptr || vm == nullptr || app_class_loader == nullptr) {
        *error = "invalid Java invoker initialization argument";
        return false;
    }
    runtime_ = runtime;
    vm_ = vm;
    class_loader_ = app_class_loader;
    return true;
}

bool JavaInvoker::register_lua(dejavu_control_session *session, std::string *error) {
    if (runtime_ == nullptr || session == nullptr) {
        *error = "Java invoker is not initialized";
        return false;
    }
    struct Registration {
        const char *name;
        dejavu_host_function function;
    };
    const Registration registrations[] = {
        {"invoke", JavaInvoker::lua_invoke},
        {"_invoke_as", JavaInvoker::lua_invoke_as},
        {"new", JavaInvoker::lua_new},
        {"release", JavaInvoker::lua_release},
        {"load_dex", JavaInvoker::lua_load_dex},
        {"_reflect", JavaInvoker::lua_reflect},
        {"_class_name", JavaInvoker::lua_class_name},
        {"_class_object", JavaInvoker::lua_class_object},
        {"_is_instance", JavaInvoker::lua_is_instance},
        {"_get_field", JavaInvoker::lua_get_field},
        {"_set_field", JavaInvoker::lua_set_field},
        {"_array_new", JavaInvoker::lua_array_new},
        {"_array_length", JavaInvoker::lua_array_length},
        {"_array_get", JavaInvoker::lua_array_get},
        {"_array_set", JavaInvoker::lua_array_set},
        {"_array_bytes", JavaInvoker::lua_array_bytes},
    };
    char runtime_error[512];
    for (const Registration &registration : registrations) {
        const int status = runtime_->control_session_register(session, "java", registration.name,
                                                              registration.function, this,
                                                              runtime_error, sizeof(runtime_error));
        if (status != DEJAVU_OK) {
            *error = runtime_error;
            return false;
        }
    }
    return true;
}

void JavaInvoker::shutdown() {
    if (vm_ != nullptr) {
        JNIEnv *env = current_env();
        if (env != nullptr) {
            std::unordered_map<jlong, jobject> objects;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                objects.swap(objects_);
            }
            for (const auto &entry : objects) {
                env->DeleteGlobalRef(entry.second);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        objects_.clear();
    }
    vm_ = nullptr;
    runtime_ = nullptr;
    class_loader_ = nullptr;
}

JNIEnv *JavaInvoker::current_env() {
    if (vm_ == nullptr) {
        return nullptr;
    }
    JNIEnv *env = nullptr;
    const jint status = vm_->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (status == JNI_OK) {
        return env;
    }
    if (status == JNI_EDETACHED && vm_->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        return env;
    }
    return nullptr;
}

jclass JavaInvoker::find_class(JNIEnv *env, const std::string &name, std::string *error) {
    return find_class_with_loader(env, name, class_loader_, error);
}

jclass JavaInvoker::find_class_with_loader(JNIEnv *env, const std::string &name, jobject loader,
                                           std::string *error) {
    std::string dotted = name;
    for (char &character : dotted) {
        if (character == '/') {
            character = '.';
        }
    }
    if (dotted.empty()) {
        *error = "class name must not be empty";
        return nullptr;
    }
    if (loader == nullptr) {
        std::string path = dotted;
        for (char &character : path) {
            if (character == '.') {
                character = '/';
            }
        }
        jclass result = env->FindClass(path.c_str());
        if (env->ExceptionCheck() || result == nullptr) {
            env->ExceptionClear();
            if (result != nullptr) {
                env->DeleteLocalRef(result);
            }
            *error = "class not found: " + name;
            return nullptr;
        }
        return result;
    }
    if (loader != class_loader_) {
        ScopedLocalRef<jclass> cl_base(env, env->FindClass("java/lang/ClassLoader"));
        if (!cl_base || env->ExceptionCheck()) {
            env->ExceptionClear();
            *error = "java.lang.ClassLoader is unavailable";
            return nullptr;
        }
        const jboolean is_cl = env->IsInstanceOf(loader, cl_base.get());
        if (env->ExceptionCheck() || is_cl != JNI_TRUE) {
            env->ExceptionClear();
            *error = "loader handle is not a java.lang.ClassLoader";
            return nullptr;
        }
    }
    // Array descriptors ("[B", "[[I", "[Ljava.lang.String;") require Class.forName(name, false,
    // loader) because ClassLoader.loadClass rejects array binary names on ART/JVM.
    if (dotted[0] == '[') {
        ScopedLocalRef<jclass> cls_class(env, env->FindClass("java/lang/Class"));
        if (!cls_class || env->ExceptionCheck()) {
            env->ExceptionClear();
            *error = "java.lang.Class is unavailable";
            return nullptr;
        }
        const jmethodID for_name =
            env->GetStaticMethodID(cls_class.get(), "forName",
                                   "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
        if (for_name == nullptr || env->ExceptionCheck()) {
            env->ExceptionClear();
            *error = "unable to resolve Class.forName";
            return nullptr;
        }
        ScopedLocalRef<jstring> class_name(env, create_string(env, dotted, error));
        if (!class_name) {
            return nullptr;
        }
        jobject loaded = env->CallStaticObjectMethod(cls_class.get(), for_name, class_name.get(),
                                                     JNI_FALSE, loader);
        if (env->ExceptionCheck()) {
            *error = describe_exception(env);
            if (loaded != nullptr) {
                env->DeleteLocalRef(loaded);
            }
            return nullptr;
        }
        if (loaded == nullptr) {
            *error = "class not found: " + name;
            return nullptr;
        }
        return static_cast<jclass>(loaded);
    }
    ScopedLocalRef<jclass> class_loader_class(env, env->GetObjectClass(loader));
    if (!class_loader_class || env->ExceptionCheck()) {
        env->ExceptionClear();
        *error = "unable to inspect the dex ClassLoader";
        return nullptr;
    }
    const jmethodID load = env->GetMethodID(class_loader_class.get(), "loadClass",
                                            "(Ljava/lang/String;)Ljava/lang/Class;");
    if (load == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        *error = "unable to resolve ClassLoader.loadClass";
        return nullptr;
    }
    ScopedLocalRef<jstring> class_name(env, create_string(env, dotted, error));
    if (!class_name) {
        return nullptr;
    }
    jobject loaded = env->CallObjectMethod(loader, load, class_name.get());
    if (env->ExceptionCheck()) {
        *error = describe_exception(env);
        if (loaded != nullptr) {
            env->DeleteLocalRef(loaded);
        }
        return nullptr;
    }
    if (loaded == nullptr) {
        *error = "class not found: " + name;
        return nullptr;
    }
    return static_cast<jclass>(loaded);
}

jstring JavaInvoker::create_string(JNIEnv *env, const std::string &text, std::string *error) {
    std::vector<jchar> units;
    if (!utf8_to_utf16(text, &units, error)) {
        return nullptr;
    }
    jstring result = env->NewString(units.data(), static_cast<jsize>(units.size()));
    if (result == nullptr) {
        *error = "unable to allocate a Java string";
        env->ExceptionClear();
        return nullptr;
    }
    return result;
}

bool JavaInvoker::read_string(JNIEnv *env, jstring value, std::string *text, std::string *error) {
    const jsize length = env->GetStringLength(value);
    const jchar *characters = env->GetStringChars(value, nullptr);
    if (characters == nullptr) {
        env->ExceptionClear();
        *error = "unable to read a Java string";
        return false;
    }
    const bool converted = utf16_to_utf8(characters, static_cast<size_t>(length), text, error);
    env->ReleaseStringChars(value, characters);
    return converted;
}

jlong JavaInvoker::register_object(JNIEnv *env, jobject local, std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (objects_.size() >= static_cast<size_t>(kMaximumObjectHandles)) {
        *error = "too many live Java object handles; call java.release";
        return 0;
    }
    jobject global = env->NewGlobalRef(local);
    if (global == nullptr) {
        env->ExceptionClear();
        *error = "unable to retain a Java object handle";
        return 0;
    }
    jlong handle = next_handle_++;
    if (next_handle_ <= 0) {
        next_handle_ = 1;
    }
    objects_.emplace(handle, global);
    return handle;
}

jobject JavaInvoker::lookup_object(JNIEnv *env, long long handle, std::string *error) {
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = objects_.find(static_cast<jlong>(handle));
    if (iterator == objects_.end()) {
        *error = "unknown Java object handle: " + std::to_string(handle);
        return nullptr;
    }
    jobject local = env->NewLocalRef(iterator->second);
    if (local == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (local != nullptr) {
            env->DeleteLocalRef(local);
        }
        *error = "unable to retain local reference for Java object handle";
        return nullptr;
    }
    return local;
}

bool JavaInvoker::release_handle(long long handle, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    jobject object = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = objects_.find(static_cast<jlong>(handle));
        if (iterator == objects_.end()) {
            *error = "unknown Java object handle: " + std::to_string(handle);
            return false;
        }
        object = iterator->second;
        objects_.erase(iterator);
    }
    env->DeleteGlobalRef(object);
    return true;
}

std::string JavaInvoker::describe_exception(JNIEnv *env) {
    jthrowable exception = env->ExceptionOccurred();
    if (exception == nullptr) {
        return "Java exception";
    }
    env->ExceptionClear();
    std::string text = "java exception";
    jclass throwable_class = env->FindClass("java/lang/Throwable");
    if (throwable_class != nullptr) {
        const jmethodID to_string =
            env->GetMethodID(throwable_class, "toString", "()Ljava/lang/String;");
        if (to_string != nullptr) {
            jobject message = env->CallObjectMethod(exception, to_string);
            if (!env->ExceptionCheck() && message != nullptr) {
                std::string detail;
                std::string ignored;
                if (read_string(env, static_cast<jstring>(message), &detail, &ignored)) {
                    text = "java exception: " + detail;
                }
                env->DeleteLocalRef(message);
            }
            env->ExceptionClear();
        }
        env->DeleteLocalRef(throwable_class);
    }
    env->DeleteLocalRef(exception);
    return text;
}

bool JavaInvoker::convert_arguments(JNIEnv *env, const std::vector<std::string> &parameters,
                                    const dejavu_value *arguments, size_t argument_count,
                                    ConvertedArguments *converted, std::string *error,
                                    jobject loader) {
    if (argument_count != parameters.size()) {
        *error = "expected " + std::to_string(parameters.size()) + " argument(s), got " +
                 std::to_string(argument_count);
        return false;
    }
    jobject active_loader = loader != nullptr ? loader : class_loader_;
    converted->values.assign(parameters.size(), jvalue{});
    for (size_t index = 0; index < parameters.size(); ++index) {
        const std::string &descriptor = parameters[index];
        const dejavu_value &argument = arguments[index];
        const std::string prefix = "argument " + std::to_string(index) + " ";
        jvalue &value = converted->values[index];
        if (descriptor == "Z") {
            if (argument.type != DEJAVU_VALUE_BOOLEAN) {
                *error = prefix + "expects a boolean";
                return false;
            }
            value.z = argument.boolean_value != 0 ? JNI_TRUE : JNI_FALSE;
        } else if (descriptor == "B") {
            if (argument.type != DEJAVU_VALUE_INTEGER ||
                !integer_in_range(argument.integer_value, -128, 127)) {
                *error = prefix + "expects a byte (-128..127)";
                return false;
            }
            value.b = static_cast<jbyte>(argument.integer_value);
        } else if (descriptor == "C") {
            if (argument.type != DEJAVU_VALUE_INTEGER ||
                !integer_in_range(argument.integer_value, 0, 65535)) {
                *error = prefix + "expects a char (0..65535)";
                return false;
            }
            value.c = static_cast<jchar>(argument.integer_value);
        } else if (descriptor == "S") {
            if (argument.type != DEJAVU_VALUE_INTEGER ||
                !integer_in_range(argument.integer_value, -32768, 32767)) {
                *error = prefix + "expects a short (-32768..32767)";
                return false;
            }
            value.s = static_cast<jshort>(argument.integer_value);
        } else if (descriptor == "I") {
            if (argument.type != DEJAVU_VALUE_INTEGER ||
                !integer_in_range(argument.integer_value, INT32_MIN, INT32_MAX)) {
                *error = prefix + "expects an int";
                return false;
            }
            value.i = static_cast<jint>(argument.integer_value);
        } else if (descriptor == "J") {
            if (argument.type != DEJAVU_VALUE_INTEGER) {
                *error = prefix + "expects an integer for a long";
                return false;
            }
            value.j = static_cast<jlong>(argument.integer_value);
        } else if (descriptor == "F") {
            if (argument.type == DEJAVU_VALUE_NUMBER) {
                value.f = static_cast<jfloat>(argument.number_value);
            } else if (argument.type == DEJAVU_VALUE_INTEGER) {
                value.f = static_cast<jfloat>(argument.integer_value);
            } else {
                *error = prefix + "expects a number";
                return false;
            }
        } else if (descriptor == "D") {
            if (argument.type == DEJAVU_VALUE_NUMBER) {
                value.d = static_cast<jdouble>(argument.number_value);
            } else if (argument.type == DEJAVU_VALUE_INTEGER) {
                value.d = static_cast<jdouble>(argument.integer_value);
            } else {
                *error = prefix + "expects a number";
                return false;
            }
        } else if (is_reference_descriptor(descriptor)) {
            if (argument.type == DEJAVU_VALUE_NIL) {
                value.l = nullptr;
            } else if (argument.type == DEJAVU_VALUE_STRING &&
                       is_string_accepting_descriptor(descriptor)) {
                jstring text = create_string(env, value_string(argument), error);
                if (text == nullptr) {
                    return false;
                }
                value.l = text;
                converted->local_references.push_back(text);
            } else if (argument.type == DEJAVU_VALUE_INTEGER) {
                jobject object = lookup_object(env, argument.integer_value, error);
                if (object == nullptr) {
                    return false;
                }
                converted->local_references.push_back(object);
                if (descriptor != "Ljava/lang/Object;") {
                    const std::string expected_cls_name = descriptor_to_dotted_class(descriptor);
                    if (!expected_cls_name.empty()) {
                        std::string ignored;
                        ScopedLocalRef<jclass> expected_cls(
                            env, find_class_with_loader(env, expected_cls_name, active_loader,
                                                        &ignored));
                        if (!expected_cls && active_loader != class_loader_) {
                            expected_cls.reset(find_class_with_loader(env, expected_cls_name,
                                                                      class_loader_, &ignored));
                        }
                        if (expected_cls) {
                            const jboolean ok = env->IsInstanceOf(object, expected_cls.get());
                            if (env->ExceptionCheck() || ok != JNI_TRUE) {
                                env->ExceptionClear();
                                *error = prefix + "is not an instance of " + expected_cls_name;
                                return false;
                            }
                        }
                    }
                }
                value.l = object;
            } else {
                *error = is_string_accepting_descriptor(descriptor)
                             ? (prefix + "expects nil, an object handle, or a string")
                             : (prefix + "expects nil or an object handle");
                return false;
            }
        } else {
            *error = prefix + "uses an unsupported JNI descriptor: " + descriptor;
            return false;
        }
    }
    return true;
}

bool JavaInvoker::call_primitive(JNIEnv *env, bool is_static, jclass class_object, jobject receiver,
                                 jmethodID method, const std::vector<jvalue> &arguments,
                                 const std::string &return_type, jvalue *result,
                                 std::string *error) {
    const jvalue *values = arguments.data();
    if (is_static) {
        switch (return_type[0]) {
        case 'V':
            env->CallStaticVoidMethodA(class_object, method, values);
            break;
        case 'Z':
            result->z = env->CallStaticBooleanMethodA(class_object, method, values);
            break;
        case 'B':
            result->b = env->CallStaticByteMethodA(class_object, method, values);
            break;
        case 'C':
            result->c = env->CallStaticCharMethodA(class_object, method, values);
            break;
        case 'S':
            result->s = env->CallStaticShortMethodA(class_object, method, values);
            break;
        case 'I':
            result->i = env->CallStaticIntMethodA(class_object, method, values);
            break;
        case 'J':
            result->j = env->CallStaticLongMethodA(class_object, method, values);
            break;
        case 'F':
            result->f = env->CallStaticFloatMethodA(class_object, method, values);
            break;
        case 'D':
            result->d = env->CallStaticDoubleMethodA(class_object, method, values);
            break;
        case 'L':
        case '[':
            result->l = env->CallStaticObjectMethodA(class_object, method, values);
            break;
        default:
            *error = "unsupported Java return descriptor: " + return_type;
            return false;
        }
    } else {
        switch (return_type[0]) {
        case 'V':
            env->CallVoidMethodA(receiver, method, values);
            break;
        case 'Z':
            result->z = env->CallBooleanMethodA(receiver, method, values);
            break;
        case 'B':
            result->b = env->CallByteMethodA(receiver, method, values);
            break;
        case 'C':
            result->c = env->CallCharMethodA(receiver, method, values);
            break;
        case 'S':
            result->s = env->CallShortMethodA(receiver, method, values);
            break;
        case 'I':
            result->i = env->CallIntMethodA(receiver, method, values);
            break;
        case 'J':
            result->j = env->CallLongMethodA(receiver, method, values);
            break;
        case 'F':
            result->f = env->CallFloatMethodA(receiver, method, values);
            break;
        case 'D':
            result->d = env->CallDoubleMethodA(receiver, method, values);
            break;
        case 'L':
        case '[':
            result->l = env->CallObjectMethodA(receiver, method, values);
            break;
        default:
            *error = "unsupported Java return descriptor: " + return_type;
            return false;
        }
    }
    if (env->ExceptionCheck()) {
        *error = describe_exception(env);
        return false;
    }
    return true;
}

bool JavaInvoker::convert_return(JNIEnv *env, const std::string &return_type, const jvalue &value,
                                 dejavu_value *result, std::string *error) {
    if (return_type == "V") {
        result->type = DEJAVU_VALUE_NIL;
        return true;
    }
    if (return_type == "Z") {
        result->type = DEJAVU_VALUE_BOOLEAN;
        result->boolean_value = value.z != 0 ? 1 : 0;
        return true;
    }
    if (return_type == "B" || return_type == "C" || return_type == "S" || return_type == "I") {
        result->type = DEJAVU_VALUE_INTEGER;
        result->integer_value = return_type == "B"   ? static_cast<long long>(value.b)
                                : return_type == "C" ? static_cast<long long>(value.c)
                                : return_type == "S" ? static_cast<long long>(value.s)
                                                     : static_cast<long long>(value.i);
        return true;
    }
    if (return_type == "J") {
        result->type = DEJAVU_VALUE_INTEGER;
        result->integer_value = static_cast<long long>(value.j);
        return true;
    }
    if (return_type == "F") {
        result->type = DEJAVU_VALUE_NUMBER;
        result->number_value = static_cast<double>(value.f);
        return true;
    }
    if (return_type == "D") {
        result->type = DEJAVU_VALUE_NUMBER;
        result->number_value = value.d;
        return true;
    }
    if (is_string_descriptor(return_type)) {
        if (value.l == nullptr) {
            result->type = DEJAVU_VALUE_NIL;
            return true;
        }
        ScopedLocalRef<jobject> local_value(env, value.l);
        ScopedLocalRef<jclass> string_cls(env, env->FindClass("java/lang/String"));
        if (!string_cls || env->ExceptionCheck() ||
            env->IsInstanceOf(local_value.get(), string_cls.get()) != JNI_TRUE ||
            env->ExceptionCheck()) {
            env->ExceptionClear();
            *error = "returned value is not a java.lang.String";
            return false;
        }
        std::string text;
        if (!read_string(env, static_cast<jstring>(local_value.get()), &text, error)) {
            return false;
        }
        if (!set_owned_string(result, text)) {
            *error = "unable to allocate the returned Java string";
            return false;
        }
        return true;
    }
    if (is_reference_descriptor(return_type)) {
        if (value.l == nullptr) {
            result->type = DEJAVU_VALUE_NIL;
            return true;
        }
        const jlong handle = register_object(env, value.l, error);
        env->DeleteLocalRef(value.l);
        if (handle == 0) {
            return false;
        }
        result->type = DEJAVU_VALUE_INTEGER;
        result->integer_value = static_cast<long long>(handle);
        return true;
    }
    *error = "unsupported Java return descriptor: " + return_type;
    return false;
}

void JavaInvoker::cleanup(ConvertedArguments *converted, jclass class_object) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        return;
    }
    for (const jobject reference : converted->local_references) {
        if (reference != nullptr) {
            env->DeleteLocalRef(reference);
        }
    }
    converted->local_references.clear();
    if (class_object != nullptr) {
        env->DeleteLocalRef(class_object);
    }
}

bool JavaInvoker::resolve_class_target(const std::string &target, jlong *loader_handle,
                                       std::string *class_name, std::string *error) {
    *loader_handle = 0;
    const size_t colon = target.find(':');
    if (colon == std::string::npos) {
        *class_name = target;
        if (class_name->empty()) {
            *error = "class name must not be empty";
            return false;
        }
        return true;
    }
    const std::string prefix = target.substr(0, colon);
    if (prefix.empty() || prefix.find_first_not_of("0123456789") != std::string::npos) {
        *error = "class loader target must be <positive_handle>:<class_name>";
        return false;
    }
    *class_name = target.substr(colon + 1);
    if (class_name->empty()) {
        *error = "class name is missing after the class loader handle";
        return false;
    }
    try {
        *loader_handle = std::stoll(prefix);
    } catch (const std::exception &) {
        *error = "invalid class loader handle: " + prefix;
        return false;
    }
    if (*loader_handle <= 0) {
        *error = "class loader handle must be positive";
        return false;
    }
    return true;
}

bool JavaInvoker::load_dex(const dejavu_value *arguments, size_t argument_count,
                           dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_STRING ||
        arguments[0].string_value == nullptr || arguments[0].string_size == 0) {
        *error = "java.load_dex expects a device path to a .dex file";
        return false;
    }
    const std::string path = value_string(arguments[0]);
    constexpr size_t kMaximumDexSize = 64 * 1024 * 1024;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        *error = "unable to open dex: " + path;
        return false;
    }
    const std::streamoff file_size = file.tellg();
    if (file_size <= 0 || static_cast<unsigned long long>(file_size) > kMaximumDexSize) {
        *error = "dex size is invalid or exceeds 64 MiB";
        return false;
    }
    std::string bytes(static_cast<size_t>(file_size), '\0');
    file.seekg(0, std::ios::beg);
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        *error = "unable to read dex: " + path;
        return false;
    }

    jbyteArray array = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (array == nullptr) {
        env->ExceptionClear();
        *error = "unable to allocate the dex byte array";
        return false;
    }
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()),
                            reinterpret_cast<const jbyte *>(bytes.data()));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(array);
        *error = "unable to copy dex bytes into the Java heap";
        return false;
    }
    jclass byte_buffer_class = env->FindClass("java/nio/ByteBuffer");
    jmethodID wrap = nullptr;
    jobject buffer = nullptr;
    jclass loader_class = nullptr;
    jmethodID constructor = nullptr;
    jobject loader = nullptr;
    do {
        if (byte_buffer_class == nullptr || env->ExceptionCheck()) {
            env->ExceptionClear();
            *error = "java.nio.ByteBuffer is unavailable";
            break;
        }
        wrap = env->GetStaticMethodID(byte_buffer_class, "wrap", "([B)Ljava/nio/ByteBuffer;");
        if (wrap == nullptr || env->ExceptionCheck()) {
            env->ExceptionClear();
            *error = "unable to resolve ByteBuffer.wrap";
            break;
        }
        buffer = env->CallStaticObjectMethod(byte_buffer_class, wrap, array);
        if (env->ExceptionCheck() || buffer == nullptr) {
            env->ExceptionClear();
            *error = "unable to wrap dex bytes in a ByteBuffer";
            break;
        }
        loader_class = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        if (loader_class == nullptr || env->ExceptionCheck()) {
            env->ExceptionClear();
            *error = "InMemoryDexClassLoader is unavailable";
            break;
        }
        constructor = env->GetMethodID(loader_class, "<init>",
                                       "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        if (constructor == nullptr || env->ExceptionCheck()) {
            env->ExceptionClear();
            *error = "unable to resolve InMemoryDexClassLoader constructor";
            break;
        }
        loader = env->NewObject(loader_class, constructor, buffer, class_loader_);
        if (env->ExceptionCheck() || loader == nullptr) {
            if (env->ExceptionCheck()) {
                *error = describe_exception(env);
            } else {
                *error = "unable to create InMemoryDexClassLoader";
            }
            break;
        }
    } while (false);
    if (byte_buffer_class != nullptr) {
        env->DeleteLocalRef(byte_buffer_class);
    }
    if (loader_class != nullptr) {
        env->DeleteLocalRef(loader_class);
    }
    if (buffer != nullptr) {
        env->DeleteLocalRef(buffer);
    }
    env->DeleteLocalRef(array);
    if (loader == nullptr) {
        return false;
    }
    const jlong handle = register_object(env, loader, error);
    env->DeleteLocalRef(loader);
    if (handle == 0) {
        return false;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(handle);
    return true;
}

bool JavaInvoker::invoke_jni(const dejavu_value *arguments, size_t argument_count,
                             dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if (argument_count < 3 || arguments[0].type == DEJAVU_VALUE_NIL ||
        arguments[1].type != DEJAVU_VALUE_STRING || arguments[2].type != DEJAVU_VALUE_STRING) {
        *error = "java.invoke expects class-or-handle, method, JNI signature and arguments";
        return false;
    }
    const bool is_static = arguments[0].type == DEJAVU_VALUE_STRING;
    if (!is_static && arguments[0].type != DEJAVU_VALUE_INTEGER) {
        *error = "java.invoke expects a class string or an object handle";
        return false;
    }
    const std::string method_name = value_string(arguments[1]);
    const std::string signature = value_string(arguments[2]);
    std::vector<std::string> parameters;
    std::string return_type;
    if (!parse_jni_signature(signature, &parameters, &return_type, error)) {
        return false;
    }
    ScopedLocalRef<jclass> class_object(env);
    ScopedLocalRef<jobject> receiver(env);
    ScopedLocalRef<jobject> loader_ref(env);
    if (is_static) {
        jlong loader_handle = 0;
        std::string class_name;
        if (!resolve_class_target(value_string(arguments[0]), &loader_handle, &class_name, error)) {
            return false;
        }
        if (loader_handle != 0) {
            loader_ref.reset(lookup_object(env, loader_handle, error));
            if (!loader_ref) {
                return false;
            }
            class_object.reset(find_class_with_loader(env, class_name, loader_ref.get(), error));
        } else {
            class_object.reset(find_class(env, class_name, error));
        }
        if (!class_object) {
            return false;
        }
    } else {
        receiver.reset(lookup_object(env, arguments[0].integer_value, error));
        if (!receiver) {
            return false;
        }
        class_object.reset(env->GetObjectClass(receiver.get()));
        if (!class_object || env->ExceptionCheck()) {
            *error = "unable to resolve the object class";
            env->ExceptionClear();
            return false;
        }
    }
    const jmethodID method =
        is_static
            ? env->GetStaticMethodID(class_object.get(), method_name.c_str(), signature.c_str())
            : env->GetMethodID(class_object.get(), method_name.c_str(), signature.c_str());
    if (method == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        *error = "method not found or signature mismatch: " + method_name + signature;
        return false;
    }
    ConvertedArguments converted;
    if (!convert_arguments(env, parameters, arguments + 3, argument_count - 3, &converted, error,
                           loader_ref.get())) {
        cleanup(&converted, nullptr);
        return false;
    }
    jvalue value = {};
    const bool called = call_primitive(env, is_static, class_object.get(), receiver.get(), method,
                                       converted.values, return_type, &value, error);
    if (!called) {
        cleanup(&converted, nullptr);
        return false;
    }
    const bool converted_result = convert_return(env, return_type, value, result, error);
    cleanup(&converted, nullptr);
    return converted_result;
}

bool JavaInvoker::construct_jni(const dejavu_value *arguments, size_t argument_count,
                                dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if (argument_count < 2 || arguments[0].type != DEJAVU_VALUE_STRING ||
        arguments[1].type != DEJAVU_VALUE_STRING) {
        *error = "java.new expects a class name, constructor signature and arguments";
        return false;
    }
    const std::string target = value_string(arguments[0]);
    const std::string signature = value_string(arguments[1]);
    std::vector<std::string> parameters;
    std::string return_type;
    if (!parse_jni_signature(signature, &parameters, &return_type, error)) {
        return false;
    }
    if (return_type != "V") {
        *error = "java.new constructor signature must return void";
        return false;
    }
    jlong loader_handle = 0;
    std::string class_name;
    if (!resolve_class_target(target, &loader_handle, &class_name, error)) {
        return false;
    }
    ScopedLocalRef<jobject> loader_ref(env);
    ScopedLocalRef<jclass> class_object(env);
    if (loader_handle != 0) {
        loader_ref.reset(lookup_object(env, loader_handle, error));
        if (!loader_ref) {
            return false;
        }
        class_object.reset(find_class_with_loader(env, class_name, loader_ref.get(), error));
    } else {
        class_object.reset(find_class(env, class_name, error));
    }
    if (!class_object) {
        return false;
    }
    const jmethodID constructor = env->GetMethodID(class_object.get(), "<init>", signature.c_str());
    if (constructor == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        *error = "constructor not found or signature mismatch: " + class_name + signature;
        return false;
    }
    ConvertedArguments converted;
    if (!convert_arguments(env, parameters, arguments + 2, argument_count - 2, &converted, error,
                           loader_ref.get())) {
        cleanup(&converted, nullptr);
        return false;
    }
    ScopedLocalRef<jobject> instance(
        env, env->NewObjectA(class_object.get(), constructor, converted.values.data()));
    if (env->ExceptionCheck() || !instance) {
        *error = env->ExceptionCheck() ? describe_exception(env) : "NewObjectA returned null";
        cleanup(&converted, nullptr);
        return false;
    }
    const jlong handle = register_object(env, instance.get(), error);
    cleanup(&converted, nullptr);
    if (handle == 0) {
        return false;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(handle);
    return true;
}

int JavaInvoker::lua_invoke(dejavu_control_session *, void *context, const dejavu_value *arguments,
                            size_t argument_count, dejavu_value *result, char *error,
                            size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string invoke_error;
    if (invoker == nullptr ||
        !invoker->invoke_jni(arguments, argument_count, result, &invoke_error)) {
        copy_error(error, error_size, invoke_error.empty() ? "java.invoke failed" : invoke_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int JavaInvoker::lua_new(dejavu_control_session *, void *context, const dejavu_value *arguments,
                         size_t argument_count, dejavu_value *result, char *error,
                         size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string construct_error;
    if (invoker == nullptr ||
        !invoker->construct_jni(arguments, argument_count, result, &construct_error)) {
        copy_error(error, error_size,
                   construct_error.empty() ? "java.new failed" : construct_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int JavaInvoker::lua_release(dejavu_control_session *, void *context, const dejavu_value *arguments,
                             size_t argument_count, dejavu_value *result, char *error,
                             size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    if (invoker == nullptr || argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[0].integer_value <= 0) {
        copy_error(error, error_size, "java.release expects a positive object handle");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string release_error;
    if (!invoker->release_handle(arguments[0].integer_value, &release_error)) {
        copy_error(error, error_size, release_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = 1;
    return DEJAVU_OK;
}

int JavaInvoker::lua_load_dex(dejavu_control_session *, void *context,
                              const dejavu_value *arguments, size_t argument_count,
                              dejavu_value *result, char *error, size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string load_error;
    if (invoker == nullptr || !invoker->load_dex(arguments, argument_count, result, &load_error)) {
        copy_error(error, error_size, load_error.empty() ? "java.load_dex failed" : load_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

jclass JavaInvoker::resolve_class(JNIEnv *env, const std::string &target, std::string *error) {
    jlong loader_handle = 0;
    std::string class_name;
    if (!resolve_class_target(target, &loader_handle, &class_name, error)) {
        return nullptr;
    }
    if (loader_handle != 0) {
        ScopedLocalRef<jobject> loader(env, lookup_object(env, loader_handle, error));
        if (!loader) {
            return nullptr;
        }
        return find_class_with_loader(env, class_name, loader.get(), error);
    }
    return find_class(env, class_name, error);
}

bool JavaInvoker::read_class_name(JNIEnv *env, jclass cls, std::string *name, std::string *error) {
    ScopedLocalRef<jclass> class_class(env, env->FindClass("java/lang/Class"));
    if (!class_class || env->ExceptionCheck()) {
        env->ExceptionClear();
        *error = "java.lang.Class is unavailable";
        return false;
    }
    const jmethodID get_name =
        env->GetMethodID(class_class.get(), "getName", "()Ljava/lang/String;");
    if (get_name == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        *error = "Class.getName is unavailable";
        return false;
    }
    ScopedLocalRef<jobject> value(env, env->CallObjectMethod(cls, get_name));
    if (env->ExceptionCheck() || !value) {
        *error = env->ExceptionCheck() ? describe_exception(env) : "Class.getName returned null";
        return false;
    }
    return read_string(env, static_cast<jstring>(value.get()), name, error);
}

bool JavaInvoker::read_object_class_name(JNIEnv *env, jobject obj, std::string *name,
                                         std::string *error) {
    ScopedLocalRef<jclass> cls(env, env->GetObjectClass(obj));
    if (!cls || env->ExceptionCheck()) {
        env->ExceptionClear();
        *error = "unable to resolve object class";
        return false;
    }
    return read_class_name(env, cls.get(), name, error);
}

bool JavaInvoker::verify_instance_of(JNIEnv *env, jobject obj, jclass cls, const char *context,
                                     std::string *error) {
    const jboolean matches = env->IsInstanceOf(obj, cls);
    if (env->ExceptionCheck() || matches != JNI_TRUE) {
        env->ExceptionClear();
        std::string actual;
        std::string expected;
        std::string ignored;
        read_object_class_name(env, obj, &actual, &ignored);
        read_class_name(env, cls, &expected, &ignored);
        *error = std::string(context) + ": object (" + (actual.empty() ? "?" : actual) +
                 ") is not an instance of " + (expected.empty() ? "?" : expected);
        return false;
    }
    return true;
}

bool JavaInvoker::invoke_as_jni(const dejavu_value *arguments, size_t argument_count,
                                dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if (argument_count < 4 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[1].type != DEJAVU_VALUE_STRING || arguments[2].type != DEJAVU_VALUE_STRING ||
        arguments[3].type != DEJAVU_VALUE_STRING) {
        *error = "java._invoke_as expects handle, class, method, JNI signature and arguments";
        return false;
    }
    ScopedLocalRef<jobject> receiver(env, lookup_object(env, arguments[0].integer_value, error));
    if (!receiver) {
        return false;
    }
    jlong loader_handle = 0;
    std::string target_class_name;
    if (!resolve_class_target(value_string(arguments[1]), &loader_handle, &target_class_name,
                              error)) {
        return false;
    }
    ScopedLocalRef<jobject> loader_ref(env);
    ScopedLocalRef<jclass> class_object(env);
    if (loader_handle != 0) {
        loader_ref.reset(lookup_object(env, loader_handle, error));
        if (!loader_ref) {
            return false;
        }
        class_object.reset(find_class_with_loader(env, target_class_name, loader_ref.get(), error));
    } else {
        class_object.reset(find_class(env, target_class_name, error));
    }
    if (!class_object) {
        return false;
    }
    if (!verify_instance_of(env, receiver.get(), class_object.get(), "java._invoke_as", error)) {
        return false;
    }
    const std::string method_name = value_string(arguments[2]);
    const std::string signature = value_string(arguments[3]);
    std::vector<std::string> parameters;
    std::string return_type;
    if (!parse_jni_signature(signature, &parameters, &return_type, error)) {
        return false;
    }
    const jmethodID method =
        env->GetMethodID(class_object.get(), method_name.c_str(), signature.c_str());
    if (method == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        *error = "method not found or signature mismatch: " + method_name + signature;
        return false;
    }
    ConvertedArguments converted;
    if (!convert_arguments(env, parameters, arguments + 4, argument_count - 4, &converted, error,
                           loader_ref.get())) {
        cleanup(&converted, nullptr);
        return false;
    }
    jvalue value = {};
    const bool called = call_primitive(env, false, class_object.get(), receiver.get(), method,
                                       converted.values, return_type, &value, error);
    if (!called) {
        cleanup(&converted, nullptr);
        return false;
    }
    const bool converted_result = convert_return(env, return_type, value, result, error);
    cleanup(&converted, nullptr);
    return converted_result;
}

bool JavaInvoker::class_name_jni(const dejavu_value *arguments, size_t argument_count,
                                 dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER) {
        *error = "java._class_name expects an object handle";
        return false;
    }
    ScopedLocalRef<jobject> object(env, lookup_object(env, arguments[0].integer_value, error));
    if (!object) {
        return false;
    }
    std::string name;
    if (!read_object_class_name(env, object.get(), &name, error)) {
        return false;
    }
    if (!set_owned_string(result, name)) {
        *error = "unable to allocate class name";
        return false;
    }
    return true;
}

bool JavaInvoker::class_object_jni(const dejavu_value *arguments, size_t argument_count,
                                   dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_STRING) {
        *error = "java._class_object expects a class name";
        return false;
    }
    ScopedLocalRef<jclass> cls(env, resolve_class(env, value_string(arguments[0]), error));
    if (!cls) {
        return false;
    }
    const jlong handle = register_object(env, cls.get(), error);
    if (handle == 0) {
        return false;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(handle);
    return true;
}

bool JavaInvoker::is_instance_jni(const dejavu_value *arguments, size_t argument_count,
                                  dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if (argument_count != 2 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[1].type != DEJAVU_VALUE_STRING) {
        *error = "java._is_instance expects an object handle and a class name";
        return false;
    }
    ScopedLocalRef<jobject> object(env, lookup_object(env, arguments[0].integer_value, error));
    if (!object) {
        return false;
    }
    ScopedLocalRef<jclass> cls(env, resolve_class(env, value_string(arguments[1]), error));
    if (!cls) {
        return false;
    }
    const jboolean matches = env->IsInstanceOf(object.get(), cls.get());
    if (env->ExceptionCheck()) {
        *error = describe_exception(env);
        return false;
    }
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = matches == JNI_TRUE ? 1 : 0;
    return true;
}

bool JavaInvoker::reflect_jni(const dejavu_value *arguments, size_t argument_count,
                              dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if (argument_count != 1 ||
        (arguments[0].type != DEJAVU_VALUE_STRING && arguments[0].type != DEJAVU_VALUE_INTEGER)) {
        *error = "java._reflect expects a class name or object handle";
        return false;
    }
    ScopedLocalRef<jclass> root_cls(env);
    if (arguments[0].type == DEJAVU_VALUE_STRING) {
        root_cls.reset(resolve_class(env, value_string(arguments[0]), error));
    } else {
        ScopedLocalRef<jobject> object(env, lookup_object(env, arguments[0].integer_value, error));
        if (!object) {
            return false;
        }
        root_cls.reset(env->GetObjectClass(object.get()));
        if (!root_cls || env->ExceptionCheck()) {
            env->ExceptionClear();
            *error = "unable to resolve object class";
            return false;
        }
    }
    if (!root_cls) {
        return false;
    }

    auto find_sys_class = [&](const char *name) -> jclass {
        jclass cls = env->FindClass(name);
        if (cls == nullptr || env->ExceptionCheck()) {
            env->ExceptionClear();
            if (cls != nullptr) {
                env->DeleteLocalRef(cls);
            }
            return nullptr;
        }
        return cls;
    };
    auto get_mid = [&](jclass cls, const char *name, const char *sig) -> jmethodID {
        jmethodID mid = env->GetMethodID(cls, name, sig);
        if (mid == nullptr || env->ExceptionCheck()) {
            env->ExceptionClear();
            return nullptr;
        }
        return mid;
    };

    ScopedLocalRef<jclass> cls_class(env, find_sys_class("java/lang/Class"));
    ScopedLocalRef<jclass> ctor_class(env, find_sys_class("java/lang/reflect/Constructor"));
    ScopedLocalRef<jclass> method_class(env, find_sys_class("java/lang/reflect/Method"));
    ScopedLocalRef<jclass> field_class(env, find_sys_class("java/lang/reflect/Field"));
    if (!cls_class || !ctor_class || !method_class || !field_class) {
        *error = "Java reflection classes are unavailable";
        return false;
    }

    const jmethodID cls_get_name = get_mid(cls_class.get(), "getName", "()Ljava/lang/String;");
    const jmethodID cls_get_super =
        get_mid(cls_class.get(), "getSuperclass", "()Ljava/lang/Class;");
    const jmethodID cls_get_ifaces =
        get_mid(cls_class.get(), "getInterfaces", "()[Ljava/lang/Class;");
    const jmethodID cls_get_ctors =
        get_mid(cls_class.get(), "getDeclaredConstructors", "()[Ljava/lang/reflect/Constructor;");
    const jmethodID cls_get_methods =
        get_mid(cls_class.get(), "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    const jmethodID cls_get_fields =
        get_mid(cls_class.get(), "getDeclaredFields", "()[Ljava/lang/reflect/Field;");

    const jmethodID ctor_param_types =
        get_mid(ctor_class.get(), "getParameterTypes", "()[Ljava/lang/Class;");
    const jmethodID method_get_name =
        get_mid(method_class.get(), "getName", "()Ljava/lang/String;");
    const jmethodID method_get_mods = get_mid(method_class.get(), "getModifiers", "()I");
    const jmethodID method_param_types =
        get_mid(method_class.get(), "getParameterTypes", "()[Ljava/lang/Class;");
    const jmethodID method_ret_type =
        get_mid(method_class.get(), "getReturnType", "()Ljava/lang/Class;");

    const jmethodID field_get_name = get_mid(field_class.get(), "getName", "()Ljava/lang/String;");
    const jmethodID field_get_mods = get_mid(field_class.get(), "getModifiers", "()I");
    const jmethodID field_get_type = get_mid(field_class.get(), "getType", "()Ljava/lang/Class;");

    cls_class.reset();
    ctor_class.reset();
    method_class.reset();
    field_class.reset();

    if (!cls_get_name || !cls_get_super || !cls_get_ifaces || !cls_get_ctors || !cls_get_methods ||
        !cls_get_fields || !ctor_param_types || !method_get_name || !method_get_mods ||
        !method_param_types || !method_ret_type || !field_get_name || !field_get_mods ||
        !field_get_type) {
        *error = "unable to resolve Java reflection methods";
        return false;
    }

    auto read_cls_name_fast = [&](jobject type_cls, std::string *out) -> bool {
        if (type_cls == nullptr) {
            return false;
        }
        ScopedLocalRef<jobject> jname(env, env->CallObjectMethod(type_cls, cls_get_name));
        if (env->ExceptionCheck() || !jname) {
            env->ExceptionClear();
            return false;
        }
        std::string ignored;
        return read_string(env, static_cast<jstring>(jname.get()), out, &ignored);
    };

    auto read_type_jni = [&](jobject type_cls, std::string *out) -> bool {
        std::string raw;
        if (!read_cls_name_fast(type_cls, &raw)) {
            return false;
        }
        *out = java_type_name_to_jni(raw);
        return true;
    };

    std::string root_name;
    if (!read_cls_name_fast(root_cls.get(), &root_name)) {
        *error = "unable to read class name";
        return false;
    }

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root_json(cJSON_CreateObject(), &cJSON_Delete);
    if (!root_json) {
        *error = "unable to allocate reflection metadata";
        return false;
    }
    cJSON *ctors_json = cJSON_AddArrayToObject(root_json.get(), "constructors");
    cJSON *methods_json = cJSON_AddObjectToObject(root_json.get(), "methods");
    cJSON *fields_json = cJSON_AddObjectToObject(root_json.get(), "fields");
    if (!ctors_json || !methods_json || !fields_json ||
        !cJSON_AddStringToObject(root_json.get(), "name", root_name.c_str())) {
        *error = "unable to initialize reflection metadata";
        return false;
    }

    // Collect constructors of the concrete class itself.
    if (env->PushLocalFrame(64) == 0) {
        auto ctors =
            static_cast<jobjectArray>(env->CallObjectMethod(root_cls.get(), cls_get_ctors));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            ctors = nullptr;
        }
        if (ctors != nullptr) {
            const jsize count = env->GetArrayLength(ctors);
            for (jsize i = 0; i < count; ++i) {
                if (env->PushLocalFrame(32) != 0) {
                    env->ExceptionClear();
                    break;
                }
                jobject ctor = env->GetObjectArrayElement(ctors, i);
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    ctor = nullptr;
                }
                if (ctor != nullptr) {
                    auto ptypes =
                        static_cast<jobjectArray>(env->CallObjectMethod(ctor, ctor_param_types));
                    if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                        ptypes = nullptr;
                    }
                    if (ptypes != nullptr) {
                        const jsize plen = env->GetArrayLength(ptypes);
                        std::string sig = "(";
                        cJSON *params_arr = cJSON_CreateArray();
                        bool ok = params_arr != nullptr;
                        for (jsize p = 0; ok && p < plen; ++p) {
                            jobject pcls = env->GetObjectArrayElement(ptypes, p);
                            if (env->ExceptionCheck()) {
                                env->ExceptionClear();
                                pcls = nullptr;
                                ok = false;
                            }
                            std::string pjni;
                            if (ok && !read_type_jni(pcls, &pjni)) {
                                ok = false;
                            }
                            if (pcls != nullptr) {
                                env->DeleteLocalRef(pcls);
                            }
                            if (!ok) {
                                break;
                            }
                            sig += pjni;
                            cJSON *pitem = cJSON_CreateString(pjni.c_str());
                            if (!pitem || !cJSON_AddItemToArray(params_arr, pitem)) {
                                cJSON_Delete(pitem);
                                ok = false;
                                break;
                            }
                        }
                        sig += ")V";
                        if (ok) {
                            cJSON *entry = cJSON_CreateObject();
                            if (entry && cJSON_AddStringToObject(entry, "sig", sig.c_str()) &&
                                cJSON_AddItemToObject(entry, "params", params_arr)) {
                                if (!cJSON_AddItemToArray(ctors_json, entry)) {
                                    cJSON_Delete(entry);
                                }
                            } else {
                                cJSON_Delete(entry);
                                cJSON_Delete(params_arr);
                            }
                        } else {
                            cJSON_Delete(params_arr);
                        }
                    }
                }
                env->PopLocalFrame(nullptr);
            }
        }
        env->PopLocalFrame(nullptr);
    } else {
        env->ExceptionClear();
    }

    // BFS over class -> superclasses -> interfaces.
    struct ScopedClassQueue {
        JNIEnv *env;
        std::deque<jclass> items;
        ~ScopedClassQueue() {
            for (jclass c : items) {
                if (c != nullptr) {
                    env->DeleteLocalRef(c);
                }
            }
        }
    } queue{env, {}};

    std::unordered_set<std::string> visited_classes;
    std::unordered_set<std::string> seen_methods;
    std::unordered_set<std::string> seen_fields;

    auto root_copy = static_cast<jclass>(env->NewLocalRef(root_cls.get()));
    if (root_copy == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        *error = "unable to create local reference for class reflection";
        return false;
    }
    queue.items.push_back(root_copy);
    visited_classes.insert(root_name);

    while (!queue.items.empty()) {
        ScopedLocalRef<jclass> cur(env, queue.items.front());
        queue.items.pop_front();

        // Collect declared methods.
        if (env->PushLocalFrame(64) == 0) {
            auto methods =
                static_cast<jobjectArray>(env->CallObjectMethod(cur.get(), cls_get_methods));
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                methods = nullptr;
            }
            if (methods != nullptr) {
                const jsize count = env->GetArrayLength(methods);
                for (jsize i = 0; i < count; ++i) {
                    if (env->PushLocalFrame(32) != 0) {
                        env->ExceptionClear();
                        break;
                    }
                    jobject m = env->GetObjectArrayElement(methods, i);
                    if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                        m = nullptr;
                    }
                    if (m != nullptr) {
                        const jint mods = env->CallIntMethod(m, method_get_mods);
                        const bool skip_method =
                            env->ExceptionCheck() ||
                            (mods & (kModifierBridge | kModifierSynthetic)) != 0;
                        if (env->ExceptionCheck()) {
                            env->ExceptionClear();
                        }
                        if (!skip_method) {
                            jobject jname = env->CallObjectMethod(m, method_get_name);
                            if (env->ExceptionCheck()) {
                                env->ExceptionClear();
                                jname = nullptr;
                            }
                            jobject rcls =
                                jname ? env->CallObjectMethod(m, method_ret_type) : nullptr;
                            if (env->ExceptionCheck()) {
                                env->ExceptionClear();
                                rcls = nullptr;
                            }
                            auto ptypes = rcls ? static_cast<jobjectArray>(
                                                     env->CallObjectMethod(m, method_param_types))
                                               : nullptr;
                            if (env->ExceptionCheck()) {
                                env->ExceptionClear();
                                ptypes = nullptr;
                            }
                            if (jname != nullptr && rcls != nullptr && ptypes != nullptr) {
                                std::string mname;
                                std::string rjni;
                                std::string ignored;
                                if (read_string(env, static_cast<jstring>(jname), &mname,
                                                &ignored) &&
                                    read_type_jni(rcls, &rjni)) {
                                    const jsize plen = env->GetArrayLength(ptypes);
                                    std::string param_sig = "(";
                                    cJSON *params_arr = cJSON_CreateArray();
                                    bool ok = params_arr != nullptr;
                                    for (jsize p = 0; ok && p < plen; ++p) {
                                        jobject pcls = env->GetObjectArrayElement(ptypes, p);
                                        if (env->ExceptionCheck()) {
                                            env->ExceptionClear();
                                            pcls = nullptr;
                                            ok = false;
                                        }
                                        std::string pjni;
                                        if (ok && !read_type_jni(pcls, &pjni)) {
                                            ok = false;
                                        }
                                        if (pcls != nullptr) {
                                            env->DeleteLocalRef(pcls);
                                        }
                                        if (!ok) {
                                            break;
                                        }
                                        param_sig += pjni;
                                        cJSON *pitem = cJSON_CreateString(pjni.c_str());
                                        if (!pitem || !cJSON_AddItemToArray(params_arr, pitem)) {
                                            cJSON_Delete(pitem);
                                            ok = false;
                                            break;
                                        }
                                    }
                                    param_sig += ")";
                                    const std::string sig = param_sig + rjni;
                                    const std::string key =
                                        mname + "#" + ((mods & kModifierStatic) != 0 ? "S" : "I") +
                                        "#" + param_sig;
                                    if (ok && seen_methods.insert(key).second) {
                                        cJSON *bucket = cJSON_GetObjectItemCaseSensitive(
                                            methods_json, mname.c_str());
                                        if (bucket == nullptr) {
                                            bucket =
                                                cJSON_AddArrayToObject(methods_json, mname.c_str());
                                        }
                                        cJSON *entry = bucket ? cJSON_CreateObject() : nullptr;
                                        if (entry &&
                                            cJSON_AddStringToObject(entry, "sig", sig.c_str()) &&
                                            cJSON_AddBoolToObject(entry, "static",
                                                                  (mods & kModifierStatic) != 0) &&
                                            cJSON_AddItemToObject(entry, "params", params_arr) &&
                                            cJSON_AddStringToObject(entry, "ret", rjni.c_str())) {
                                            if (!cJSON_AddItemToArray(bucket, entry)) {
                                                cJSON_Delete(entry);
                                            }
                                        } else {
                                            cJSON_Delete(entry);
                                            cJSON_Delete(params_arr);
                                        }
                                    } else {
                                        cJSON_Delete(params_arr);
                                    }
                                }
                            }
                        }
                    }
                    env->PopLocalFrame(nullptr);
                }
            }
            env->PopLocalFrame(nullptr);
        } else {
            env->ExceptionClear();
        }

        // Collect declared fields.
        if (env->PushLocalFrame(64) == 0) {
            auto fields =
                static_cast<jobjectArray>(env->CallObjectMethod(cur.get(), cls_get_fields));
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                fields = nullptr;
            }
            if (fields != nullptr) {
                const jsize count = env->GetArrayLength(fields);
                for (jsize i = 0; i < count; ++i) {
                    if (env->PushLocalFrame(32) != 0) {
                        env->ExceptionClear();
                        break;
                    }
                    jobject f = env->GetObjectArrayElement(fields, i);
                    if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                        f = nullptr;
                    }
                    if (f != nullptr) {
                        jobject jname = env->CallObjectMethod(f, field_get_name);
                        if (env->ExceptionCheck()) {
                            env->ExceptionClear();
                            jname = nullptr;
                        }
                        const jint mods = jname ? env->CallIntMethod(f, field_get_mods) : 0;
                        if (env->ExceptionCheck()) {
                            env->ExceptionClear();
                            jname = nullptr;
                        }
                        jobject fcls = jname ? env->CallObjectMethod(f, field_get_type) : nullptr;
                        if (env->ExceptionCheck()) {
                            env->ExceptionClear();
                            fcls = nullptr;
                        }
                        if (jname != nullptr && fcls != nullptr) {
                            std::string fname;
                            std::string fjni;
                            std::string ignored;
                            if (read_string(env, static_cast<jstring>(jname), &fname, &ignored) &&
                                read_type_jni(fcls, &fjni) && seen_fields.insert(fname).second) {
                                cJSON *entry = cJSON_CreateObject();
                                if (entry && cJSON_AddStringToObject(entry, "sig", fjni.c_str()) &&
                                    cJSON_AddBoolToObject(entry, "static",
                                                          (mods & kModifierStatic) != 0)) {
                                    if (!cJSON_AddItemToObject(fields_json, fname.c_str(), entry)) {
                                        cJSON_Delete(entry);
                                    }
                                } else {
                                    cJSON_Delete(entry);
                                }
                            }
                        }
                    }
                    env->PopLocalFrame(nullptr);
                }
            }
            env->PopLocalFrame(nullptr);
        } else {
            env->ExceptionClear();
        }

        if (env->EnsureLocalCapacity(32) != 0) {
            env->ExceptionClear();
            continue;
        }

        // Enqueue superclass and interfaces.
        jobject super_obj = env->CallObjectMethod(cur.get(), cls_get_super);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            if (super_obj != nullptr) {
                env->DeleteLocalRef(super_obj);
                super_obj = nullptr;
            }
        }
        if (super_obj != nullptr) {
            auto super_cls = static_cast<jclass>(super_obj);
            std::string sname;
            if (read_cls_name_fast(super_cls, &sname) && visited_classes.insert(sname).second) {
                queue.items.push_back(super_cls);
            } else {
                env->DeleteLocalRef(super_cls);
            }
        }
        ScopedLocalRef<jobjectArray> ifaces(
            env, static_cast<jobjectArray>(env->CallObjectMethod(cur.get(), cls_get_ifaces)));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            ifaces.reset();
        }
        if (ifaces) {
            const jsize icount = env->GetArrayLength(ifaces.get());
            for (jsize i = 0; i < icount; ++i) {
                if (env->EnsureLocalCapacity(16) != 0) {
                    env->ExceptionClear();
                    break;
                }
                auto iface_cls = static_cast<jclass>(env->GetObjectArrayElement(ifaces.get(), i));
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    if (iface_cls != nullptr) {
                        env->DeleteLocalRef(iface_cls);
                    }
                    continue;
                }
                if (iface_cls != nullptr) {
                    std::string iname;
                    if (read_cls_name_fast(iface_cls, &iname) &&
                        visited_classes.insert(iname).second) {
                        queue.items.push_back(iface_cls);
                    } else {
                        env->DeleteLocalRef(iface_cls);
                    }
                }
            }
        }
    }

    char *printed = cJSON_PrintUnformatted(root_json.get());
    if (printed == nullptr) {
        *error = "unable to serialize class reflection metadata";
        return false;
    }
    const bool ok = set_owned_string(result, printed);
    cJSON_free(printed);
    if (!ok) {
        *error = "unable to allocate reflection JSON";
        return false;
    }
    return true;
}

bool JavaInvoker::get_field_jni(const dejavu_value *arguments, size_t argument_count,
                                dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if ((argument_count != 3 && argument_count != 4) ||
        (arguments[0].type != DEJAVU_VALUE_STRING && arguments[0].type != DEJAVU_VALUE_INTEGER) ||
        arguments[1].type != DEJAVU_VALUE_STRING || arguments[2].type != DEJAVU_VALUE_STRING) {
        *error =
            "java._get_field expects class-or-handle, field name, JNI type [, declaring class]";
        return false;
    }
    const bool is_static = arguments[0].type == DEJAVU_VALUE_STRING;
    if (argument_count == 4 && (is_static || arguments[3].type != DEJAVU_VALUE_STRING)) {
        *error = "java._get_field declaring class must be a string and is only valid for instance "
                 "fields";
        return false;
    }
    const std::string field_name = value_string(arguments[1]);
    const std::string field_sig = value_string(arguments[2]);
    size_t offset = 0;
    std::string field_type;
    if (!parse_type_descriptor(field_sig, &offset, false, &field_type, error) ||
        offset != field_sig.size()) {
        if (error->empty()) {
            *error = "invalid JNI field type: " + field_sig;
        }
        return false;
    }
    ScopedLocalRef<jobject> receiver(env);
    ScopedLocalRef<jclass> cls(env);
    if (is_static) {
        cls.reset(resolve_class(env, value_string(arguments[0]), error));
    } else {
        receiver.reset(lookup_object(env, arguments[0].integer_value, error));
        if (!receiver) {
            return false;
        }
        if (argument_count == 4) {
            cls.reset(resolve_class(env, value_string(arguments[3]), error));
            if (cls &&
                !verify_instance_of(env, receiver.get(), cls.get(), "java._get_field", error)) {
                return false;
            }
        } else {
            cls.reset(env->GetObjectClass(receiver.get()));
            if (!cls || env->ExceptionCheck()) {
                env->ExceptionClear();
                *error = "unable to resolve object class";
                return false;
            }
        }
    }
    if (!cls) {
        return false;
    }
    const jfieldID fid =
        is_static ? env->GetStaticFieldID(cls.get(), field_name.c_str(), field_type.c_str())
                  : env->GetFieldID(cls.get(), field_name.c_str(), field_type.c_str());
    if (fid == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        *error = "field not found or type mismatch: " + field_name + ":" + field_type;
        return false;
    }
    jvalue value = {};
    switch (field_type[0]) {
    case 'Z':
        value.z = is_static ? env->GetStaticBooleanField(cls.get(), fid)
                            : env->GetBooleanField(receiver.get(), fid);
        break;
    case 'B':
        value.b = is_static ? env->GetStaticByteField(cls.get(), fid)
                            : env->GetByteField(receiver.get(), fid);
        break;
    case 'C':
        value.c = is_static ? env->GetStaticCharField(cls.get(), fid)
                            : env->GetCharField(receiver.get(), fid);
        break;
    case 'S':
        value.s = is_static ? env->GetStaticShortField(cls.get(), fid)
                            : env->GetShortField(receiver.get(), fid);
        break;
    case 'I':
        value.i = is_static ? env->GetStaticIntField(cls.get(), fid)
                            : env->GetIntField(receiver.get(), fid);
        break;
    case 'J':
        value.j = is_static ? env->GetStaticLongField(cls.get(), fid)
                            : env->GetLongField(receiver.get(), fid);
        break;
    case 'F':
        value.f = is_static ? env->GetStaticFloatField(cls.get(), fid)
                            : env->GetFloatField(receiver.get(), fid);
        break;
    case 'D':
        value.d = is_static ? env->GetStaticDoubleField(cls.get(), fid)
                            : env->GetDoubleField(receiver.get(), fid);
        break;
    case 'L':
    case '[':
        value.l = is_static ? env->GetStaticObjectField(cls.get(), fid)
                            : env->GetObjectField(receiver.get(), fid);
        break;
    default:
        *error = "unsupported field type: " + field_type;
        return false;
    }
    if (env->ExceptionCheck()) {
        *error = describe_exception(env);
        if ((field_type[0] == 'L' || field_type[0] == '[') && value.l != nullptr) {
            env->DeleteLocalRef(value.l);
        }
        return false;
    }
    return convert_return(env, field_type, value, result, error);
}

bool JavaInvoker::set_field_jni(const dejavu_value *arguments, size_t argument_count,
                                dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if ((argument_count != 4 && argument_count != 5) ||
        (arguments[0].type != DEJAVU_VALUE_STRING && arguments[0].type != DEJAVU_VALUE_INTEGER) ||
        arguments[1].type != DEJAVU_VALUE_STRING || arguments[2].type != DEJAVU_VALUE_STRING) {
        *error = "java._set_field expects class-or-handle, field name, JNI type, value [, "
                 "declaring class]";
        return false;
    }
    const bool is_static = arguments[0].type == DEJAVU_VALUE_STRING;
    if (argument_count == 5 && (is_static || arguments[4].type != DEJAVU_VALUE_STRING)) {
        *error = "java._set_field declaring class must be a string and is only valid for instance "
                 "fields";
        return false;
    }
    const std::string field_name = value_string(arguments[1]);
    const std::string field_sig = value_string(arguments[2]);
    size_t offset = 0;
    std::string field_type;
    if (!parse_type_descriptor(field_sig, &offset, false, &field_type, error) ||
        offset != field_sig.size()) {
        if (error->empty()) {
            *error = "invalid JNI field type: " + field_sig;
        }
        return false;
    }
    ScopedLocalRef<jobject> receiver(env);
    ScopedLocalRef<jclass> cls(env);
    ScopedLocalRef<jobject> loader_ref(env);
    auto resolve_with_loader_keep = [&](const std::string &target) -> jclass {
        jlong loader_handle = 0;
        std::string class_name;
        if (!resolve_class_target(target, &loader_handle, &class_name, error)) {
            return nullptr;
        }
        if (loader_handle != 0) {
            loader_ref.reset(lookup_object(env, loader_handle, error));
            if (!loader_ref) {
                return nullptr;
            }
            return find_class_with_loader(env, class_name, loader_ref.get(), error);
        }
        return find_class(env, class_name, error);
    };
    if (is_static) {
        cls.reset(resolve_with_loader_keep(value_string(arguments[0])));
    } else {
        receiver.reset(lookup_object(env, arguments[0].integer_value, error));
        if (!receiver) {
            return false;
        }
        if (argument_count == 5) {
            cls.reset(resolve_with_loader_keep(value_string(arguments[4])));
            if (cls &&
                !verify_instance_of(env, receiver.get(), cls.get(), "java._set_field", error)) {
                return false;
            }
        } else {
            cls.reset(env->GetObjectClass(receiver.get()));
            if (!cls || env->ExceptionCheck()) {
                env->ExceptionClear();
                *error = "unable to resolve object class";
                return false;
            }
        }
    }
    if (!cls) {
        return false;
    }
    const jfieldID fid =
        is_static ? env->GetStaticFieldID(cls.get(), field_name.c_str(), field_type.c_str())
                  : env->GetFieldID(cls.get(), field_name.c_str(), field_type.c_str());
    if (fid == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        *error = "field not found or type mismatch: " + field_name + ":" + field_type;
        return false;
    }
    ConvertedArguments converted;
    const std::vector<std::string> params = {field_type};
    if (!convert_arguments(env, params, &arguments[3], 1, &converted, error, loader_ref.get())) {
        cleanup(&converted, nullptr);
        return false;
    }
    const jvalue &value = converted.values[0];
    switch (field_type[0]) {
    case 'Z':
        if (is_static) {
            env->SetStaticBooleanField(cls.get(), fid, value.z);
        } else {
            env->SetBooleanField(receiver.get(), fid, value.z);
        }
        break;
    case 'B':
        if (is_static) {
            env->SetStaticByteField(cls.get(), fid, value.b);
        } else {
            env->SetByteField(receiver.get(), fid, value.b);
        }
        break;
    case 'C':
        if (is_static) {
            env->SetStaticCharField(cls.get(), fid, value.c);
        } else {
            env->SetCharField(receiver.get(), fid, value.c);
        }
        break;
    case 'S':
        if (is_static) {
            env->SetStaticShortField(cls.get(), fid, value.s);
        } else {
            env->SetShortField(receiver.get(), fid, value.s);
        }
        break;
    case 'I':
        if (is_static) {
            env->SetStaticIntField(cls.get(), fid, value.i);
        } else {
            env->SetIntField(receiver.get(), fid, value.i);
        }
        break;
    case 'J':
        if (is_static) {
            env->SetStaticLongField(cls.get(), fid, value.j);
        } else {
            env->SetLongField(receiver.get(), fid, value.j);
        }
        break;
    case 'F':
        if (is_static) {
            env->SetStaticFloatField(cls.get(), fid, value.f);
        } else {
            env->SetFloatField(receiver.get(), fid, value.f);
        }
        break;
    case 'D':
        if (is_static) {
            env->SetStaticDoubleField(cls.get(), fid, value.d);
        } else {
            env->SetDoubleField(receiver.get(), fid, value.d);
        }
        break;
    case 'L':
    case '[':
        if (is_static) {
            env->SetStaticObjectField(cls.get(), fid, value.l);
        } else {
            env->SetObjectField(receiver.get(), fid, value.l);
        }
        break;
    default:
        cleanup(&converted, nullptr);
        *error = "unsupported field type: " + field_type;
        return false;
    }
    if (env->ExceptionCheck()) {
        *error = describe_exception(env);
        cleanup(&converted, nullptr);
        return false;
    }
    cleanup(&converted, nullptr);
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = 1;
    return true;
}

bool JavaInvoker::array_new_jni(const dejavu_value *arguments, size_t argument_count,
                                dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if ((argument_count != 2 && argument_count != 3) || arguments[0].type != DEJAVU_VALUE_STRING) {
        *error = "java._array_new expects element JNI type and length (or bytes string)";
        return false;
    }
    jlong loader_handle = 0;
    if (argument_count == 3 && arguments[2].type != DEJAVU_VALUE_NIL) {
        if (arguments[2].type != DEJAVU_VALUE_INTEGER || arguments[2].integer_value <= 0) {
            *error = "java._array_new optional loader must be a positive object handle";
            return false;
        }
        loader_handle = static_cast<jlong>(arguments[2].integer_value);
    }
    const std::string elem_sig = value_string(arguments[0]);
    size_t offset = 0;
    std::string elem_type;
    if (!parse_type_descriptor(elem_sig, &offset, false, &elem_type, error) ||
        offset != elem_sig.size()) {
        if (error->empty()) {
            *error = "invalid array element JNI type: " + elem_sig;
        }
        return false;
    }

    ScopedLocalRef<jarray> arr(env);
    if (elem_type == "B" && arguments[1].type == DEJAVU_VALUE_STRING) {
        const size_t size = arguments[1].string_size;
        if (size > kMaximumByteArrayRead) {
            *error = "byte array exceeds maximum size";
            return false;
        }
        ScopedLocalRef<jbyteArray> barr(env, env->NewByteArray(static_cast<jsize>(size)));
        if (!barr || env->ExceptionCheck()) {
            *error = env->ExceptionCheck() ? describe_exception(env) : "NewByteArray failed";
            return false;
        }
        if (size > 0 && arguments[1].string_value != nullptr) {
            env->SetByteArrayRegion(barr.get(), 0, static_cast<jsize>(size),
                                    reinterpret_cast<const jbyte *>(arguments[1].string_value));
            if (env->ExceptionCheck()) {
                *error = describe_exception(env);
                return false;
            }
        }
        arr.reset(barr.release());
    } else {
        if (arguments[1].type != DEJAVU_VALUE_INTEGER || arguments[1].integer_value < 0 ||
            static_cast<size_t>(arguments[1].integer_value) > kMaximumArrayLength) {
            *error = "array length must be a non-negative integer within bounds";
            return false;
        }
        const jsize len = static_cast<jsize>(arguments[1].integer_value);
        switch (elem_type[0]) {
        case 'Z':
            arr.reset(env->NewBooleanArray(len));
            break;
        case 'B':
            arr.reset(env->NewByteArray(len));
            break;
        case 'C':
            arr.reset(env->NewCharArray(len));
            break;
        case 'S':
            arr.reset(env->NewShortArray(len));
            break;
        case 'I':
            arr.reset(env->NewIntArray(len));
            break;
        case 'J':
            arr.reset(env->NewLongArray(len));
            break;
        case 'F':
            arr.reset(env->NewFloatArray(len));
            break;
        case 'D':
            arr.reset(env->NewDoubleArray(len));
            break;
        case 'L':
        case '[': {
            std::string target = descriptor_to_dotted_class(elem_type);
            if (target.empty()) {
                *error = "unsupported array element type: " + elem_type;
                return false;
            }
            if (loader_handle > 0) {
                target = std::to_string(loader_handle) + ":" + target;
            }
            ScopedLocalRef<jclass> elem_cls(env, resolve_class(env, target, error));
            if (!elem_cls) {
                return false;
            }
            arr.reset(env->NewObjectArray(len, elem_cls.get(), nullptr));
            break;
        }
        default:
            *error = "unsupported array element type: " + elem_type;
            return false;
        }
        if (!arr || env->ExceptionCheck()) {
            *error = env->ExceptionCheck() ? describe_exception(env) : "unable to allocate array";
            return false;
        }
    }

    const jlong handle = register_object(env, arr.get(), error);
    if (handle == 0) {
        return false;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(handle);
    return true;
}

bool JavaInvoker::array_length_jni(const dejavu_value *arguments, size_t argument_count,
                                   dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER) {
        *error = "java._array_length expects an array handle";
        return false;
    }
    ScopedLocalRef<jobject> obj(env, lookup_object(env, arguments[0].integer_value, error));
    if (!obj) {
        return false;
    }
    std::string cname;
    if (!read_object_class_name(env, obj.get(), &cname, error)) {
        return false;
    }
    if (cname.size() < 2 || cname[0] != '[') {
        *error = "java._array_length: object (" + cname + ") is not a Java array";
        return false;
    }
    const jsize len = env->GetArrayLength(static_cast<jarray>(obj.get()));
    if (env->ExceptionCheck()) {
        *error = describe_exception(env);
        return false;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(len);
    return true;
}

bool JavaInvoker::array_get_jni(const dejavu_value *arguments, size_t argument_count,
                                dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if (argument_count != 3 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[1].type != DEJAVU_VALUE_STRING || arguments[2].type != DEJAVU_VALUE_INTEGER) {
        *error = "java._array_get expects array handle, element JNI type, and 0-based index";
        return false;
    }
    ScopedLocalRef<jobject> obj(env, lookup_object(env, arguments[0].integer_value, error));
    if (!obj) {
        return false;
    }
    const std::string elem_sig = value_string(arguments[1]);
    size_t offset = 0;
    std::string elem_type;
    if (!parse_type_descriptor(elem_sig, &offset, false, &elem_type, error) ||
        offset != elem_sig.size()) {
        if (error->empty()) {
            *error = "invalid array element JNI type: " + elem_sig;
        }
        return false;
    }
    std::string cname;
    if (!read_object_class_name(env, obj.get(), &cname, error)) {
        return false;
    }
    if (cname.size() < 2 || cname[0] != '[') {
        *error = "java._array_get: object (" + cname + ") is not a Java array";
        return false;
    }
    const bool expect_obj_array = elem_type[0] == 'L' || elem_type[0] == '[';
    if (expect_obj_array) {
        if (cname[1] != 'L' && cname[1] != '[') {
            *error = "java._array_get: array (" + cname + ") is not an object array";
            return false;
        }
    } else if (cname.size() != 2 || cname[1] != elem_type[0]) {
        *error = "java._array_get: array (" + cname + ") does not match element type " + elem_type;
        return false;
    }
    const jsize len = env->GetArrayLength(static_cast<jarray>(obj.get()));
    if (env->ExceptionCheck()) {
        *error = describe_exception(env);
        return false;
    }
    const long long index = arguments[2].integer_value;
    if (index < 0 || index >= static_cast<long long>(len)) {
        *error = "array index out of bounds: " + std::to_string(index) + " (length " +
                 std::to_string(len) + ")";
        return false;
    }
    const jsize idx = static_cast<jsize>(index);
    jvalue value = {};
    switch (elem_type[0]) {
    case 'Z':
        env->GetBooleanArrayRegion(static_cast<jbooleanArray>(obj.get()), idx, 1, &value.z);
        break;
    case 'B':
        env->GetByteArrayRegion(static_cast<jbyteArray>(obj.get()), idx, 1, &value.b);
        break;
    case 'C':
        env->GetCharArrayRegion(static_cast<jcharArray>(obj.get()), idx, 1, &value.c);
        break;
    case 'S':
        env->GetShortArrayRegion(static_cast<jshortArray>(obj.get()), idx, 1, &value.s);
        break;
    case 'I':
        env->GetIntArrayRegion(static_cast<jintArray>(obj.get()), idx, 1, &value.i);
        break;
    case 'J':
        env->GetLongArrayRegion(static_cast<jlongArray>(obj.get()), idx, 1, &value.j);
        break;
    case 'F':
        env->GetFloatArrayRegion(static_cast<jfloatArray>(obj.get()), idx, 1, &value.f);
        break;
    case 'D':
        env->GetDoubleArrayRegion(static_cast<jdoubleArray>(obj.get()), idx, 1, &value.d);
        break;
    case 'L':
    case '[':
        value.l = env->GetObjectArrayElement(static_cast<jobjectArray>(obj.get()), idx);
        break;
    default:
        *error = "unsupported array element type: " + elem_type;
        return false;
    }
    if (env->ExceptionCheck()) {
        *error = describe_exception(env);
        if (expect_obj_array && value.l != nullptr) {
            env->DeleteLocalRef(value.l);
        }
        return false;
    }
    return convert_return(env, elem_type, value, result, error);
}

bool JavaInvoker::array_set_jni(const dejavu_value *arguments, size_t argument_count,
                                dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if (argument_count != 4 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[1].type != DEJAVU_VALUE_STRING || arguments[2].type != DEJAVU_VALUE_INTEGER) {
        *error = "java._array_set expects array handle, element JNI type, 0-based index, and value";
        return false;
    }
    ScopedLocalRef<jobject> obj(env, lookup_object(env, arguments[0].integer_value, error));
    if (!obj) {
        return false;
    }
    const std::string elem_sig = value_string(arguments[1]);
    size_t offset = 0;
    std::string elem_type;
    if (!parse_type_descriptor(elem_sig, &offset, false, &elem_type, error) ||
        offset != elem_sig.size()) {
        if (error->empty()) {
            *error = "invalid array element JNI type: " + elem_sig;
        }
        return false;
    }
    std::string cname;
    if (!read_object_class_name(env, obj.get(), &cname, error)) {
        return false;
    }
    if (cname.size() < 2 || cname[0] != '[') {
        *error = "java._array_set: object (" + cname + ") is not a Java array";
        return false;
    }
    const bool expect_obj_array = elem_type[0] == 'L' || elem_type[0] == '[';
    if (expect_obj_array) {
        if (cname[1] != 'L' && cname[1] != '[') {
            *error = "java._array_set: array (" + cname + ") is not an object array";
            return false;
        }
    } else if (cname.size() != 2 || cname[1] != elem_type[0]) {
        *error = "java._array_set: array (" + cname + ") does not match element type " + elem_type;
        return false;
    }
    const jsize len = env->GetArrayLength(static_cast<jarray>(obj.get()));
    if (env->ExceptionCheck()) {
        *error = describe_exception(env);
        return false;
    }
    const long long index = arguments[2].integer_value;
    if (index < 0 || index >= static_cast<long long>(len)) {
        *error = "array index out of bounds: " + std::to_string(index) + " (length " +
                 std::to_string(len) + ")";
        return false;
    }
    ConvertedArguments converted;
    const std::vector<std::string> params = {elem_type};
    if (!convert_arguments(env, params, &arguments[3], 1, &converted, error)) {
        cleanup(&converted, nullptr);
        return false;
    }
    const jsize idx = static_cast<jsize>(index);
    const jvalue &value = converted.values[0];
    switch (elem_type[0]) {
    case 'Z':
        env->SetBooleanArrayRegion(static_cast<jbooleanArray>(obj.get()), idx, 1, &value.z);
        break;
    case 'B':
        env->SetByteArrayRegion(static_cast<jbyteArray>(obj.get()), idx, 1, &value.b);
        break;
    case 'C':
        env->SetCharArrayRegion(static_cast<jcharArray>(obj.get()), idx, 1, &value.c);
        break;
    case 'S':
        env->SetShortArrayRegion(static_cast<jshortArray>(obj.get()), idx, 1, &value.s);
        break;
    case 'I':
        env->SetIntArrayRegion(static_cast<jintArray>(obj.get()), idx, 1, &value.i);
        break;
    case 'J':
        env->SetLongArrayRegion(static_cast<jlongArray>(obj.get()), idx, 1, &value.j);
        break;
    case 'F':
        env->SetFloatArrayRegion(static_cast<jfloatArray>(obj.get()), idx, 1, &value.f);
        break;
    case 'D':
        env->SetDoubleArrayRegion(static_cast<jdoubleArray>(obj.get()), idx, 1, &value.d);
        break;
    case 'L':
    case '[':
        env->SetObjectArrayElement(static_cast<jobjectArray>(obj.get()), idx, value.l);
        break;
    default:
        cleanup(&converted, nullptr);
        *error = "unsupported array element type: " + elem_type;
        return false;
    }
    if (env->ExceptionCheck()) {
        *error = describe_exception(env);
        cleanup(&converted, nullptr);
        return false;
    }
    cleanup(&converted, nullptr);
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = 1;
    return true;
}

bool JavaInvoker::array_bytes_jni(const dejavu_value *arguments, size_t argument_count,
                                  dejavu_value *result, std::string *error) {
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER) {
        *error = "java._array_bytes expects a byte[] handle";
        return false;
    }
    ScopedLocalRef<jobject> obj(env, lookup_object(env, arguments[0].integer_value, error));
    if (!obj) {
        return false;
    }
    std::string cname;
    if (!read_object_class_name(env, obj.get(), &cname, error)) {
        return false;
    }
    if (cname != "[B") {
        *error = "java._array_bytes: object (" + cname + ") is not a byte[]";
        return false;
    }
    auto barr = static_cast<jbyteArray>(obj.get());
    const jsize len = env->GetArrayLength(barr);
    if (env->ExceptionCheck()) {
        *error = describe_exception(env);
        return false;
    }
    if (len < 0 || static_cast<size_t>(len) > kMaximumByteArrayRead) {
        *error = "byte[] length exceeds read limit";
        return false;
    }
    std::string bytes(static_cast<size_t>(len), '\0');
    if (len > 0) {
        env->GetByteArrayRegion(barr, 0, len, reinterpret_cast<jbyte *>(bytes.data()));
        if (env->ExceptionCheck()) {
            *error = describe_exception(env);
            return false;
        }
    }
    if (!set_owned_string(result, bytes)) {
        *error = "unable to allocate byte array string";
        return false;
    }
    return true;
}

int JavaInvoker::lua_invoke_as(dejavu_control_session *, void *context,
                               const dejavu_value *arguments, size_t argument_count,
                               dejavu_value *result, char *error, size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string detail;
    if (invoker == nullptr || !invoker->invoke_as_jni(arguments, argument_count, result, &detail)) {
        copy_error(error, error_size, detail.empty() ? "java._invoke_as failed" : detail);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int JavaInvoker::lua_reflect(dejavu_control_session *, void *context, const dejavu_value *arguments,
                             size_t argument_count, dejavu_value *result, char *error,
                             size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string detail;
    if (invoker == nullptr || !invoker->reflect_jni(arguments, argument_count, result, &detail)) {
        copy_error(error, error_size, detail.empty() ? "java._reflect failed" : detail);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int JavaInvoker::lua_class_name(dejavu_control_session *, void *context,
                                const dejavu_value *arguments, size_t argument_count,
                                dejavu_value *result, char *error, size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string detail;
    if (invoker == nullptr ||
        !invoker->class_name_jni(arguments, argument_count, result, &detail)) {
        copy_error(error, error_size, detail.empty() ? "java._class_name failed" : detail);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int JavaInvoker::lua_class_object(dejavu_control_session *, void *context,
                                  const dejavu_value *arguments, size_t argument_count,
                                  dejavu_value *result, char *error, size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string detail;
    if (invoker == nullptr ||
        !invoker->class_object_jni(arguments, argument_count, result, &detail)) {
        copy_error(error, error_size, detail.empty() ? "java._class_object failed" : detail);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int JavaInvoker::lua_is_instance(dejavu_control_session *, void *context,
                                 const dejavu_value *arguments, size_t argument_count,
                                 dejavu_value *result, char *error, size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string detail;
    if (invoker == nullptr ||
        !invoker->is_instance_jni(arguments, argument_count, result, &detail)) {
        copy_error(error, error_size, detail.empty() ? "java._is_instance failed" : detail);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int JavaInvoker::lua_get_field(dejavu_control_session *, void *context,
                               const dejavu_value *arguments, size_t argument_count,
                               dejavu_value *result, char *error, size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string detail;
    if (invoker == nullptr || !invoker->get_field_jni(arguments, argument_count, result, &detail)) {
        copy_error(error, error_size, detail.empty() ? "java._get_field failed" : detail);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int JavaInvoker::lua_set_field(dejavu_control_session *, void *context,
                               const dejavu_value *arguments, size_t argument_count,
                               dejavu_value *result, char *error, size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string detail;
    if (invoker == nullptr || !invoker->set_field_jni(arguments, argument_count, result, &detail)) {
        copy_error(error, error_size, detail.empty() ? "java._set_field failed" : detail);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int JavaInvoker::lua_array_new(dejavu_control_session *, void *context,
                               const dejavu_value *arguments, size_t argument_count,
                               dejavu_value *result, char *error, size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string detail;
    if (invoker == nullptr || !invoker->array_new_jni(arguments, argument_count, result, &detail)) {
        copy_error(error, error_size, detail.empty() ? "java._array_new failed" : detail);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int JavaInvoker::lua_array_length(dejavu_control_session *, void *context,
                                  const dejavu_value *arguments, size_t argument_count,
                                  dejavu_value *result, char *error, size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string detail;
    if (invoker == nullptr ||
        !invoker->array_length_jni(arguments, argument_count, result, &detail)) {
        copy_error(error, error_size, detail.empty() ? "java._array_length failed" : detail);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int JavaInvoker::lua_array_get(dejavu_control_session *, void *context,
                               const dejavu_value *arguments, size_t argument_count,
                               dejavu_value *result, char *error, size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string detail;
    if (invoker == nullptr || !invoker->array_get_jni(arguments, argument_count, result, &detail)) {
        copy_error(error, error_size, detail.empty() ? "java._array_get failed" : detail);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int JavaInvoker::lua_array_set(dejavu_control_session *, void *context,
                               const dejavu_value *arguments, size_t argument_count,
                               dejavu_value *result, char *error, size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string detail;
    if (invoker == nullptr || !invoker->array_set_jni(arguments, argument_count, result, &detail)) {
        copy_error(error, error_size, detail.empty() ? "java._array_set failed" : detail);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int JavaInvoker::lua_array_bytes(dejavu_control_session *, void *context,
                                 const dejavu_value *arguments, size_t argument_count,
                                 dejavu_value *result, char *error, size_t error_size) {
    auto *invoker = static_cast<JavaInvoker *>(context);
    std::string detail;
    if (invoker == nullptr ||
        !invoker->array_bytes_jni(arguments, argument_count, result, &detail)) {
        copy_error(error, error_size, detail.empty() ? "java._array_bytes failed" : detail);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}
