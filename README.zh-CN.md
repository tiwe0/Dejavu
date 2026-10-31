# Dejavu

[English](README.md) | **简体中文**

`libdejavu.so` 是一个面向 Android arm64 的共享库骨架，内嵌：

- Lua 5.4.8
- TinyCC 0.9.27-frida

公共 ABI 定义在 `include/dejavu.h`。Lua 和 TinyCC 以固定版本的 Git
子模块引入，且其符号不会暴露到公共 ABI。引擎可以直接在内存中编译
freestanding C，Lua 则作为该引擎之上的轻量控制层。

核心 `libdejavu.so` 不依赖 Magisk。任何能够控制自身进程的应用都可以直接
集成或加载它。`zygisk/` 目录只是一个可选集成，用于把 Dejavu 注入其他
Android 进程；这条路径需要兼容的 Zygisk provider，但并不特指 Magisk。

Android Zygisk 集成还提供了纯 Lua 的高级辅助层 `hookx.*`，用于完成常见
hook 操作。完整说明参见 `zygisk/docs/user-guide.md` 和
`zygisk/docs/cookbook.md`。

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

每次向 `main` 推送且全部检查通过后，工作流都会把 Zygisk 模块 ZIP 发布为
GitHub 预发布版本，标签格式为 `v<模块版本>-build.<工作流运行号>`。Pull
Request 和 `develop` 构建不会创建 Release。推送与模块版本一致的 `v*` 标签
（例如 `v0.2.0`）时，则会创建对应的正式版本。只有主机测试、Android API
26 基线、兼容性构建、Zygisk 打包和代码质量检查全部通过后才会发布。

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

## 版本语义

- `include/dejavu.h` 中的 `dejavu_version()` / `DEJAVU_VERSION_*` 是**核心库
  ABI 版本**（当前 `0.1.0`），公共 C ABI 变化时才递增。
- `zygisk/module/module.prop` 和 release tag（`vX.Y.Z`）是 **Zygisk 模块包
  版本**（当前 `0.2.0`）；模块打包、loader/agent 行为与文档更新独立递增。
- CI 对 `main` 的 push 发布 `v<模块版本>-build.<run>` 预发布；推送匹配的
  `v*` tag 才发布正式版本。

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
