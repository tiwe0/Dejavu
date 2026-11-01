# Dejavu

[English](README.md) | **简体中文**

`libdejavu.so` 是一个面向 Android arm64 的共享库骨架，内嵌：

- Lua 5.4.8
- TinyCC 0.9.27-frida

公共 ABI 定义在 `include/dejavu.h`。Lua 和 TinyCC 以固定版本的 Git
子模块引入，且其符号不会暴露到公共 ABI。引擎可以直接在内存中编译
freestanding C，Lua 则作为该引擎之上的轻量控制层。

## 免责声明与授权使用

Dejavu 是一个**自用的安全研究工具**。请仅在您拥有的设备、或您已获得明确
授权的进程/应用上进行测试。您需自行遵守适用法律、合同与平台政策（包括
目标应用的服务条款）；不得用于未授权访问、绕过保护措施或损害他人。

本软件按**“现状”**提供，不附带任何形式的保证。作者不对使用或误用造成的
任何损害、数据丢失、账号封禁、法律后果或其他损失负责。您对自己的行为以及
测试的授权合规性负全部责任。

核心 `libdejavu.so` 不依赖 Magisk。任何能够控制自身进程的应用都可以直接
集成或加载它。`zygisk/` 目录只是一个可选集成，用于把 Dejavu 注入其他
Android 进程；这条路径需要兼容的 Zygisk provider，但并不特指 Magisk。

Android Zygisk 集成还提供了纯 Lua 的高级辅助层 `hookx.*`，用于完成常见
hook 操作。完整说明参见 `zygisk/docs/user-guide.md` 和
`zygisk/docs/cookbook.md`。

Zygisk 集成同时支持 Java/Kotlin 方法 hook（LSPlant）和已加载模块中的
native 函数 inline hook（Dobby + TinyCC 生成的替换函数）；`native.*`
API 与限制见 `zygisk/docs/native-hook-api.md`。

也支持主动调用：`native.call(...)` 通过 TinyCC 包装器直接调用已加载模块的
导出函数；`java.invoke(...)` / `java.new(...)` / `java.release(...)` 通过 JNI
直接调用 Java 方法，参数由 JNI 签名驱动转换；返回值（primitive、string、
对象/数组 handle）可以继续参与后续调用（见 `zygisk/docs/java-invoke-api.md`
和 `zygisk/docs/native-hook-api.md`）。

控制会话还提供 agent helper：`fs.read/write/exists/remove/list/stat` 用于
目标进程沙箱内的文件读写；`module.load/unload/loaded/symbols/resolve` 用于
加载 so、枚举 ELF 符号和解析隐藏（`LOCAL`）符号；`native.call` /
`native.install` 在导出符号查找失败时会回退 ELF 符号表。原生 **plugin** 机制
（`plugin.load/list/call/unload`，C ABI 见 `include/dejavu_plugin.h`，文档见
`zygisk/docs/plugin-api.md`）允许插件动态注册 Lua 函数，并可通过
`debug.dejavu.plugins` 在 App 代码执行前自动加载。内存 helper
（`mem.read/write`、类型化 `read_u*/write_u*`、`mem.dump`、`mem.query`、
`mem.protect`、`mem.maps`）提供带映射校验的目标地址空间访问。详见
`zygisk/docs/agent-helpers.md`。

## 引擎模型

第一版 ABI 有意只支持一种生成代码入口签名：

```c
int dejavu_entry(void);
```

直接通过 C API 使用：

```c
dejavu_engine *engine = dejavu_engine_create();
dejavu_module *module = NULL;
char error[512];
int result;

dejavu_engine_compile(
    engine,
    "int dejavu_entry(void) { return 6 * 7; }",
    &module,
    error,
    sizeof(error));
dejavu_module_call(module, &result);
dejavu_module_destroy(module);
dejavu_engine_destroy(engine);
```

生成代码使用 `-nostdlib` 编译。所有宿主函数必须先通过
`dejavu_engine_add_symbol()` 显式注册；未解析符号会被拒绝，而不会通过
`dlsym()` 自动从宿主进程解析。

这份允许列表限制的是链接范围，并不是安全沙箱。生成的 native 代码仍然
拥有宿主进程的权限，并运行在同一地址空间中。

Lua 控制层只开放 `dejavu.compile()`，以及 base、table、string 和 math
标准库：

```lua
local run = dejavu.compile [[
  int dejavu_entry(void) { return 6 * 7; }
]]
return run()
```

## 构建与冒烟验证

构建环境需要 Linux 或 macOS 主机以及 Android NDK；核心库不要求安装
CMake。构建脚本会在临时副本上为固定的 TinyCC 源码应用
`patches/tinycc-runtime-safety.patch`（将可写数据与可执行页分离，并让
重定位失败返回错误而不是退出进程），不会改动 submodule 本身。

```sh
git submodule update --init --recursive
export ANDROID_NDK_HOME="$HOME/Android/Sdk/ndk/android-ndk-r29"
make smoke
```

`make smoke` 会先执行引擎和 Lua 控制层的主机测试，然后交叉编译并检查
Android 产物。若只需要 Android 库，可执行 `make android-arm64`。

默认目标为 `arm64-v8a`、native API 26，输出路径为：

```text
out/android-arm64-v8a-api26/libdejavu.so
```

可以覆盖 native API 或构建类型：

```sh
ANDROID_API=35 BUILD_TYPE=release make smoke
```

Android 应用的 `targetSdkVersion` 由应用清单或 Gradle 配置；这里选择的
NDK API 表示 `.so` 允许使用的最低 Android native API。API 26 产物面向
Android 8 及以上版本，并只使用稳定的 libc 和 NDK API。

完整 Zygisk 集成当前支持 Android 8 至 15（API 26-35）。Android 16 涉及
ART 私有接口和 LSPlant 兼容性，必须作为独立适配目标验证，不能仅凭核心
库成功构建就视为已经支持。

### Android 兼容性状态

| Android 版本 | API | 状态 |
| --- | --- | --- |
| Android 8-14 | 26-34 | 已支持构建和安装；仍需在各代设备及 ROM 上完成运行验收 |
| Android 15 | 35 | 已支持构建和安装，并具备当前 MT Manager 真机及压力测试路径 |
| Android 16-17 | 36-37 | 上游 LSPlant 已支持；Dejavu 尚待真机运行验收，因此模块安装器暂未放开 |

### 自动发布

向 `main` 推送和 Pull Request 只执行构建与测试，**不会**发布 Release。只有
显式请求时才发布：

- 推送与模块版本一致的 `v*` 标签（例如 `v0.4.0`）时创建正式版本；
- 手动运行 `workflow_dispatch` 时，从所选分支创建
  `v<模块版本>-build.<工作流运行号>` 预发布版本。

只有主机测试、Android API 26 基线、兼容性构建、Zygisk 打包和代码质量检查
全部通过后才会发布。

构建冒烟检查会验证产物满足以下条件：

- ELF64 AArch64 共享库；
- SONAME 正确；
- LOAD 段满足 16 KB 页面对齐；
- 仅导出预期的 Dejavu ABI。

库中还包含用于真机验证的入口：

- `dejavu_smoke_lua()`：通过 Lua 编译并调用一个 C 入口函数；
- `dejavu_smoke_tcc()`：编译并执行一个内存中的 AArch64 函数；
- `dejavu_smoke()`：依次运行以上两项检查。

可执行内存来自独立且按页对齐的映射。TinyCC 在 RW 页面中写入和重定位，
随后只把生成代码所在页面切换为 RX；它不会把分配器堆页面改成可执行。
每个受支持的 Android 代际仍需要真机验证目标进程的 SELinux 策略和页面
大小配置。

## 统一 CLI

`./dejavu` 把构建脚本、模块打包和 `dejavuctl` 控制通道收进一个命令：

```sh
./dejavu status                                 # 主机/设备/模块状态
./dejavu build [--api 26]                       # 构建 libdejavu.so
./dejavu package [--api 26]                     # 构建并打包模块 zip
./dejavu install [--zip PATH] [--soft-reboot]   # 推送并通过 ksud/magisk 安装
./dejavu targets list|add|remove|set|clear      # 编辑设备 targets.txt
./dejavu deploy-agent -p <进程>                 # 热部署 agent + init.lua
./dejavu exec -p <进程> -e 'return hook.list()'
./dejavu exec -p <进程> hooks/demo.lua
./dejavu native -p <进程> list|stats|uninstall|clear
./dejavu native -p <进程> install libc.so read replacement.c
./dejavu native -p <进程> call libc.so getpid call.c
./dejavu java -p <进程> invoke java.lang.String valueOf '(I)Ljava/lang/String;' --arg 42
./dejavu java -p <进程> new java.lang.StringBuilder '(Ljava/lang/String;)V' --arg abc
./dejavu java -p <进程> release <handle>


./dejavu repl -p <进程>
./dejavu watch hooks/demo.lua -p <进程>
./dejavu log [-p <进程>] [--dump] [--clear]
./dejavu test -p <进程> [--with-hang-test]
```

支持 `ADB`、`ADB_SERIAL` 与 `-s/--serial`；当 adb 二进制无法读取主机路径
（例如从 WSL 调用 Windows 的 `adb.exe`）时会自动改用 adb shell 流式传输
文件。`./dejavu --help` 或 `./dejavu <命令> --help` 查看完整选项。

供 agent/助手使用的配套说明见 [SKILL.md](SKILL.md)。

## 版本语义

- `include/dejavu.h` 中的 `dejavu_version()` / `DEJAVU_VERSION_*` 是**核心库
  ABI 版本**（当前 `0.1.0`），公共 C ABI 变化时才递增。
- `zygisk/module/module.prop` 和 release tag（`vX.Y.Z`）是 **Zygisk 模块包
  版本**（当前 `0.4.0`）；模块打包、loader/agent 行为与文档更新独立递增。
- CI 对 `main` 的 push 只构建与测试；只有推送匹配的 `v*` tag（正式版本）或
  手动 `workflow_dispatch`（预发布）才会创建 Release。

## 测试

- 主机：`make host-smoke` 覆盖引擎/Lua 测试，以及可写数据、大参数回调、
  （Linux 上）模拟 execmem 拒绝等回归；`make android-arm64` 校验 Android 产物。
- Zygisk：`make -C zygisk test` 运行主机侧 C/C++/Lua/CLI 测试。
- 真机：`TARGET_PROCESS=<包名> ./zygisk/scripts/device-test.sh` 在已 root、
  装有 Zygisk provider 且已安装模块的设备上自动验收（见 `zygisk/README.md`）。
  脚本走文件控制通道、**不会重启设备**，覆盖控制面、实例方法 hook、可写
  生成数据、超大响应与头文件 include 行为；`--with-hang-test` 额外验证
  relay 超时。
- 依赖补丁：见 `patches/README.md`。

## 已知限制

- 不是安全沙箱：hook 回调以目标进程权限运行。
- 设备端生成的代码不能 `#include` 头文件，只能使用固定 preamble 里的 API
  （见 `zygisk/docs/hook-c-api.md`）。
- 加固 ROM 可能拒绝 execmem；DejaVu 会返回错误而不是杀死进程，但 hook 仍需
  按设备验收。
- 挂死的 Lua 控制脚本会卡住该进程的 Agent；companion 通道会超时而不是永久
  阻塞，重启目标进程可恢复。
- LSPlant 依赖私有 ART 符号，每个 Android 代际都需要真机验证；Android
  16/17 仍待验收。

## 许可证

Dejavu 以 MIT 许可证发布，详见 [LICENSE](LICENSE)。第三方组件及其
许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

内置的第三方组件保留各自许可证：Lua 5.4（MIT）、TinyCC（LGPL-2.1）、
LSPlant 与 DexBuilder（LGPL-3.0）、Dobby（Apache-2.0）。模块包会附带
本项目的 LICENSE；再分发二进制前请自行核对第三方许可证要求。

## 示例（非框架能力）

`plugins/unpack/` 是**用例示范**，不是框架稳定能力。它被编译为动态加载插件
（`plugins/unpack/build.sh` → `libdejavu_unpack.so`），通过 `plugin.load` /
`debug.dejavu.plugins` 加载；agent 里不再编译任何 unpack 代码。插件把通用 hook
与 Lua 策略（Manifest 命名空间 → dumped DEX 类解析 → 节流主动调用）组合起来，
由 `plugins/unpack/cli.py` 驱动。详见 `plugins/unpack/README.md` 与
`plugins/unpack/design.md`。
