# Dejavu Zygisk 使用手册

本文是 Dejavu Zygisk 模块的完整使用手册，覆盖 Android 15 arm64 设备
上的安装、作用域配置、Lua 控制、TinyCC native hook、热更新、RPC CLI
和常见故障处理。

Dejavu 的边界很简单：Lua 只负责管理 hook，真正进入目标进程的
`before_hook` / `after_hook` 由 TinyCC 编译为 native C 执行。Lua 管理请求
按顺序执行；native callback 可以在多个 Java 线程上并发运行。

## 1. 运行条件

- Android 15 或更高版本（API 35+）。
- `arm64-v8a` 设备。
- Magisk 27+ 且启用 Zygisk，或者 APatch + 兼容的 Zygisk provider。
- 设备已 root，主机安装 `adb`、Python 3 和 Android NDK。
- 构建 Dejavu Zygisk 前，根目录的 `libdejavu.so` 已经构建完成。

APatch 本身不实现 Zygisk。APatch 用户需要额外安装并启用 ZygiskNext
之类的 provider。APatch Manager 可以直接显示本模块的 WebUI；WebUI
不是 Agent 的运行时依赖，没有 APatch WebUI 时仍可使用配置文件和 CLI。

## 2. 模块组成

安装包是一个标准 Magisk/APatch 模块，主要文件如下：

```text
module.prop
customize.sh
zygisk/arm64-v8a.so                 # Zygisk Loader
lib/arm64-v8a/libdejavu_agent.so    # LSPlant/Dobby/Lua/TinyCC Agent
lib/arm64-v8a/libdejavu.so          # Dejavu runtime
config/targets.txt                   # 进程作用域
config/init.lua                      # Agent 启动时执行一次
webroot/index.html                   # APatch Manager WebUI
```

Loader 会在每个 Zygote 子进程中读取 `config/targets.txt`。只有精确匹配
的进程会加载 Agent，其他进程会请求卸载当前模块库。Agent 启动后通过
两个 abstract Unix domain socket 提供持久管理通道和 CLI 通道。socket
不创建文件节点，也不需要目标应用的网络权限。

## 3. 构建模块

在仓库根目录执行：

```sh
# 先构建 Agent 需要的 libdejavu.so
make android-arm64

# 运行 host tests 并构建 Android arm64 模块
make -C zygisk test
make -C zygisk package
```

产物为：

```text
zygisk/dist/dejavu-zygisk-v0.1.0-arm64.zip
```

`package` 会自动查找 NDK 的 `llvm-strip`，并对 Loader、Agent 和
`libdejavu.so` 执行 `--strip-debug`。未剥离的文件仍保存在 `out/`，用于
native 符号化调试。找不到工具时可以显式指定：

```sh
LLVM_STRIP="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" \
  make -C zygisk package
```

## 4. 安装和首次验证

将 ZIP 复制到设备：

```sh
adb push zygisk/dist/dejavu-zygisk-v0.1.0-arm64.zip /sdcard/Download/
```

在 Magisk Manager 或 APatch Manager 中选择该 ZIP 安装，然后重启设备。
首次安装、Loader 改动、Zygisk provider 改动都需要重启 Zygote。

重启后先确认模块和作用域：

```sh
adb shell su -c 'nsenter -t 1 -m -- cat /data/adb/modules/dejavu_zygisk/config/targets.txt'
adb shell pidof bin.mt.plus
```

查看启动日志：

```sh
adb logcat -s DejavuZygisk:V DejavuAgent:V DejavuHook:V
```

目标进程正常加载时应看到类似日志：

```text
target selected: process=bin.mt.plus
opening Agent for bin.mt.plus
agent active for bin.mt.plus
persistent Lua control initialized
abstract-socket RPC control connected
```

## 5. 配置作用域

### 5.1 直接编辑 `targets.txt`

`config/targets.txt` 每行写一个精确的 Android 进程名，空行和 `#` 注释
会被忽略：

```text
# MT Manager 主进程
bin.mt.plus

# 只有确实需要时才加入子进程
bin.mt.plus:worker
```

这里匹配的是进程名，不是简单的包名。可以使用以下命令查看正在运行的
进程：

```sh
adb shell ps -A | grep bin.mt.plus
adb shell pidof bin.mt.plus
```

修改后需要重启目标进程；Loader 配置变化通常直接重启 Zygote 最可靠：

```sh
adb shell am force-stop bin.mt.plus
adb shell monkey -p bin.mt.plus 1
```

### 5.2 使用 APatch WebUI

在 APatch Manager 的模块列表中打开 `Dejavu Zygisk`，点击 `WebUI` 或
`打开`。页面会：

1. 通过 APatch/KernelSU bridge 获取已安装应用和图标；
2. 读取 `/data/adb/modules/dejavu_zygisk/config/targets.txt`；
3. 按应用名称或包名搜索并勾选作用域；
4. 使用临时文件、权限设置和 `mv` 原子替换配置；
5. 可选地对选中包执行 `am force-stop`。

保存只影响后续启动的进程。页面提示保存后重启已运行的应用。对于声明
了 `android:process="包名:xxx"` 的多进程应用，WebUI 默认只写入包名，
额外进程需要手工加入 `targets.txt`。

如果页面提示 `WebUI bridge is unavailable`，说明当前管理器没有提供
`window.ksu` bridge。此时不影响 native Agent，改用文件编辑或下面的
CLI 即可。

## 6. 不重启设备的开发循环

Loader 和 Zygote 已经正常工作后，日常修改只需要替换 Agent 和
`init.lua`，不需要重启整台设备：

```sh
ADB_SERIAL=c44d68aa make -C zygisk deploy-agent
```

指定其他进程和启动 Activity：

```sh
ADB_SERIAL=c44d68aa \
PROCESS=bin.mt.plus:worker \
ACTIVITY=bin.mt.plus/.MainLightIcon \
make -C zygisk deploy-agent
```

脚本会先复制到临时文件，再原子替换模块中的 Agent 和 `init.lua`，然后
停止并重新启动目标进程。已经运行的进程不会自动映射新 Agent；只需重启
目标进程即可。Loader、module.prop、Zygisk provider 或模块安装状态变化
仍然需要 Zygote 重启。

## 7. Lua 管理 API

`config/init.lua` 在 `Application.attach(Context)` 之后执行一次，此时目标
应用的 class loader 已可用。通过 CLI 发送的 Lua 也运行在同一个管理会话
中。

```lua
local id = hook.install(class_name, method_name, jni_signature, c_source)
local text = hook.list()
local changed = hook.remove(id)
local removed = hook.uninstall(id)
local count = hook.clear()
hook.log("message")
```

### 7.1 安装一个最小 hook

下面的 hook 只在日志中记录 `Activity.onResume`：

```lua
local source = [[
int before_hook(dejavu_hook_context *context) {
    (void)context;
    dejavu_hook_log("Activity.onResume entered");
    return DEJAVU_HOOK_OK;
}
]]

local id = hook.install(
    "android.app.Activity",
    "onResume",
    "()V",
    source)

hook.log("installed hook " .. id)
hook.log(hook.list())
```

`hook.install` 返回正整数 ID。C source 至少要定义 `before_hook` 或
`after_hook` 其中一个。类名使用 Java 点号形式，方法签名使用 JNI
descriptor。

### 7.2 管理 hook 生命周期

```lua
-- 快速逻辑禁用：保留 LSPlant 和 TinyCC 资源
assert(hook.remove(id))

-- 重新安装一个新 hook 时使用新的 ID
local id2 = hook.install("android.app.Activity", "onResume", "()V", source)

-- 物理卸载：恢复 Java 方法，等待 in-flight callback，并释放资源
assert(hook.uninstall(id2))
assert(not hook.uninstall(id2))

-- 一次性逻辑禁用全部用户 hook
local changed = hook.clear()
```

- `hook.list()` 返回每个用户 hook 的一行：`id<TAB>active|disabled<TAB>...`。
- `remove` 和 `clear` 只改变逻辑状态，适合故障排查和快速回滚。
- `uninstall` 才会请求 LSPlant 恢复目标方法并释放编译模块。
- 内部 bootstrap hook 不能通过这些 API 卸载。

## 8. TinyCC Hook C ABI

每个 C source 都会自动注入 ABI 头。入口固定为：

```c
int before_hook(dejavu_hook_context *context);
int after_hook(dejavu_hook_context *context);
```

成功返回 `DEJAVU_HOOK_OK`（0）。推荐所有 helper 调用都使用：

```c
DEJAVU_HOOK_TRY(expression);
```

该宏在 helper 失败时立即返回错误码。完整声明和所有符号见
[hook-c-api.md](hook-c-api.md)。

### 8.1 参数和返回值改写

参数索引从 0 开始，不包含 instance receiver。类型必须和 JNI descriptor
匹配：整数使用 `*_arg_int` 等 typed helper，浮点使用 `*_arg_float` 或
`*_arg_double`，对象使用 `*_arg_object`。

```lua
local source = [[
int before_hook(dejavu_hook_context *context) {
    int value = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_int(context, 0, &value));
    return dejavu_hook_set_arg_int(context, 0, value + 1);
}

int after_hook(dejavu_hook_context *context) {
    int result = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_result_int(context, &result));
    return dejavu_hook_set_result_int(context, result + 1);
}
]]

local id = hook.install(
    "com.example.Target",
    "compute",
    "(I)I",
    source)
```

调用顺序是：复制参数、执行 before、调用原方法、执行 after、返回最终
结果。before 修改的是私有参数副本，不会污染其他调用线程。

### 8.2 提前返回

在 before 阶段设置 result 会跳过原始 Java 方法；after 仍然可以读取或
替换这个提前返回值：

```c
int before_hook(dejavu_hook_context *context) {
    int value = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_int(context, 0, &value));
    if (value < 0) {
        return dejavu_hook_return_int(context, 0);
    }
    return DEJAVU_HOOK_OK;
}
```

如果 before 返回错误，参数改写会被丢弃，原方法使用原始参数继续执行，
after 不执行。如果 after 返回错误，保留进入 after 前的 result。

### 8.3 String 读取和创建

String helper 使用 JNI Modified UTF-8。读取时由调用者提供缓冲区；`size`
是不含 C 结尾 NUL 的字节数：

```c
int before_hook(dejavu_hook_context *context) {
    char text[128];
    size_t size = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_string_mutf8(
        context, 0, text, sizeof(text), &size));

    if (size == 3 && text[0] == 'b' && text[1] == 'a' && text[2] == 'd') {
        DEJAVU_HOOK_TRY(dejavu_hook_set_arg_string_mutf8(
            context, 0, "good", 4));
    }
    return DEJAVU_HOOK_OK;
}
```

`dejavu_hook_new_string_mutf8` 会复制输入并返回当前 callback 有效的借用
JNI local reference：

```c
void *created = 0;
DEJAVU_HOOK_TRY(dejavu_hook_new_string_mutf8(
    context, "created", 7, &created));
DEJAVU_HOOK_TRY(dejavu_hook_set_result_object(context, created));
```

缓冲区容量不足返回 `DEJAVU_HOOK_ERROR_RANGE`，并通过 `size` 报告所需
字节数。输入缓冲区不会转移所有权。

### 8.4 对象类型检查

`dejavu_hook_is_instance_of` 接受 JNI 内部类名或常见点号类名，返回布尔
结果，不返回 class reference：

```c
int before_hook(dejavu_hook_context *context) {
    void *value = 0;
    int is_string = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_object(context, 0, &value));
    if (value == 0)
        return DEJAVU_HOOK_OK;
    DEJAVU_HOOK_TRY(dejavu_hook_is_instance_of(
        context, value, "java.lang.String", &is_string));
    return is_string ? DEJAVU_HOOK_OK : DEJAVU_HOOK_ERROR_TYPE;
}
```

空引用永远不是任何类型的 instance。类查找失败或 JNI 异常会转换为
`DEJAVU_HOOK_ERROR_JNI`，并清除 pending exception。

### 8.5 受控 Java 方法调用

`dejavu_hook_call` 使用完整 JNI descriptor 和固定标签参数，不使用 C
varargs。下面调用一个静态的 `(I)I` 方法：

```c
int before_hook(dejavu_hook_context *context) {
    dejavu_hook_value argument;
    argument.kind = DEJAVU_HOOK_VALUE_I64;
    argument.value.i64_value = 4;

    dejavu_hook_value result;
    result.kind = DEJAVU_HOOK_VALUE_VOID;

    DEJAVU_HOOK_TRY(dejavu_hook_call(
        context,
        DEJAVU_HOOK_CALL_STATIC,
        0,
        "com.example.Target",
        "helper",
        "(I)I",
        &argument,
        1,
        &result));

    if (result.kind != DEJAVU_HOOK_VALUE_I64)
        return DEJAVU_HOOK_ERROR_TYPE;
    return result.value.i64_value == 7
        ? DEJAVU_HOOK_OK
        : DEJAVU_HOOK_ERROR_ARGUMENT;
}
```

instance 调用时将 `flags` 设为 0，并传入当前 callback 借用的 receiver。
调用是同步的，只能在当前 callback 线程完成，不能保存 receiver、参数或
结果到后台线程。

### 8.6 void 方法

void 方法没有结果 getter，但可以在 before 中提前返回 void：

```c
int before_hook(dejavu_hook_context *context) {
    dejavu_hook_log("skip original void method");
    return dejavu_hook_return_void(context);
}
```

## 9. 引用、异常和并发规则

这些规则是 ABI 的一部分：

- `dejavu_hook_context *` 只在当前 before/after 调用期间有效。
- 参数、receiver、result 和 helper 返回的对象都是借用 JNI local reference。
- 引用只能在当前 callback、当前线程使用，不能保存或传给其他线程。
- C source 没有 `JNIEnv`，也没有手工 `DeleteLocalRef`；runtime 在 dispatch
  结束时清理临时引用。
- 原 Java 方法抛出的异常不会被 Dejavu 清除，after 会跳过，调用方仍会
  观察到原始异常的 reflective wrapper。
- helper 产生的 JNI 异常会被清除并返回 `DEJAVU_HOOK_ERROR_JNI`，不会把
  偶然的 pending exception 留给原方法。
- native `SIGSEGV`、错误函数指针、栈破坏和死循环无法转换成 Lua 错误，
  可能直接终止目标进程。C callback 是受信任代码。
- native callbacks 可以在多个 Java 线程并发执行；共享状态必须使用
  原子操作或用户自己的同步方案。
- 同一线程递归进入已 hook 方法时，会绕过 Dejavu callback 并调用原方法。

当前 hook 表使用 copy-on-write snapshot 加 in-flight drain。`remove`/
`clear` 是逻辑状态变化，`uninstall` 才会等待 callback 并做物理清理；这
是轻量方案，不承诺通用 RCU 的全部语义。

## 10. `dejavuctl` CLI 和 RPC

CLI 会发现目标 PID 和 endpoint，创建临时 `adb forward`，完成 token 认证，
执行一段 Lua 后自动移除 forward。命令在主机执行：

```sh
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/dejavuctl \
  -p bin.mt.plus \
  -e 'return 6 * 7'
# 42
```

也可以发送 Lua 文件：

```sh
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/dejavuctl \
  zygisk/tests/device/live-control-smoke.lua \
  bin.mt.plus
```

常用选项：

```text
-p, --process PROCESS   目标进程，默认 bin.mt.plus
-s, --serial SERIAL     adb serial，也可使用 ADB_SERIAL
--wait SECONDS          等待 Agent endpoint，默认 5 秒
--timeout SECONDS       socket 超时，默认 10 秒
--reconnect             重新连接并验证同一个 PID 的 Agent 通道
```

CLI 支持 `nil`、布尔、整数、浮点数和字符串结果。Lua 错误、目标不可用
和协议错误写入 stderr，并返回非零状态。Lua 状态本身是单线程的，因此
主机 CLI 调用使用 advisory lock 串行化。

Agent 通道断开时会以 100 ms 到 5 s 的指数退避自动重连。只要 PID 没有
改变，Lua 状态和已安装 hook 会保留：

```sh
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/dejavuctl --reconnect -p bin.mt.plus
```

旧的文件控制路径仍可用于兼容性和故障隔离：

```sh
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/push-control.sh \
  zygisk/tests/device/live-control-smoke.lua \
  bin.mt.plus
```

## 11. 从零开始的 MT Manager 示例

下面流程假设设备序列号为 `c44d68aa`，目标进程为 `bin.mt.plus`：

```sh
# 1. 构建和打包
make android-arm64
make -C zygisk test
make -C zygisk package

# 2. 推送安装包，之后在 APatch Manager 中安装并重启
adb -s c44d68aa push \
  zygisk/dist/dejavu-zygisk-v0.1.0-arm64.zip \
  /sdcard/Download/

# 3. 确认作用域并启动 MT Manager
adb -s c44d68aa shell su -c \
  'nsenter -t 1 -m -- cat /data/adb/modules/dejavu_zygisk/config/targets.txt'
adb -s c44d68aa shell monkey -p bin.mt.plus 1

# 4. 发送最小 live control
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/dejavuctl -p bin.mt.plus \
  -e 'hook.log("MT live control connected"); return hook.list()'

# 5. 检查日志
adb -s c44d68aa logcat -d -s DejavuZygisk:V DejavuAgent:V DejavuHook:V
```

仓库中的可复用测试脚本：

```text
zygisk/tests/device/mt-manager-init.lua       # Activity.onResume 示例
zygisk/tests/device/live-control-smoke.lua    # CLI 连通性
zygisk/tests/device/mt-manager-stress.lua     # 并发、helper、remove、uninstall
```

## 12. 验证和排错

每次源码或文档改动后至少运行：

```sh
make -C zygisk test
git diff --check
```

设备 smoke/stress：

```sh
ADB_SERIAL=c44d68aa make -C zygisk deploy-agent
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/dejavuctl -p bin.mt.plus \
  zygisk/tests/device/mt-manager-stress.lua
```

测试失败时检查 `DejavuAgent`、`DejavuZygisk` 和 `DejavuHook` 日志标签。

### 没有 Agent 日志

1. 确认 API >= 35、设备为 arm64。
2. 确认 APatch + ZygiskNext 或 Magisk + Zygisk 已启用。
3. 确认 `targets.txt` 使用的是实际进程名。
4. 首次安装或 Loader 改动后重启 Zygote。
5. 查看 `DejavuZygisk` 的 `target skipped`、`target selected` 日志。

### `target process is not running`

先启动目标应用，再确认：

```sh
adb shell pidof bin.mt.plus
adb shell ps -A | grep bin.mt.plus
```

### `RPC endpoint is not ready`

Agent 可能还没有执行到 `Application.attach`，或者目标进程是在 Agent
更新前启动的。重新 force-stop 目标应用，等待几秒后重试，并检查
`DejavuAgent` 日志。

### Lua 或 C 编译失败

- 检查类名、方法名和 JNI descriptor 是否对应。
- C source 必须定义至少一个 callback。
- 不要包含目标进程中不存在的头文件或未导出的外部符号。
- 每个 helper 都检查返回码；优先使用 `DEJAVU_HOOK_TRY`。

### 目标应用崩溃

先通过 CLI 逻辑禁用 hook：

```sh
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/dejavuctl -p bin.mt.plus \
  -e 'return hook.clear()'
```

然后检查参数类型、对象引用生命周期、递归调用和外部符号声明。错误
函数指针、越界内存和阻塞 callback 无法由 Lua 层恢复。

### APatch WebUI 打不开

确认 APatch Manager 的模块卡片显示 `WebUI`，并确认模块目录存在：

```sh
adb shell su -c 'nsenter -t 1 -m -- \
  ls -l /data/adb/modules/dejavu_zygisk/webroot/index.html'
```

若 bridge 不可用，直接编辑 `config/targets.txt` 或使用 `dejavuctl`；这
只影响配置界面，不影响 Zygisk Loader 和 Agent。

## 13. 设计边界

当前版本刻意保持小巧：

- 不在 Lua 中执行并行 callback。
- 不提供 native fault recovery、callback 强制超时或跨线程 JNI handle。
- 不把 raw `JNIEnv`、`jmethodID` 或 reflective `Method` 暴露给 TinyCC C。
- 不承诺物理卸载后所有极晚到达的 ART bridge 调用都消失；内部 tombstone
  会保留必要的原方法引用。

需要更强隔离时，应增加独立 native worker 或版本化 ABI，而不是在现有
callback 中偷偷引入后台线程和长期 JNI 引用。
