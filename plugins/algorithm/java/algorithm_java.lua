-- Java crypto/algorithm tracer using the existing Java hook API.
-- Load with: ./dejavu exec -p <pkg> plugins/algorithm/java/algorithm_java.lua
-- Logs to the DejavuHook logcat tag; inspect with ./dejavu log -p <pkg>.
local hook_source = [[
static void algorithm_copy(char *buffer, unsigned long capacity, const char *prefix,
                           const char *data, unsigned long size) {
    unsigned long offset = 0;
    while (prefix[offset] != 0 && offset + 1 < capacity) {
        buffer[offset] = prefix[offset];
        ++offset;
    }
    unsigned long index = 0;
    while (index < size && offset + 1 < capacity) {
        buffer[offset++] = data[index++];
    }
    buffer[offset] = 0;
}

static int algorithm_log_algorithm(dejavu_hook_context *context, const char *class_name,
                                   const char *prefix) {
    dejavu_hook_value result = {0};
    void *receiver = dejavu_hook_this_object(context);
    if (receiver == 0) {
        return DEJAVU_HOOK_OK;
    }
    int status = dejavu_hook_call(context, 0, receiver, class_name, "getAlgorithm",
                                  "()Ljava/lang/String;", 0, 0, &result);
    if (status == DEJAVU_HOOK_OK && result.kind == DEJAVU_HOOK_VALUE_STRING) {
        char line[256];
        algorithm_copy(line, sizeof(line), prefix, result.value.string_value.data,
                       result.value.string_value.size);
        dejavu_hook_log(line);
    }
    return DEJAVU_HOOK_OK;
}

static int algorithm_log_byte_array(dejavu_hook_context *context, void *object,
                                    const char *prefix) {
    if (object == 0) {
        return DEJAVU_HOOK_OK;
    }
    dejavu_hook_value argument = {0};
    argument.kind = DEJAVU_HOOK_VALUE_OBJECT;
    argument.value.object_value = object;
    dejavu_hook_value result = {0};
    int status = dejavu_hook_call(context, DEJAVU_HOOK_CALL_STATIC, 0,
                                  "java/util/Arrays", "toString",
                                  "([B)Ljava/lang/String;", &argument, 1, &result);
    if (status == DEJAVU_HOOK_OK && result.kind == DEJAVU_HOOK_VALUE_STRING) {
        char line[2048];
        algorithm_copy(line, sizeof(line), prefix, result.value.string_value.data,
                       result.value.string_value.size);
        dejavu_hook_log(line);
    }
    return DEJAVU_HOOK_OK;
}

int before_hook(dejavu_hook_context *context) {
    DEJAVU_HOOK_TRY(algorithm_log_algorithm(context, "__CLASS__", "ALG __LABEL__ algo="));
    void *argument = 0;
    if (dejavu_hook_get_arg_object(context, 0, &argument) == DEJAVU_HOOK_OK) {
        DEJAVU_HOOK_TRY(algorithm_log_byte_array(context, argument, "ALG __LABEL__ in="));
    }
    return DEJAVU_HOOK_OK;
}

int after_hook(dejavu_hook_context *context) {
    void *result = 0;
    if (dejavu_hook_get_result_object(context, &result) == DEJAVU_HOOK_OK) {
        DEJAVU_HOOK_TRY(algorithm_log_byte_array(context, result, "ALG __LABEL__ out="));
    }
    return DEJAVU_HOOK_OK;
}
]]

local function install(class_name, method, signature, label)
    local source = hook_source
        :gsub("__CLASS__", class_name:gsub("%.", "/"))
        :gsub("__LABEL__", label)
    local ok, result = pcall(hook.install, class_name, method, signature, source)
    if ok then
        hook.log("algorithm java hook installed " .. label .. " id=" .. tostring(result))
        return result
    end
    hook.log("algorithm java hook failed " .. label .. ": " .. tostring(result))
    return nil
end

local ids = {}
local function add(id)
    if id ~= nil then
        ids[#ids + 1] = tostring(id)
    end
end

add(install("java.security.MessageDigest", "digest", "([B)[B", "MessageDigest.digest"))
add(install("java.security.MessageDigest", "update", "([B)V", "MessageDigest.update"))
add(install("javax.crypto.Mac", "doFinal", "([B)[B", "Mac.doFinal"))
add(install("javax.crypto.Mac", "update", "([B)V", "Mac.update"))
add(install("javax.crypto.Cipher", "doFinal", "([B)[B", "Cipher.doFinal"))
add(install("javax.crypto.Cipher", "update", "([B)[B", "Cipher.update"))
return table.concat(ids, ",")
