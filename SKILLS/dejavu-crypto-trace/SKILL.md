---
name: dejavu-crypto-trace
description: 使用 Dejavu 定位自有测试操作经过的 Java JCA 或原生 libcrypto 路径，验证摘要、加解密等已知测试向量的调用与结果，适用于区分算法输入差异、Provider 路径和追踪未命中。
---

# 加密调用与测试向量对照

准备精确进程名、可重复执行的测试操作、已知输入与期望输出，以及已有的算法和 Provider 信息。观察窗口使用开发者控制的测试数据。先明确需要调用命中、计数还是合成数据的输入输出对照，按这个范围选取观测点。[基础操作与 ABI](../../SKILL.md)

## 定位调用层

结合测试代码选择 Java JCA 或原生入口。Java 示例覆盖 `MessageDigest`、`Mac` 和 `Cipher` 的部分字节数组重载。原生示例尝试安装已加载 `libcrypto` 中的一组 EVP/HMAC 等符号。记录实际安装的入口或 Hook ID，并保留原有 Hook 清单，便于结束时清理本次操作。[Java 追踪示例](../../examples/algorithm/java/algorithm_java.lua) · [原生入口实现](../../examples/algorithm/plugin_entry.cpp)

需要原生观测时，按插件说明加载 `algorithm`。选择用户要求的输出，纯计数可把路径设为 `/dev/null`，实时输出可使用空路径加流传输参数，持久记录使用用户指定路径。原生 `emit` 支持文件或流，`status` 提供 `hooks`、`events` 和 `bytes`。[插件调用说明](../../examples/algorithm/SKILL.md)

现有原生追踪器会生成输入输出及部分参数内容，Java 示例直接写 logcat。需要仅保留内存或计数时，采用符合该要求的输出路径和观测点。把原示例用于合成测试前，先核对其实际字段与输出位置。[原生输出实现](../../examples/algorithm/plugin_entry.cpp) · [Java 日志实现](../../examples/algorithm/java/algorithm_java.lua)

## 验证观测路径

1. 原生插件启动后记录 `algorithm.status()`。`hooks` 是安装数量，启动成功可能只覆盖部分候选符号。
2. 运行 `algorithm.selftest()`，记录其返回结果和事件差值。它直接通过解析到的 EVP 摘要接口计算 `abc` 的 MD5，预期为 `900150983cd24fb0d6963f7d28e17f72`。摘要正确且预期摘要事件出现，才能同时证明计算和这条观测路径可用。
3. 再单独触发目标测试操作，对照调用阶段、输入长度、算法名称和期望输出。自检和目标操作分开记录，避免把自检事件计入目标结论。
4. Java 路径先核对测试代码调用的具体重载，再触发一次已知操作。若基类入口未命中，检查实际 Provider 实现和 ART 分派路径，依据源码确定需要观测的具体方法。[自检及候选符号](../../examples/algorithm/plugin_entry.cpp) · [Java 方法签名](../../examples/algorithm/java/algorithm_java.lua)

## 解释结果

- `events` 统计生成的日志记录，`bytes` 是这些记录的字节数，包含字段和换行。一次测试操作可产生多条记录，原始输入长度应读取相应事件的长度字段。
- 原生样例把每个数据字段最多编码为前 1024 字节，超过时附加 `...`。算法名依赖所解析库提供的名称接口，字段缺失或截断时标明证据范围。
- 自检只验证所用库的摘要路径。目标操作可能使用其他重载、Provider、原生库或硬件实现，依据实际命中结果说明覆盖范围。
- `algorithm.start` 的“无可 Hook 符号”需要结合模块加载、符号解析及 Hook 安装错误判断。当前宿主会尝试调整并恢复代码页权限，按实际错误排查。[追踪实现](../../examples/algorithm/plugin_entry.cpp) · [当前权限处理](../../zygisk/docs/root-helper-prototype.md)

## 完成与清理

形成“测试操作 → 已命中的入口 → 已知测试向量结果”的证据链，并列出尚未观测到的层级。结束时停止原生追踪，再卸载本次加载的插件并检查结果。原生示例的停止函数会清空自身 Hook 列表，宿主卸载结果用于确认剩余受追踪 Hook 的清理。Java 追踪按本次返回的 ID 逐个 `hook.uninstall(id)`。按用户约定处理本次生成的输出。[停止实现](../../examples/algorithm/plugin_entry.cpp) · [宿主卸载](../../zygisk/docs/plugin-api.md#unloading)
