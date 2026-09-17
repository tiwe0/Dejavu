do
    if _G.hookx == nil then
        local hookx = {}

        local descriptor_aliases = {
            ["boolean"] = "Z",
            ["byte"] = "B",
            ["char"] = "C",
            ["short"] = "S",
            ["int"] = "I",
            ["long"] = "J",
            ["float"] = "F",
            ["double"] = "D",
            ["void"] = "V",
            ["String"] = "Ljava/lang/String;",
        }
        local c_int_min = -2147483648
        local c_int_max = 2147483647
        local safe_integer_limit = 9007199254740991

        local lua_tointeger = math.tointeger
        if lua_tointeger == nil then
            lua_tointeger = function(value)
                if type(value) == "number" and
                    value >= -safe_integer_limit and
                    value <= safe_integer_limit and
                    value == math.floor(value) then
                    return value
                end
                return nil
            end
        end

        local function fail(message, level)
            error("hookx: " .. message, (level or 1) + 1)
        end

        local function ensure_non_empty_string(label, value, level)
            if type(value) ~= "string" or value == "" then
                fail(label .. " must be a non-empty string", (level or 1) + 1)
            end
            return value
        end

        local function ensure_integer(label, value, level)
            local integer = lua_tointeger(value)
            if integer == nil then
                fail(label .. " must be an integer", (level or 1) + 1)
            end
            return integer
        end

        local function ensure_non_negative_index(value, level)
            local index = ensure_integer("argument index", value, (level or 1) + 1)
            if index < 0 then
                fail("argument index must be non-negative", (level or 1) + 1)
            end
            return index
        end

        local function ensure_arg_index(parsed_signature, index, helper_name, level)
            if parsed_signature.args[index + 1] == nil then
                fail(
                    helper_name .. " argument index " .. index ..
                        " is out of range for signature with " ..
                        #parsed_signature.args .. " parameter(s)",
                    (level or 1) + 1)
            end
            return index
        end

        local function ensure_c_int(label, value, level)
            local integer = ensure_integer(label, value, (level or 1) + 1)
            if integer < c_int_min or integer > c_int_max then
                fail(label .. " must fit in C int range", (level or 1) + 1)
            end
            return integer
        end

        local function ensure_boolean_constant(label, value, level)
            if type(value) == "boolean" then
                return value and 1 or 0
            end
            local integer = lua_tointeger(value)
            if integer == 0 or integer == 1 then
                return integer
            end
            fail(label .. " must be a boolean or 0/1 integer", (level or 1) + 1)
        end

        local function c_string(value)
            ensure_non_empty_string("log message", value, 3)
            return '"' .. value:gsub('[%z\1-\31\\"]', function(character)
                local replacements = {
                    ['"'] = '\\"',
                    ["\\"] = "\\\\",
                    ["\n"] = "\\n",
                    ["\r"] = "\\r",
                    ["\t"] = "\\t",
                    ["\b"] = "\\b",
                    ["\f"] = "\\f",
                    ["\v"] = "\\v",
                    ["\0"] = "\\0",
                }
                return replacements[character] or string.format("\\%03o", string.byte(character))
            end) .. '"'
        end

        local function parse_descriptor(descriptor, index, allow_void, level)
            local code = descriptor:sub(index, index)
            if code == "" then
                fail("truncated JNI descriptor in '" .. descriptor .. "'", (level or 1) + 1)
            end
            if code == "[" then
                local nested, next_index = parse_descriptor(
                    descriptor, index + 1, false, (level or 1) + 1)
                return "[" .. nested, next_index
            end
            if code == "L" then
                local end_index = descriptor:find(";", index, true)
                local name = end_index == nil and "" or descriptor:sub(index + 1, end_index - 1)
                if end_index == nil or name == "" or not name:match("^[%w_$/]+$") then
                    fail(
                        "invalid object descriptor in '" .. descriptor .. "'",
                        (level or 1) + 1)
                end
                return descriptor:sub(index, end_index), end_index + 1
            end
            if code:match("^[ZBCSIJFDV]$") then
                if code == "V" and not allow_void then
                    fail("void is valid only as a return type", (level or 1) + 1)
                end
                return code, index + 1
            end
            fail("invalid JNI descriptor '" .. code .. "' in '" .. descriptor .. "'", (level or 1) + 1)
        end

        local function validate_full_descriptor(descriptor, allow_void, level)
            if descriptor == "" then
                fail("JNI descriptor must be non-empty", (level or 1) + 1)
            end
            local parsed, next_index = parse_descriptor(
                descriptor, 1, allow_void, (level or 1) + 1)
            if next_index <= #descriptor then
                fail(
                    "trailing data in JNI descriptor '" .. descriptor .. "'",
                    (level or 1) + 1)
            end
            return parsed
        end

        local function normalize_type_name(type_name, allow_void, level)
            ensure_non_empty_string("type name", type_name, (level or 1) + 1)
            local alias = descriptor_aliases[type_name]
            if alias ~= nil then
                if alias == "V" and not allow_void then
                    fail("void is valid only as a return type", (level or 1) + 1)
                end
                return alias
            end
            if type_name:find(".", 1, true) ~= nil then
                fail(
                    "unsupported type name '" .. type_name ..
                        "'; use 'String' or a JNI descriptor like 'L" ..
                        type_name:gsub("%.", "/") .. ";'",
                    (level or 1) + 1)
            end
            return validate_full_descriptor(type_name, allow_void, (level or 1) + 1)
        end

        local function parse_method_signature(signature, level)
            ensure_non_empty_string("JNI signature", signature, (level or 1) + 1)
            if signature:sub(1, 1) ~= "(" then
                fail("JNI signature must start with '('", (level or 1) + 1)
            end
            local index = 2
            local args = {}
            while true do
                local code = signature:sub(index, index)
                if code == "" then
                    fail("JNI signature is missing ')'", (level or 1) + 1)
                end
                if code == ")" then
                    index = index + 1
                    break
                end
                local descriptor
                descriptor, index = parse_descriptor(signature, index, false, (level or 1) + 1)
                args[#args + 1] = descriptor
            end
            local ret, next_index = parse_descriptor(
                signature, index, true, (level or 1) + 1)
            if next_index <= #signature then
                fail("JNI signature has trailing data", (level or 1) + 1)
            end
            return {
                args = args,
                ret = ret,
            }
        end

        local function ensure_target(class_name, method_name, signature, level)
            ensure_non_empty_string("class name", class_name, (level or 1) + 1)
            ensure_non_empty_string("method name", method_name, (level or 1) + 1)
            return parse_method_signature(signature, (level or 1) + 1)
        end

        local function ensure_install_available(level)
            if type(hook) ~= "table" or type(hook.install) ~= "function" then
                fail("hook.install is unavailable in this Lua session", (level or 1) + 1)
            end
        end

        local function install_generated(class_name, method_name, signature, source, level)
            ensure_install_available((level or 1) + 1)
            return hook.install(class_name, method_name, signature, source)
        end

        local function require_arg_descriptor(parsed_signature, index, descriptor, helper_name, level)
            local actual = parsed_signature.args[index + 1]
            if actual == nil then
                fail(
                    helper_name .. " argument index " .. index ..
                        " is out of range for signature with " ..
                        #parsed_signature.args .. " parameter(s)",
                    (level or 1) + 1)
            end
            if actual ~= descriptor then
                fail(
                    helper_name .. " requires argument #" .. index .. " to be " ..
                        descriptor .. ", got " .. actual,
                    (level or 1) + 1)
            end
        end

        local function require_return_descriptor(parsed_signature, descriptor, helper_name, level)
            if parsed_signature.ret ~= descriptor then
                fail(
                    helper_name .. " requires return type " .. descriptor ..
                        ", got " .. parsed_signature.ret,
                    (level or 1) + 1)
            end
        end

        local function build_source(lines)
            return table.concat(lines, "\n") .. "\n"
        end

        local unsigned_logger_source = [[
static void hookx_log_unsigned(const char *prefix, unsigned long long value) {
    char buffer[96];
    char reversed[32];
    size_t length = 0;
    unsigned int digits = 0;
    while (prefix[length] != '\0' && length + 1 < sizeof(buffer)) {
        buffer[length] = prefix[length];
        ++length;
    }
    while (1) {
        if (digits == sizeof(reversed)) {
            dejavu_hook_log("hookx.log_string_arg integer log overflow");
            return;
        }
        reversed[digits++] = (char)('0' + (value % 10ull));
        value /= 10ull;
        if (value == 0) {
            break;
        }
    }
    while (digits != 0 && length + 1 < sizeof(buffer)) {
        buffer[length++] = reversed[--digits];
    }
    buffer[length] = '\0';
    dejavu_hook_log(buffer);
}
]]

        local signed_logger_source = [[
static void hookx_log_signed(const char *prefix, long long value) {
    char buffer[96];
    char reversed[32];
    unsigned long long magnitude = (unsigned long long)value;
    size_t length = 0;
    unsigned int digits = 0;
    while (prefix[length] != '\0' && length + 1 < sizeof(buffer)) {
        buffer[length] = prefix[length];
        ++length;
    }
    if (value < 0) {
        if (length + 1 < sizeof(buffer)) {
            buffer[length++] = '-';
        }
        magnitude = 0ull - magnitude;
    }
    while (1) {
        if (digits == sizeof(reversed)) {
            dejavu_hook_log("hookx.log_result_int integer log overflow");
            return;
        }
        reversed[digits++] = (char)('0' + (magnitude % 10ull));
        magnitude /= 10ull;
        if (magnitude == 0) {
            break;
        }
    }
    while (digits != 0 && length + 1 < sizeof(buffer)) {
        buffer[length++] = reversed[--digits];
    }
    buffer[length] = '\0';
    dejavu_hook_log(buffer);
}
]]

        function hookx.sig(spec)
            if type(spec) ~= "table" then
                fail("hookx.sig expects a table", 2)
            end
            local args = spec.args
            if args == nil then
                args = {}
            elseif type(args) ~= "table" then
                fail("hookx.sig args must be a table", 2)
            end
            local ret = spec.ret
            if ret == nil then
                fail("hookx.sig ret is required", 2)
            end
            local parts = {"("}
            for index = 1, #args do
                parts[#parts + 1] = normalize_type_name(args[index], false, 2)
            end
            parts[#parts + 1] = ")"
            parts[#parts + 1] = normalize_type_name(ret, true, 2)
            return table.concat(parts)
        end

        function hookx.trace(class_name, method_name, signature, opts)
            ensure_target(class_name, method_name, signature, 2)
            if opts ~= nil and type(opts) ~= "table" then
                fail("trace options must be a table", 2)
            end
            local message = "hookx.trace " .. class_name .. "#" .. method_name .. signature
            if opts ~= nil and opts.message ~= nil then
                message = message .. " :: " ..
                    ensure_non_empty_string("trace message", opts.message, 2)
            end
            return install_generated(class_name, method_name, signature, build_source({
                "int before_hook(dejavu_hook_context *context) {",
                "    (void)context;",
                "    dejavu_hook_log(" .. c_string(message) .. ");",
                "    return DEJAVU_HOOK_OK;",
                "}",
            }), 2)
        end

        function hookx.replace_arg_int(class_name, method_name, signature, index, new_value)
            local parsed_signature = ensure_target(class_name, method_name, signature, 2)
            local arg_index = ensure_arg_index(
                parsed_signature,
                ensure_non_negative_index(index, 2),
                "hookx.replace_arg_int",
                2)
            local replacement = ensure_c_int("new_value", new_value, 2)
            require_arg_descriptor(parsed_signature, arg_index, "I", "hookx.replace_arg_int", 2)
            return install_generated(class_name, method_name, signature, build_source({
                "int before_hook(dejavu_hook_context *context) {",
                "    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_int(context, " ..
                    arg_index .. ", " .. replacement .. "));",
                "    return DEJAVU_HOOK_OK;",
                "}",
            }), 2)
        end

        function hookx.force_return_int(class_name, method_name, signature, value)
            local parsed_signature = ensure_target(class_name, method_name, signature, 2)
            local result = ensure_c_int("value", value, 2)
            require_return_descriptor(parsed_signature, "I", "hookx.force_return_int", 2)
            return install_generated(class_name, method_name, signature, build_source({
                "int before_hook(dejavu_hook_context *context) {",
                "    return dejavu_hook_return_int(context, " .. result .. ");",
                "}",
            }), 2)
        end

        function hookx.force_return_bool(class_name, method_name, signature, value)
            local parsed_signature = ensure_target(class_name, method_name, signature, 2)
            local result = ensure_boolean_constant("value", value, 2)
            require_return_descriptor(parsed_signature, "Z", "hookx.force_return_bool", 2)
            return install_generated(class_name, method_name, signature, build_source({
                "int before_hook(dejavu_hook_context *context) {",
                "    return dejavu_hook_return_boolean(context, " .. result .. ");",
                "}",
            }), 2)
        end

        function hookx.force_return_void(class_name, method_name, signature)
            local parsed_signature = ensure_target(class_name, method_name, signature, 2)
            require_return_descriptor(parsed_signature, "V", "hookx.force_return_void", 2)
            return install_generated(class_name, method_name, signature, build_source({
                "int before_hook(dejavu_hook_context *context) {",
                "    return dejavu_hook_return_void(context);",
                "}",
            }), 2)
        end

        function hookx.log_string_arg(class_name, method_name, signature, index)
            local parsed_signature = ensure_target(class_name, method_name, signature, 2)
            local arg_index = ensure_arg_index(
                parsed_signature,
                ensure_non_negative_index(index, 2),
                "hookx.log_string_arg",
                2)
            require_arg_descriptor(
                parsed_signature, arg_index, "Ljava/lang/String;", "hookx.log_string_arg", 2)
            return install_generated(class_name, method_name, signature, build_source({
                unsigned_logger_source,
                "int before_hook(dejavu_hook_context *context) {",
                "    char buffer[256] = {0};",
                "    size_t size = 0;",
                "    int status = dejavu_hook_get_arg_string_mutf8(",
                "        context, " .. arg_index .. ", buffer, sizeof(buffer), &size);",
                "    if (status == DEJAVU_HOOK_OK) {",
                "        dejavu_hook_log(\"hookx.log_string_arg value:\");",
                "        dejavu_hook_log(buffer);",
                "        return DEJAVU_HOOK_OK;",
                "    }",
                "    if (status == DEJAVU_HOOK_ERROR_RANGE) {",
                "        hookx_log_unsigned(" ..
                    c_string("hookx.log_string_arg required bytes=") ..
                    ", (unsigned long long)size);",
                "        return DEJAVU_HOOK_OK;",
                "    }",
                "    return status;",
                "}",
            }), 2)
        end

        function hookx.log_result_int(class_name, method_name, signature)
            local parsed_signature = ensure_target(class_name, method_name, signature, 2)
            require_return_descriptor(parsed_signature, "I", "hookx.log_result_int", 2)
            return install_generated(class_name, method_name, signature, build_source({
                signed_logger_source,
                "int after_hook(dejavu_hook_context *context) {",
                "    int value = 0;",
                "    DEJAVU_HOOK_TRY(dejavu_hook_get_result_int(context, &value));",
                "    hookx_log_signed(" ..
                    c_string("hookx.log_result_int value=") ..
                    ", (long long)value);",
                "    return DEJAVU_HOOK_OK;",
                "}",
            }), 2)
        end

        _G.hookx = hookx
    end
end
