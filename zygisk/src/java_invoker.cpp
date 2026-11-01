#include "java_invoker.h"

#include <android/log.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr jlong kMaximumObjectHandles = 1024;

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
        {"new", JavaInvoker::lua_new},
        {"release", JavaInvoker::lua_release},
        {"load_dex", JavaInvoker::lua_load_dex},
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
            *error = "class not found: " + name;
            return nullptr;
        }
        return result;
    }
    jclass class_loader_class = env->GetObjectClass(loader);
    if (class_loader_class == nullptr) {
        env->ExceptionClear();
        *error = "unable to inspect the dex ClassLoader";
        return nullptr;
    }
    const jmethodID load =
        env->GetMethodID(class_loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (load == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(class_loader_class);
        *error = "unable to resolve ClassLoader.loadClass";
        return nullptr;
    }
    jstring class_name = create_string(env, dotted, error);
    if (class_name == nullptr) {
        env->DeleteLocalRef(class_loader_class);
        return nullptr;
    }
    jobject loaded = env->CallObjectMethod(loader, load, class_name);
    env->DeleteLocalRef(class_name);
    env->DeleteLocalRef(class_loader_class);
    if (env->ExceptionCheck()) {
        *error = describe_exception(env);
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

jobject JavaInvoker::lookup_object(long long handle, std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = objects_.find(static_cast<jlong>(handle));
    if (iterator == objects_.end()) {
        *error = "unknown Java object handle: " + std::to_string(handle);
        return nullptr;
    }
    return iterator->second;
}

bool JavaInvoker::release_handle(long long handle, std::string *error) {
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
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
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
                                    ConvertedArguments *converted, std::string *error) {
    if (argument_count != parameters.size()) {
        *error = "expected " + std::to_string(parameters.size()) + " argument(s), got " +
                 std::to_string(argument_count);
        return false;
    }
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
            } else if (argument.type == DEJAVU_VALUE_INTEGER && is_string_descriptor(descriptor)) {
                jobject object = lookup_object(argument.integer_value, error);
                if (object == nullptr) {
                    return false;
                }
                value.l = object;
            } else if (argument.type == DEJAVU_VALUE_STRING && is_string_descriptor(descriptor)) {
                jstring text = create_string(env, value_string(argument), error);
                if (text == nullptr) {
                    return false;
                }
                value.l = text;
                converted->local_references.push_back(text);
            } else if (argument.type == DEJAVU_VALUE_INTEGER) {
                jobject object = lookup_object(argument.integer_value, error);
                if (object == nullptr) {
                    return false;
                }
                value.l = object;
            } else {
                *error = prefix + "expects nil, an object handle, or a string";
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
        std::string text;
        if (!read_string(env, static_cast<jstring>(value.l), &text, error)) {
            env->DeleteLocalRef(value.l);
            return false;
        }
        if (!set_owned_string(result, text)) {
            env->DeleteLocalRef(value.l);
            *error = "unable to allocate the returned Java string";
            return false;
        }
        env->DeleteLocalRef(value.l);
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
        return !class_name->empty();
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
    jclass class_object = nullptr;
    jobject receiver = nullptr;
    if (is_static) {
        jlong loader_handle = 0;
        std::string class_name;
        if (!resolve_class_target(value_string(arguments[0]), &loader_handle, &class_name, error)) {
            return false;
        }
        if (loader_handle != 0) {
            jobject loader = lookup_object(loader_handle, error);
            if (loader == nullptr) {
                return false;
            }
            class_object = find_class_with_loader(env, class_name, loader, error);
        } else {
            class_object = find_class(env, class_name, error);
        }
        if (class_object == nullptr) {
            return false;
        }
    } else {
        receiver = lookup_object(arguments[0].integer_value, error);
        if (receiver == nullptr) {
            return false;
        }
        class_object = env->GetObjectClass(receiver);
        if (class_object == nullptr || env->ExceptionCheck()) {
            *error = "unable to resolve the object class";
            env->ExceptionClear();
            return false;
        }
    }
    const jmethodID method =
        is_static ? env->GetStaticMethodID(class_object, method_name.c_str(), signature.c_str())
                  : env->GetMethodID(class_object, method_name.c_str(), signature.c_str());
    if (method == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(class_object);
        *error = "method not found or signature mismatch: " + method_name + signature;
        return false;
    }
    ConvertedArguments converted;
    if (!convert_arguments(env, parameters, arguments + 3, argument_count - 3, &converted, error)) {
        cleanup(&converted, class_object);
        return false;
    }
    jvalue value = {};
    const bool called = call_primitive(env, is_static, class_object, receiver, method,
                                       converted.values, return_type, &value, error);
    if (!called) {
        cleanup(&converted, class_object);
        return false;
    }
    const bool converted_result = convert_return(env, return_type, value, result, error);
    cleanup(&converted, class_object);
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
    jclass class_object = nullptr;
    if (loader_handle != 0) {
        jobject loader = lookup_object(loader_handle, error);
        if (loader == nullptr) {
            return false;
        }
        class_object = find_class_with_loader(env, class_name, loader, error);
    } else {
        class_object = find_class(env, class_name, error);
    }
    if (class_object == nullptr) {
        return false;
    }
    const jmethodID constructor = env->GetMethodID(class_object, "<init>", signature.c_str());
    if (constructor == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(class_object);
        *error = "constructor not found or signature mismatch: " + class_name + signature;
        return false;
    }
    ConvertedArguments converted;
    if (!convert_arguments(env, parameters, arguments + 2, argument_count - 2, &converted, error)) {
        cleanup(&converted, class_object);
        return false;
    }
    jobject instance = env->NewObjectA(class_object, constructor, converted.values.data());
    if (env->ExceptionCheck() || instance == nullptr) {
        *error = describe_exception(env);
        cleanup(&converted, class_object);
        return false;
    }
    const jlong handle = register_object(env, instance, error);
    env->DeleteLocalRef(instance);
    cleanup(&converted, class_object);
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
