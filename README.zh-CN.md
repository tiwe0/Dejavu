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
CMake。

```sh
git submodule update --init --recursive
export ANDROID_NDK_HOME="$HOME/Android/Sdk/ndk/android-ndk-r27d"
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
| Android 16+ | 36+ | 当前固定版本的 LSPlant 尚不支持 |

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
