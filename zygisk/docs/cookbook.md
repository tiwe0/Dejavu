# Dejavu `hookx` Cookbook

`hookx` 是构建在现有 `hook.install(class, method, jni_signature, c_source)` 之上的纯
Lua 高层封装。它不修改 Hook C ABI v1，也不改变 `hook.install/remove/uninstall/clear/list/log`
的语义；只是帮你生成一段符合 ABI v1 的 TinyCC/GNU C hook source。

> 所有下面的手写 C 示例都面向 TinyCC 接受的 GNU C 方言（与
> `hook-c-api.md` 中 `-std=gnu17` 的说明一致）。
>
> 如果目标进程已经执行过模块里拼装后的 `config/init.lua`，`hookx` 会自动存在于
> 共享 Lua 会话中；若你是在一个全新的独立脚本里显式兜底，可以先加：
>
> ```lua
> if type(hookx) ~= "table" then
>     assert(loadfile("/data/adb/modules/dejavu_zygisk/config/hookx.lua"))()
> end
> ```

## 1. 追踪某方法的每次调用

**场景**：只想在进入方法时打一条日志。

### `hookx` 一行写法

```lua
local id = hookx.trace("android.app.Activity", "onResume", "()V")
hook.log("installed hook " .. id)
```

### 等价底层写法

```lua
local source = [[
int before_hook(dejavu_hook_context *context) {
    (void)context;
    dejavu_hook_log("hookx.trace android.app.Activity#onResume()V");
    return DEJAVU_HOOK_OK;
}
]]
local id = hook.install("android.app.Activity", "onResume", "()V", source)
hook.log("installed hook " .. id)
```

## 2. 绕过返回 boolean 的校验方法

**场景**：强制 `()Z` 方法始终返回 `true`，常用于快速绕过 root/完整性检查。

### `hookx` 一行写法

```lua
hookx.force_return_bool("com.example.Guard", "isDeviceSafe", "()Z", true)
```

### 等价底层写法

```lua
local source = [[
int before_hook(dejavu_hook_context *context) {
    return dejavu_hook_return_boolean(context, 1);
}
]]
hook.install("com.example.Guard", "isDeviceSafe", "()Z", source)
```

## 3. 篡改一个 int 参数

**场景**：把第 0 个 `int` 参数强制改成常量。

### `hookx` 一行写法

```lua
hookx.replace_arg_int("com.example.Target", "compute", "(I)I", 0, 42)
```

### 等价底层写法

```lua
local source = [[
int before_hook(dejavu_hook_context *context) {
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_int(context, 0, 42));
    return DEJAVU_HOOK_OK;
}
]]
hook.install("com.example.Target", "compute", "(I)I", source)
```

## 4. Dump 一个 String 参数

**场景**：把第 0 个 `String` 参数写入 `DejavuHook` 日志；如果超出固定缓冲区，记录所需字节数。

### `hookx` 一行写法

```lua
hookx.log_string_arg(
    "com.example.Api",
    "submit",
    "(Ljava/lang/String;)V",
    0)
```

### 等价底层写法

```lua
local source = [[
static void hookx_log_unsigned(const char *prefix, unsigned long long value) {
    char buffer[96];
    char reversed[32];
    size_t length = 0;
    unsigned int digits = 0;
    while (prefix[length] != '\0' && length + 1 < sizeof(buffer)) {
        buffer[length] = prefix[length];
        ++length;
    }
    do {
        reversed[digits++] = (char)('0' + (value % 10ull));
        value /= 10ull;
    } while (value != 0 && digits < sizeof(reversed));
    while (digits != 0 && length + 1 < sizeof(buffer)) {
        buffer[length++] = reversed[--digits];
    }
    buffer[length] = '\0';
    dejavu_hook_log(buffer);
}

int before_hook(dejavu_hook_context *context) {
    char buffer[256] = {0};
    size_t size = 0;
    int status = dejavu_hook_get_arg_string_mutf8(
        context, 0, buffer, sizeof(buffer), &size);
    if (status == DEJAVU_HOOK_OK) {
        dejavu_hook_log("hookx.log_string_arg value:");
        dejavu_hook_log(buffer);
        return DEJAVU_HOOK_OK;
    }
    if (status == DEJAVU_HOOK_ERROR_RANGE) {
        hookx_log_unsigned("hookx.log_string_arg required bytes=", (unsigned long long)size);
        return DEJAVU_HOOK_OK;
    }
    return status;
}
]]
hook.install("com.example.Api", "submit", "(Ljava/lang/String;)V", source)
```

## 5. 记录一个 int 返回值

**场景**：在 `after_hook` 里读取 `int` 返回值并写日志。

### `hookx` 一行写法

```lua
hookx.log_result_int("com.example.Target", "compute", "(I)I")
```

### 等价底层写法

```lua
local source = [[
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
        if (length + 1 < sizeof(buffer))
            buffer[length++] = '-';
        magnitude = 0ull - magnitude;
    }
    do {
        reversed[digits++] = (char)('0' + (magnitude % 10ull));
        magnitude /= 10ull;
    } while (magnitude != 0 && digits < sizeof(reversed));
    while (digits != 0 && length + 1 < sizeof(buffer)) {
        buffer[length++] = reversed[--digits];
    }
    buffer[length] = '\0';
    dejavu_hook_log(buffer);
}

int after_hook(dejavu_hook_context *context) {
    int value = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_result_int(context, &value));
    hookx_log_signed("hookx.log_result_int value=", (long long)value);
    return DEJAVU_HOOK_OK;
}
]]
hook.install("com.example.Target", "compute", "(I)I", source)
```

## 6. 用 `hookx.sig` 构造带对象参数的 descriptor

**场景**：不想手写 `(Ljava/lang/String;Landroid/content/Context;)V` 这种 JNI descriptor。

### `hookx.sig` 写法

```lua
local sig = hookx.sig{
    args = {"String", "Landroid/content/Context;"},
    ret = "void",
}
hook.log(sig)
return sig
```

返回值就是：

```text
(Ljava/lang/String;Landroid/content/Context;)V
```

### 和手写 descriptor 等价的安装方式

```lua
local sig = hookx.sig{
    args = {"String", "Landroid/content/Context;"},
    ret = "void",
}
hookx.trace("com.example.Api", "submit", sig)
hookx.trace("com.example.Api", "submit", "(Ljava/lang/String;Landroid/content/Context;)V")
```

上面的 Lua 片段既可以保存成 `.lua` 文件后通过 `dejavuctl path/to/file.lua bin.mt.plus`
执行，也可以根据需要改写成 `dejavuctl -e '...'` 的单行形式；如果是全新的独立
脚本且当前会话还没有执行过模块里的 `init.lua`，先加上前面的兜底加载片段。

## 设备端快速验证

仓库自带一个可直接下发的示例脚本：

```sh
./zygisk/scripts/dejavuctl \
  /home/runner/work/Dejavu/Dejavu/zygisk/tests/device/hookx-demo.lua \
  bin.mt.plus
```

它会：

1. 用 `hookx.trace` 安装 `android.app.Activity.onResume` hook；
2. 输出安装得到的 hook id；
3. 输出 `hook.list()`，方便快速确认 helper 已经在共享 Lua 会话中可用。
