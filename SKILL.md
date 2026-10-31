---
name: dejavu
description: >-
  Dejavu Android Zygisk hook toolkit. Use this skill when building, installing
  or operating Dejavu on a rooted Android device with a Zygisk provider:
  packaging the module, selecting target processes, running Lua control
  scripts, installing Java hooks (hook.*/hookx.*), reading Dejavu logcat, and
  running the on-device acceptance suite. Covers environment checks, safety
  rules (soft reboot only, targets allowlist, trusted native callbacks),
  hook source constraints and troubleshooting.
---

# Dejavu 使用技能

Dejavu 是一个 Android arm64 动态调试 / hook 工具：核心 `libdejavu.so` 内嵌
Lua 5.4 + TinyCC，可在目标进程里用 Lua 编排、现场编译 C 回调；可选
`zygisk/` 集成通过 Zygisk provider 注入目标进程，内置 LSPlant（ART Java
hook）+ Dobby（inline hook）。本仓库同时提供统一 CLI `./dejavu`。

## 何时使用

- 在已 root、启用 Zygisk provider 的设备上动态 hook Java 方法、观察/修改
  参数与返回值；
- 需要免重启设备的开发循环（热部署 agent、重启单个 App）；
- 验证 Dejavu 构建/模块/设备链路（CI 之外的本地验收）。

不适用：非 root 设备、生产环境热修复、安全沙箱需求（hook 回调是受信代码，
以目标进程权限运行）。

## 安全与操作约束（必须遵守）

1. **不要随意重启设备**。需要重载 Loader/模块时用软重启：
   `adb shell su -c 'ksud soft-reboot'`（或 `./dejavu install --soft-reboot`）。
   重启整机需先向用户确认。
2. 安装模块、改 `targets.txt`、热部署 agent 都作用于真实设备：先
   `./dejavu status` 确认设备序列号与目标进程，避免误操作。
3. `targets.txt` 是进程白名单，只加入要调试的进程；不要 hook 系统关键进程
   （system_server、zygote、telephony 等）。
4. 设备端 hook 源码在目标进程内以 `-nostdlib` 编译：**不能 `#include`**，
   只能用固定 preamble 中的 `dejavu_hook_*` / `DEJAVU_HOOK_TRY`。
   回调必须短小，不能阻塞或长时间持有对象。
5. 修改 agent/loader 后，已运行进程不会自动换映射；重启目标进程即可，
   只有 Loader/模块安装变化才需要软重启。
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
./dejavu install --soft-reboot    # 推送 + ksud/magisk 自动探测安装 + 软重启
```

### 2. 配置目标进程

```sh
./dejavu targets list
./dejavu targets add com.android.settings
./dejavu targets set bin.mt.plus           # 覆盖列表
./dejavu targets remove com.android.settings
adb shell am force-stop com.android.settings
adb shell monkey -p com.android.settings 1
```

保存 targets 后只影响**后续启动**的进程；已运行进程需重启该 App。

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
- 同一进程同一时刻只允许一个 CLI 客户端（主机侧 advisory lock）。
- Lua 沙箱只有 base/table/string/math + `dejavu.*`/`hook.*`/`hookx.*`，
  没有文件或系统 IO。

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

### 5. hookx 高层封装（推荐）

```lua
local trace_id   = hookx.trace("android.app.Activity", "onResume", "()V")
local bypass_id  = hookx.force_return_bool("com.example.Guard", "isSafe", "()Z", true)
local rewrite_id = hookx.replace_arg_int("com.example.Target", "compute", "(I)I", 0, 42)
hook.log(hook.list())
```

可用：`hookx.trace`、`replace_arg_int`、`force_return_int/bool/void`、
`log_string_arg`、`log_result_int`、`hookx.sig{args=...,ret=...}`。
详见 `zygisk/docs/user-guide.md` 与 `zygisk/docs/cookbook.md`。

### 6. 免重启开发循环

```sh
./dejavu build                       # 改核心库/agent
./dejavu deploy-agent -p com.android.settings   # 原子替换 agent + init.lua 并重启 App
```

Loader（`zygisk/arm64-v8a.so`）或模块安装变化才需要 `ksud soft-reboot`。

### 7. 验证与排障

```sh
./dejavu test -p com.android.settings               # 设备验收 8 项
./dejavu test -p com.android.settings --with-hang-test   # 含挂死/超时恢复（~40s）
./dejavu log [-p 进程] [--dump] [--clear]
```

日志 tag：`DejavuZygisk`（loader/companion）、`DejavuAgent`（agent 生命周期）、
`DejavuHook`（hook 回调/编译）、`DejavuLSPlant`（ART hook 初始化）。

| 现象 | 处理 |
|---|---|
| `target process is not running` | 启动目标 App；确认 `pidof` 与 `targets.txt` 完全一致 |
| `RPC endpoint is not ready` | Agent 尚未 attach；等待/重启目标进程，确认模块已启用 |
| `target Agent did not respond in time` | 该进程 Lua 线程被长任务卡住；重启目标进程恢复 |
| `control result exceeds the 1 MiB response limit` | 减少返回数据或改用 `hook.log()` |
| `hook.install` 编译失败 | 检查 JNI 签名；设备端不能 `#include`，只用 preamble API |
| 模块安装后无 Agent 日志 | 检查 root/provider、targets、API 26–35；软重启后再试 |
| 直接覆盖运行中 `.so` 导致崩溃 | 用 `deploy-agent`/安装流程的“临时文件+mv”原子替换 |

## 关键文件与文档

- `dejavu` — 统一 CLI（status/build/package/install/targets/deploy-agent/exec/repl/watch/log/test）
- `include/dejavu.h` — 核心 C ABI
- `zygisk/include/dejavu_hook_api.h` — Hook C ABI v1（preamble 暴露的函数）
- `zygisk/docs/user-guide.md` — 安装、部署、Lua API、真机开发循环
- `zygisk/docs/hook-c-api.md` — hook 回调 ABI、结果通道、限制
- `zygisk/docs/cookbook.md` — hookx 常见任务
- `zygisk/scripts/device-test.sh` — 自动化真机验收
- `patches/README.md` — TinyCC 维护补丁说明
- `THIRD_PARTY_NOTICES.md` — 第三方许可证

## 修改仓库时

- 构建：`make host-smoke`、`make android-arm64`、`make -C zygisk test`、`make -C zygisk package`。
- 提交前：`git diff --check`；改动的 C/C++ 文件用 `clang-format`（CI 固定 18.1.8）格式化；
  新增 shell 脚本通过 `bash -n`。
- 不要修改 `extern/` 子模块；TinyCC 修复放在 `patches/` 并由构建脚本应用。
- 设备端改动后优先跑 `./dejavu test`；涉及 Loader 变化才软重启。
