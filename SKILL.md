---
name: dejavu
description: >-
  Dejavu Android Zygisk hook toolkit. Use this skill when building, installing
  or operating Dejavu on a rooted Android device with a Zygisk provider:
  packaging the module, selecting target processes, running Lua control
  scripts, installing Java hooks (hook.*/hookx.*), native function hooks
  (native.*) and active invocation (native.call/java.invoke), reading Dejavu
  logcat, and
  running the on-device acceptance suite. Covers environment checks, safety
  rules (device restart boundaries, targets allowlist, trusted native callbacks),
  hook source constraints and troubleshooting.
---

# Dejavu 使用技能

Dejavu 是一个 Android arm64 动态调试 / hook 工具：核心 `libdejavu.so` 内嵌
Lua 5.4 + TinyCC，可在目标进程里用 Lua 编排、现场编译 C 回调；可选
`zygisk/` 集成通过 Zygisk provider 注入目标进程，内置 LSPlant（ART Java
hook）+ Dobby（inline hook）。本仓库同时提供统一 CLI `./dejavu`。

常见场景的专项技能维护在 [SKILLS/](SKILLS/README.md)，按需读取对应技能的 `SKILL.md`。

## 何时使用

- 在已 root、启用 Zygisk provider 的设备上动态 hook Java 方法、观察/修改
  参数与返回值；
- 对目标进程的 native 函数按模块符号或绝对运行时地址做 inline hook（`native.*`）；
- 需要免重启设备的开发循环（热部署 agent、重启单个 App）；
- 验证 Dejavu 构建/模块/设备链路（CI 之外的本地验收）。

不适用：非 root 设备、生产环境热修复、安全沙箱需求（hook 回调是受信代码，
以目标进程权限运行）。

## 按问题选插件

公开示例和 ABI 验证工具位于 `examples/`，可独立于私有 `plugins/` 子模块使用。[公开示例索引](examples/README.md)

| 插件 | 适用场景 | 输出与限制 |
| --- | --- | --- |
| [`hello`](examples/hello/SKILL.md) | 先验证 Agent、插件 ABI、调用和卸载链路。 | `greet` 验证调用，`info` 检查内存映射查询和模块符号解析。原生 `hello` 本身不安装 Hook，可安全卸载。 |
| [`capture`](examples/capture/README.md) | 分析受控测试请求在目标进程内的 TLS 收发方向、调用次数和流量大小。 | `status` 提供收发次数和字节数，`probe` 可用合成记录检查流通道。仅覆盖已加载插件的进程及其 `libssl` 路径，不负责解码 HTTP/2、压缩或二进制协议。内容输出可能包含敏感数据，也可以写入文件。 |
| [`algorithm`](examples/algorithm/README.md) | 确认测试样本走原生加密库还是 Java 加密 API，观察实际执行的摘要、HMAC、加解密或签名操作。 | 原生部分依赖已加载且可 Hook 的 `libcrypto` 符号；[Java Lua 示例](examples/algorithm/java/algorithm_java.lua) 覆盖常见 JCA 入口，输出到 logcat。没有命中不代表没有加密，记录也可能含密钥、IV 或明文。 |
| [`unpack`](examples/unpack/README.md) | 对自有测试 APK 核对运行时加载的 DEX，以及静态构建产物和运行时产物的差异。 | 通过 ART Hook 采集 DEX 快照，后台写入文件，适合离线对照。它会改变目标运行状态，且不支持安全热卸载；优先从已有 APK/DEX 做静态检查。 |

`hello/` 还提供三种 JIT 对照样例：[基础 ABI](examples/hello/hello_jit.c.SKILL.md)、[文件接口](examples/hello/hello_fs_jit.c.SKILL.md)和 [Hook 卸载清理](examples/hello/hello_hook_jit.c.SKILL.md)。只验证其中一项时，用对应的最小样例。

## 分析顺序

1. **先定位层级。** 从源码、日志或已有测试产物确认问题。需要真机时，先用 `./dejavu status` 核对设备、目标进程和模块状态，再用 `hello` 排除 Agent／RPC／ABI 链路问题。进程名要精确到 `:worker` 等后缀。
2. **用最小输入复现。** 只在自有或明确获授权的测试目标上运行，优先使用合成请求或测试数据。先检查插件状态与计数；只有这些信息不足时，才查看必要的输出。区分“插件未加载”“目标库或符号不可用”和“已开始观察但没有命中”。
3. **说明产物与清理。** 记录插件、进程、触发条件和可复现证据。确认输出是内存、流、logcat 还是文件；不落盘任务应避免会写文件或输出正文到 logcat 的配置。停止本次采集并清理本次产生的临时数据，报告中不贴出原始消息、密钥或令牌。

## 安全与操作约束（必须遵守）

1. **不要随意重启设备**。只有设备提供 `ksud` 时，才可用
   `adb shell su -c 'ksud soft-reboot'` 或 `./dejavu install --soft-reboot`
   请求软重启。重启整机需先向用户确认。
2. 安装模块、改 `targets.txt`、热部署 agent 都作用于真实设备：先
   `./dejavu status` 确认设备序列号与目标进程，避免误操作。
3. `targets.txt` 是进程白名单，只加入要调试的进程；不要 hook 系统关键进程
   （system_server、zygote、telephony 等）。
4. 设备端 hook 源码在目标进程内以 `-nostdlib` 编译：**不能 `#include`**，
   只能用固定 preamble 中的 `dejavu_hook_*` / `DEJAVU_HOOK_TRY`。
   回调必须短小，不能阻塞或长时间持有对象。
5. 已运行进程保留旧映射。部署核心库或 Agent 后重启目标进程，Loader、
   provider 或模块安装状态变化后需要重启 Zygote。
6. 仓库内改动应保持 `extern/` 子模块干净；构建脚本会自动应用
   `patches/tinycc-runtime-safety.patch` 到临时副本。

## 环境检查

```sh
./dejavu status                  # 主机工具链、设备、root、provider、模块、targets、PID
./dejavu --help                  # 全部子命令
```

若 `adb` 在 Windows、命令在 WSL 中执行：设置 `ADB=/mnt/c/.../platform-tools/adb.exe`
和 `ADB_SERIAL=<serial>`；CLI 会自动处理文件传输路径问题。

源码构建需要 NDK r29 + CMake 3.28+/Ninja（仓库可能已在
`out/tooling/env.sh` 中提供工具链，`./dejavu` 会自动加载）。

## 核心工作流

### 1. 构建、打包、安装

```sh
./dejavu build                    # 构建 out/android-arm64-v8a-api26/libdejavu.so
./dejavu package                  # 构建并打包 zygisk/dist/dejavu-zygisk-v*-arm64.zip
./dejavu install                 # 自动探测 ksud / magisk 并安装
```

### 内置 Router（独立进程）

Router 是框架内部组件，正式可执行文件安装到 `/data/adb/dejavu/bin/dejavuerouter`。
Manager 的**配置 → Router**管理其独立 root daemon 的状态、启停、Hub 配置和唤醒规则。
启动后由模块 watchdog 维持，关闭 Manager 不停止；停止会关闭自动重启和开机启动。
Android 管理通道使用带令牌认证的具名 abstract Unix socket，先校验 root 对端身份。
数据目录为 `/data/adb/dejavu/router`，只保存配置和唤醒规则，不保存转发正文或状态历史。
插件的在线路由由插件注册，删除唤醒规则不会断开在线插件。唤醒仍遵循已有插件作用域，
不会改写全局自动加载属性。旧的独立 `dejavurouter` 模块需另行迁移，不自动覆盖。
构建、管理协议和隔离测试见 [Router 指南](router/README.md)。

### 插件业务 RPC 规范

原生业务插件声明 `DEJAVU_PLUGIN_CAP_RPC`，通过 `get_interface("rpc", 1)`
注册方法表和路由。框架负责默认 Unix socket `dejavu.router.plugins`、重连、
握手、请求信封、超时取消和卸载排空；插件只保留业务参数校验与应用 API 调用。
回调请求只在调用期间有效，结果 JSON 必须使用 host 分配器；业务错误返回
`ok=0` 和领域错误码，`rpc.*` 前缀由框架保留。超时不代表业务回滚，不自动重放请求。
完整约定见 [plugin-rpc.md](zygisk/docs/plugin-rpc.md)，最小示例见
[RPC Echo](examples/rpc_echo/README.md)。

### 2. 配置目标进程

```sh
./dejavu targets list
./dejavu targets add com.android.settings
./dejavu targets set bin.mt.plus           # 替换手动目标，保留插件作用域
./dejavu targets remove com.android.settings
adb shell am force-stop com.android.settings
adb shell monkey -p com.android.settings 1
```

Loader 在进程启动时读取目标配置。保存后重启受影响的 App。Manager 和
`./dejavu targets` 维护同一份作用域归属。模块 WebUI 保存时会写回纯文本进程列表，
覆盖归属信息。[作用域配置](zygisk/docs/user-guide.md#5-配置作用域)

### 3. 执行 Lua 控制脚本

```sh
./dejavu exec -p com.android.settings -e 'return 6 * 7'      # 42
./dejavu exec -p com.android.settings -e 'return hook.list()'
./dejavu exec -p com.android.settings path/to/script.lua
./dejavu repl -p com.android.settings                        # 交互式，保留状态
./dejavu watch path/to/live.lua -p com.android.settings      # 保存即重执行
```

- `exec -e` 打印的是 **Lua 返回值**（仅标量）；`hook.log()` 输出在设备
  logcat，用 `./dejavu log -p <进程>` 查看。
- 控制结果上限 1 MiB；超过会返回明确的执行错误（不会断链）。
- `dejavuctl` 的主机锁按本机用户共享。该用户的 CLI 会话会串行执行，REPL 和
  watch 在整个会话期间持锁。
- 核心 Lua 会话加载 base、table、string 和 math 标准库。Agent 额外注册
  `hook`、`native`、`java`、`fs`、`module`、`mem` 和插件接口。
  `fs.*` 以目标进程身份访问文件。
- `./dejavu exec --timeout SECONDS` 设置 RPC 交换期限，默认 40 秒。超时可能
  发生在 Lua 已开始执行之后，先核对目标状态再决定是否重试。

### 4. 安装 Java hook

```lua
-- /tmp/demo.lua
local source = [[
int before_hook(dejavu_hook_context *context) {
    (void)context;
    dejavu_hook_log("Activity.onResume entered");
    return DEJAVU_HOOK_OK;
}
]]
local id = hook.install("android.app.Activity", "onResume", "()V", source)
hook.log("installed id=" .. id)
return hook.list()
```

```sh
./dejavu exec -p com.android.settings /tmp/demo.lua
./dejavu log -p com.android.settings        # 观察 DejavuHook 日志
```

规则：

- `hook.install(class, method, jni_signature, c_source)` 返回正整数 id；
  C 源码至少定义 `before_hook` 或 `after_hook`，返回 `DEJAVU_HOOK_OK` 或
  `DEJAVU_HOOK_ERROR_*`。
- `hook.list()` 每行 `id<TAB>active|disabled<TAB>类#方法签名`；
  `hook.remove(id)` 逻辑移除（保留物理 hook），`hook.uninstall(id)` 物理卸载，
  `hook.clear()` 清空。
- 只读参数/设置结果使用 ABI 函数（见 `zygisk/docs/hook-c-api.md`）；
  失败检查用 `DEJAVU_HOOK_TRY(...)`。
- 生成代码中的可写 static/global 已由 TinyCC patch 支持；不要写超长回调。

### 4.1 Native 函数 hook（Dobby）

在目标进程里按已加载模块的符号或已知运行时地址安装 hook：

```lua
local source = [[
long dejavu_native_target(int fd, void *buffer, unsigned long count) {
    dejavu_native_hit();
    return ((long (*)(int, void *, unsigned long))dejavu_native_original())(
        fd, buffer, count);
}
]]
local id = native.install("libc.so", "read", source)
local hits = native.stats(id)
native.uninstall(id)
```

`v0.9.1` 新增地址入口：

```lua
local address = module.resolve("libc.so", "read")
local id = native.install_address(address, source)
-- 设备已有 C 源码时：native.install_address_file(address, "/data/.../hook.c")
native.uninstall(id)
```

```sh
./dejavu native -p com.android.settings install-address 0x7abcdef000 replacement.c
./dejavu native -p com.android.settings install-address-file 0x7abcdef000 /data/.../hook.c
```

- 替换函数必须命名为 `dejavu_native_target`，签名与目标一致；preamble 提供
  `dejavu_native_original()` / `dejavu_native_hit()` / `dejavu_native_log()`。
- 符号入口先用 `dlsym`，再查已加载 ELF 的符号表；地址入口无需符号，接受正 Lua
  integer 或 `0xHEX` 字符串，拒绝浮点、零、负值、溢出。目标须四字节对齐，且
  完整入口指令位于可执行映射。CLI 接受十进制或十六进制地址。
- 地址只对当前进程本次加载有效；ASLR、进程重启或模块重载后重新获取。文件
  offset 不能直接作为运行时地址。匿名可执行映射由调用方保持有效，直到所有
  已进入的回调及保存的 original 的后续使用全部结束；卸载不等待这些使用结束，
  无法证明静默时保留至进程退出。安装全过程须保持同一目标映射稳定；已识别的
  模块会保留引用，无法保留时拒绝安装。
- 原跳板在替换生效前发布，卸载后保留已暴露的代码和上下文以供在途回调完成；
  插件自己的状态仍须同步，重复安装/卸载保留的内存在进程退出时释放。
- `native.list()` 查看、`native.clear()` 全部卸载；详见
  `zygisk/docs/native-hook-api.md`。

主动调用（不安装 hook 直接执行目标函数，返回整数）：

```lua
local source = [[
void dejavu_native_call(void) {
    int (*target)(void) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    dejavu_native_call_return((long long)target());
}
]]
local pid = native.call('libc.so', 'getpid', source)   -- e.g. 12345
```

- C 源必须定义 `void dejavu_native_call(void)`，用注入的
  `DEJAVU_NATIVE_CALL_ADDRESS` 拿函数地址；`dejavu_native_call_return(v)`
  上报返回值。
- 参数按统一类型协议转换：nil/boolean/integer/number/string，C 侧用
  `dejavu_native_call_argument_int/_double/_boolean/_string(i, &size)` 取对应
  类型（`_argument` 是 int64 兼容视图），最多 32 个参数。
- 同步执行，阻塞函数会卡住控制通道；C 签名不匹配会崩进程（受信代码模型）。
- 常用来确定性地触发已安装的 native hook：`native.call` 后查
  `native.stats(id)`。

CLI 快捷方式（等价于在目标进程执行对应 Lua）：

```sh
./dejavu native -p com.android.settings list
./dejavu native -p com.android.settings install libc.so read replacement.c
./dejavu native -p com.android.settings call libc.so getpid call.c
./dejavu native -p com.android.settings stats <id>
./dejavu native -p com.android.settings uninstall <id>
./dejavu native -p com.android.settings clear

./dejavu java -p com.android.settings invoke java.lang.String valueOf '(I)Ljava/lang/String;' --arg 42
./dejavu java -p com.android.settings new java.lang.StringBuilder '(Ljava/lang/String;)V' --arg abc
./dejavu java -p com.android.settings invoke <handle> toString '()Ljava/lang/String;'
./dejavu java -p com.android.settings release <handle>
./dejavu java -p com.android.settings load-dex /data/user_de/0/<pkg>/files/probe.dex
./dejavu java -p com.android.settings invoke <loader>:dexprobe.Probe add '(II)I' --arg 3 --arg 4
```


### 4.2 Java 主动调用（java.invoke）

不装 hook，直接调用目标进程里的 Java 方法：

```lua
-- 静态方法：第一个参数是类名
local text = java.invoke('java.lang.String', 'valueOf', '(I)Ljava/lang/String;', 42)

-- 构造 + 实例方法 + 释放
local handle = java.new('java.lang.StringBuilder', '(Ljava/lang/String;)V', 'abc')
local value = java.invoke(handle, 'toString', '()Ljava/lang/String;')
local loader = java.load_dex('/data/user_de/0/<pkg>/files/probe.dex')
local probe_text = java.invoke(loader .. ':dexprobe.Probe', 'hello',
                               '(Ljava/lang/String;)Ljava/lang/String;', 'world')
java.release(handle)
java.release(loader)
```

- `java.invoke(类名, 方法, JNI签名, ...)` 调静态方法；
  `java.invoke(handle, 方法, JNI签名, ...)` 调实例方法。
- 参数/返回值由 **JNI 签名驱动转换**：Z→boolean、B/C/S/I/J→integer（范围
  校验）、F/D→number、`Ljava/lang/String;`↔string（UTF-8/UTF-16）、其他引用
  与数组↔整数对象 handle（`nil` 为 null）。
- Java 异常会转成 Lua 错误（`Throwable.toString()`，异常已清理）；对象
  handle 是进程内全局引用，最多 1024 个，用 `java.release` 释放，目标进程
  重启后失效。
- 同步执行；阻塞的 Java 调用会卡住控制通道。详见
  `zygisk/docs/java-invoke-api.md`。

### 4.3 统一参数类型协议

`native.call` 与 `java.invoke` 共用同一套 Lua 值模型：`nil`、boolean、
integer（64 位精确）、number（double）、string（UTF-8）。

| Lua 值 | native.call（C 访问器） | java.invoke（按 JNI 签名） |
| --- | --- | --- |
| nil | null，`_argument` 返回 0 | null（引用类型） |
| boolean | `_argument_boolean` | `Z` |
| integer | `_argument_int` / `_argument` | `B C S I J`、（`F D` 也接受） |
| number | `_argument_double` | `F D` |
| string | `_argument_string(i, &size)` | `Ljava/lang/String;` |
| 对象 handle（integer） | 作为整数传递 | 其他引用类型 / 数组 |

类型不匹配由协议检查并返回 Lua 错误（不会静默重解释内存）。

**链式调用**：返回值可以继续参与下一次主动调用。`java.invoke` 返回的
primitive/string 可直接作为下一次参数；返回的对象/数组是 handle，可作为
receiver 或引用参数继续调用，用完 `java.release`。`native.call` 返回的
int64 可传给后续 `native.call`/`java.invoke`。注意 handle 释放后失效、
目标进程重启后失效；对象 handle 不能当作 jobject 传给 `native.call`。
详见 `zygisk/docs/java-invoke-api.md` 的 "Chaining invocation results"。

### 4.4 Agent helper（文件 / 模块符号）

```lua
fs.read(path[, offset, size]); fs.write(path, data[, append]); fs.exists(path)
fs.remove(path); fs.list(dir); fs.stat(path)

local id = module.load('libz.so')          -- dlopen, 返回 id
module.symbols(id, 'inflate')              -- name<TAB>TYPE<TAB>BIND<TAB>0xaddr
module.resolve('libc.so', 'getpid')        -- 整数地址（含隐藏符号）
module.loaded(); module.unload(id)

mem.read(addr, size); mem.write(addr, data)
mem.read_u8/u16/u32/u64(addr); mem.write_u8/u16/u32/u64(addr, value)
mem.dump(addr, size[, width]); mem.query(addr)
mem.protect(addr, size, 'rw-'|'r--'|...); mem.maps([filter])
```

- `fs.*` 以目标进程 UID 访问文件（不是沙箱）；`fs.read` 拒绝非普通文件和
  >64MiB 文件，`fs.write` 默认 0644，第三个参数 `true` 表示追加。
- `module.load` 用 `RTLD_NOW`，scope 可传 `'local'`（默认）或 `'global'`；
  `module.symbols` 读取已加载库的 ELF `.dynsym`+`.symtab`，支持子串过滤，
  输出上限 50k/1MiB；`module.resolve` 先 dlsym、失败后回退 ELF 符号表，
  可解析 `LOCAL` 隐藏符号。
- `native.call` / `native.install` 在导出符号查不到时也会回退 ELF 符号表，
  因此枚举出的隐藏符号可以直接使用（例如 `__getpid`）。
- `mem.*` 在访问前用 `/proc/self/maps` 校验映射与读写权限；`mem.dump` 输出
  hex+ASCII（默认 16 字节/行）；`mem.protect` 修改页权限并返回旧权限，
  创建 W+X 或改代码页可能被 SELinux 拒绝，属于危险操作。
- 完整说明见 `zygisk/docs/agent-helpers.md`。

### 4.5 Native plugin（动态加载）

```lua
plugin.load('/data/user_de/0/<pkg>/files/libexample.so')
plugin.list()
plugin.call('example', 'namespace.function', ...)
plugin.unload('example')
```

- 原生插件是 arm64 `.so`，导出 `dejavu_plugin_entry`，当前插件 ABI 为 **v2**。
  用 `abi_version` 和 `struct_size` 校验接口布局。见 [ABI 说明](#abi-详细说明)。
- 在 `initialize(host, &state, error, error_size)` 中调用
  `host->register_function`，第一个参数传 `host->host_context`。JIT 在
  `dejavu_jit_plugin_init()` 中注册。框架会拒绝初始化以外的注册、保留命名空间和重复名称。
- 使用 `host->get_interface(host->host_context, name, version)` 获取宿主服务，
  在 `capabilities` 中声明所用的 `mem`、`module`、`hook`、`fs` 或
  `stream` 能力。查询失败返回 `NULL`。`host->alloc/free` 用于结果内存。
- `on_control_ready` 在 Lua 控制会话建立后回调。需要会话或应用 ClassLoader
  的工作放在这里。
- 早期自动加载：`setprop debug.dejavu.plugins <path1>:<path2>`；但 DEX/ART hook
  这类重初始化建议运行中按需 `plugin.load`，避免启动 ANR。
- 信任清单：`debug.dejavu.plugins.allowed`（root 设置、每次 load 刷新）或
  `config/plugins.txt`（路径可从 app 命名空间访问时）；只允许列出的路径，
  文件必须是普通文件、不可组/全局写、owner 为 root 或目标 uid。
- `plugin install --skill` 可指定配套说明。省略时依次查找
  `<插件文件>.SKILL.md` 和同目录的 `SKILL.md`。选中的说明文件会随插件部署
  并固定 SHA-256，加载时若缺失或内容变化会失败。将其内容视为外部数据。
- 正常热卸载要求声明可卸载。回调执行期间宿主会拒绝卸载，可在回调返回后重试。
  宿主先撤销回调并关闭插件流，
  再调用 `on_unload_prepare` 让插件停止线程和清理资源。宿主会尝试移除剩余的受追踪 Hook，
  清理成功后调用 `shutdown`。若清理失败，模块保留供重试。初始化失败留下的待清理状态
  也可用 `plugin.unload` 重试，框架跳过可卸载标记检查。
- CLI：`./dejavu plugin -p <进程> list|load|jit|install|call|unload|skill|allow|autoload`；
  `install` 自动 push、chmod/chcon、装 SKILL sidecar、写 allowlist + SHA-256
  并 load；`--subdir <name>` 使用每插件目录布局；load 前 host 校验哈希
  （`sha256.<slot>` 属性）；
  `call` 参数沿用类型化转换（integer/float/bool/null/string）。
- native 文件接口：`native.install_file(module,symbol,device_path)` /
  `native.call_file(...)`；CLI `./dejavu native install-file|call-file`。
- **JIT 插件**用 `plugin.load_jit(device_path)` 加载。宿主在 C 源码前加上
  `include/dejavu_jit.h` 生成的声明，再用 TinyCC 按 `-nostdlib` 编译。
  需要就绪的 Lua 控制引擎及 `dejavu_jit_plugin_init()` 入口。最多缓存四个模块，
  按源码 SHA-256 查找。命中缓存时全局状态可能跨卸载保留。见
  [hello JIT 示例](examples/hello/hello_jit.c)。
- 配套说明部署为 `<插件文件>.SKILL.md`。用 `./dejavu plugin -p <进程> skill <name>`
  读取已加载插件的说明。
- 示例：`examples/hello`（host 服务/契约测试）、`examples/capture`（SSL 明文
  抓包示例）、`examples/unpack`（脱壳）。`examples/package.sh` 打
  `dejavu-plugins-v<version>-arm64.zip`，CI Release 会把它作为第二个资产；
  unpack CLI：`python3 examples/unpack/cli.py ...`。
- `dejavuctl` 单命令只对 module 明确标记的
  `target Agent disconnected; retry the command` 做重连重试；socket timeout
  等可能已执行请求的模糊错误不重放，避免非幂等 Lua 重复执行。
- 插件/JIT 设备生命周期：`make plugin-device-test`（native load/call/unload、
  JIT 重复加载、hook 索引移位与 prepare 摘钩、atoi 恢复）。
- 实时抓包流：框架 stream 服务支持 TCP/UDP/KCP；host 用
  `./dejavu stream --transport tcp|udp|kcp --port N` 查看，插件通过
  `host->get_interface(host->host_context, "stream", version)` 或
  `dejavu_jit_stream_*` 推送记录。
- AI agent 接入口：`./dejavu mcp -p <pkg>` 启动 MCP stdio server；21 个工具
  覆盖 status/state、exec、插件生命周期/call/skill/allow、native、java、
  logs、验收套件、deploy-agent 和 `dejavu_cli` 通用逃生口；详见 README 的
  MCP 配置示例；真机端到端用 `tests/mcp_device_test.py`。目标进程存活期间
  Lua 会话/插件/hook 是累计的，多步操作前先取 `dejavu_state`。
- 设备套件用 `./dejavu test -p <pkg>`，默认运行一次。它会把
  `config/targets.txt` 覆盖为测试进程、清除现有 `run/*.lua` 请求并重启目标，
  结束时不会恢复原作用域。启用 `--retries` 后，每次尝试前都会清空 logcat。

## ABI 详细说明

| 层级 | 当前契约 | 定义 |
| --- | --- | --- |
| Android 二进制 | `arm64-v8a`（AArch64，64 位）。公开示例用 Android NDK、`android-26` 构建。 | [原生示例构建脚本](examples/hello/build.sh) |
| 原生插件 | `DEJAVU_PLUGIN_ABI_VERSION = 2`，通过 C 入口返回描述符。 | [dejavu_plugin.h](include/dejavu_plugin.h) |
| TinyCC JIT 插件 | Agent 在进程内编译 C 源码，宿主注入只追加声明的 prelude。 | [dejavu_jit.h](include/dejavu_jit.h) |
| 核心 C 库 | `dejavu_version()` 对应核心 ABI `0.1.0`，包含引擎、Lua 会话及值类型。 | [dejavu.h](include/dejavu.h) |
| Zygisk ↔ Agent | 内部 ABI `3`，模块通过 `dejavu_agent_get_api(version)` 获取函数表。 | [dejavu_agent_api.h](zygisk/include/dejavu_agent_api.h) |
| Java Hook C 回调 | 独立 ABI `1`，规定 Hook 回调阶段、参数和返回值的读写及引用生命周期。 | [Hook C API](zygisk/docs/hook-c-api.md) |
| Manager 插件包 | `manifest.json` 的 `schemaVersion: 1` 管理资源与入口。Lua 包入口返回 `start(ctx)`、`stop(ctx)` 和 `status(ctx)`。 | [Manager 插件包契约](app/README.md#zip-plugin-import) |

发版标签跟随 Zygisk 模块包版本。判断兼容性时，应分别核对实际调用的接口版本和结构体长度。

### 原生插件入口与兼容性

- 用 NDK 生成共享库。默认隐藏符号，导出 `const DejavuPluginDescriptor *dejavu_plugin_entry(void)`。C++ 实现使用 `extern "C"`。描述符和它引用的内容应在模块存活期间有效。
- Agent 要求 `abi_version` 精确等于 `2`，`struct_size` 至少覆盖到 `shutdown`，并检查 `name` 和 `initialize`。新插件用 `sizeof(DejavuPluginDescriptor)` 填充长度。名称须匹配 `[A-Za-z_][A-Za-z0-9_]*`，同一 Agent 内不能重名。`version` 是插件自身的版本。
- `capabilities` 和 `on_unload_prepare` 是追加字段。读取前用 `offsetof(类型, 字段) + sizeof(字段)` 检查字段是否完整。宿主表和服务接口也有版本及长度。插件应在解引用之前检查所用字段，保持公开结构体的既有字段顺序、类型和对齐。
- `initialize(host, &state, error, error_size)` 返回 `0` 表示成功。保存本实例的宿主表和 `host_context`，后续调用宿主服务时传回同一 context，工作线程也遵守这一约定。需要 Java ClassLoader 的工作放到可选的 `on_control_ready(state)` 中，此时 Lua 控制会话已就绪。[原生 hello 入口](examples/hello/plugin_entry.cpp)

### 宿主服务与能力位

在原生描述符的 `capabilities` 或 JIT 的 `dejavu_jit_plugin_capabilities()` 中声明所用服务。原生插件调用 `host->get_interface(host_context, name, version)`。能力未声明、版本不支持或接口不可用时返回 `NULL`，初始化应据此报告缺失的必需服务或跳过可选功能。

| 能力位 | 接口版本 | 用途 |
| --- | --- | --- |
| `MEM`（`0x01`） | `mem` v1 | 当前进程的内存读写与映射查询。 |
| `MODULE`（`0x02`） | `module` v1 | 解析已加载模块的符号。 |
| `HOOK`（`0x04`） | `hook` v1 | 安装和移除宿主追踪归属的原生 Hook。 |
| `FS`（`0x08`） | `fs` v1 | 访问目标进程可读写的文件。 |
| `JAVA`（`0x10`） | — | 头文件定义的能力位。当前宿主表通过 `java_vm` 和 `get_app_class_loader` 提供 JNI 入口。 |
| `STREAM`（`0x20`） | `stream` v1／v2 | v1 支持连接、写入和关闭，v2 增加 TCP 监听、接收连接和读取。 |

流的每次写入是一条记录，使用 4 字节网络字节序长度前缀。句柄归当前插件实例所有。读取返回 `1` 表示收到一条记录，`0` 表示超时，`-1` 表示 EOF、错误或记录超出缓冲区。[宿主服务与流接口](zygisk/docs/plugin-api.md#host-services)

### Lua 调用与数据所有权

- 原生插件在 `initialize` 内用 `host->register_function` 注册 `namespace.function`，首个参数传 `host->host_context`。JIT 插件在 `dejavu_jit_plugin_init` 内用 `dejavu_jit_register` 注册。框架会拒绝初始化阶段以外的注册，包括 `on_control_ready`。名称使用简单标识符，框架保留 `hook`、`fs`、`module`、`native`、`java`、`mem` 和 `plugin` 命名空间，重复注册会被拒绝。用 `plugin.call(plugin_name, "namespace.function", ...)` 验证调用。
- 回调接收会话、注册时传入的 context、借用的参数数组、参数数量、结果指针和错误缓冲区。先验证参数数量和 `type`。字符串按 `string_size` 读取。参数在回调返回后失效，需要延长寿命的数据应在回调内复制。
- 返回值填写对应的类型和字段。原生成功返回 `DEJAVU_OK`，JIT 成功返回 `DEJAVU_JIT_OK`。失败时返回相应错误码并在给定容量内写入错误信息。需要转移字符串所有权时，用宿主分配器分配，填写字节数并置 `owned = 1`。借用字符串应在宿主完成拷贝前保持有效，避免返回栈内存。
- `HANDLE` 的底层资源由调用方按约定管理。返回 `HOST_FUNCTION` 时，回调 context 由插件维护。框架跟踪执行中的回调，并在卸载时撤销已注册和已返回的回调。[值类型和所有权](include/dejavu.h)

### JIT 插件与卸载

- JIT 插件需要已就绪的 Lua 控制引擎，通过 `plugin.load_jit` 按需加载。源码上限为 512 KiB，由 TinyCC 按 `-nostdlib` 编译。必需导出 `int dejavu_jit_plugin_init(void)`。名称、版本、能力、可卸载标志及生命周期回调使用头文件中的可选导出，回调按 `DejavuJitFunction` 签名编写。
- JIT 流服务在初始化或已注册回调中使用当前插件身份。工作线程使用流服务时，由插件先捕获上下文，再在线程中绑定，退出前解除绑定。卸载会撤销该上下文，重新加载后应重新获取。JIT 模块按源码 SHA-256 缓存，同一源码重新加载时全局状态可能保留，初始化时应显式重置本次运行所需的状态。[JIT 生命周期](zygisk/docs/plugin-api.md#jit-tinycc-plugins)
- 初始化失败时，插件应在返回前停止并等待自己启动的线程退出，释放自行管理的资源。框架会关闭该插件的流并尝试移除宿主追踪的 Hook。若仍有待清理状态，模块保持加载并禁用其回调，可用 `plugin.unload` 重试清理，此时跳过可卸载标志检查。初始化失败的状态不会收到 `on_unload_prepare` 或 `shutdown`。
- 正常热卸载要求原生描述符声明 `DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE`，或 JIT 的 `dejavu_jit_plugin_unload_safe()` 返回非零。执行中的回调会阻止卸载。进入卸载后，框架禁用新回调并关闭该插件的流，再调用可选的 `on_unload_prepare`，插件应在此停止工作线程并确保原生回调退出。框架随后尝试移除剩余的宿主追踪 Hook，成功后调用可选的 `shutdown`，关闭原生库或释放 JIT 模块引用。清理失败时模块会保留，供后续重试。[加载和卸载实现](zygisk/src/plugin_manager.cpp)

排查加载失败时，依次检查目标进程架构、设备端文件是否可加载、入口符号与 ABI 版本、初始化返回值。接口获取失败时核对能力位、接口版本和该实例的 `host_context`。修改 ABI 时用原生 `hello` 和 JIT `hello` 分别验证加载、调用和卸载，并用 `make host-smoke` 检查公开 JIT 头文件。

Manager 导入每次使用包含一份合法 `manifest.json` 及声明资源的单插件 ZIP。公开示例合集需先解包再逐个打包。导入后配置作用域并选择进程，在**控制**页启动 Lua 插件，或加载原生和 JIT 插件。[Manager 插件指南](app/README.md#zip-plugin-import)
