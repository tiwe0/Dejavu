---
name: dejavu-native-debug
description: 使用 Dejavu 定位已加载原生库中的函数、主动调用已知签名函数，或以保持原行为的计数 Hook 验证调用路径，适用于原生函数是否执行及输入输出行为的分析。
---

# 观察原生函数调用

先确定目标进程、原生库、函数符号、真实 C 签名和触发操作。从源码、头文件或已有静态分析确认参数宽度、返回值、指针及调用约定，再编写调用包装器或替换函数。符号名只提供定位线索，函数签名需要单独确认。

1. 用 `mem.maps(library_name)` 核对目标进程的库映射，用 `module.symbols(library_name, filter)` 缩小符号范围，再用 `module.resolve(library_name, symbol)` 取得地址。库必须已加载。解析先尝试动态符号，再回退加载映像的 ELF 符号表，完全剥离的文件可能缺少所需符号。[模块与符号接口](../../zygisk/docs/agent-helpers.md#shared-libraries-and-symbols-module)
2. 主动调用使用 `native.call(module, symbol, source, ...)`。包装器定义 `void dejavu_native_call(void)`，从已确认的签名声明函数指针，并用 `dejavu_native_call_return` 返回 int64 结果。调用是同步的，先选择能用有限输入验证的函数。
3. 需要证明实际调用路径时，安装最小替换函数，显式调用 `dejavu_native_hit()`，再通过 `dejavu_native_original()` 转发原调用。记录安装前状态、返回的 Hook ID、触发前后计数及实际返回值。下面的完整 Lua 用 `libc.so` 的 `getpid()` 验证这一链路。

   ```lua
   local replacement = [[
   int dejavu_native_target(void) {
       dejavu_native_hit();
       return ((int (*)(void))dejavu_native_original())();
   }
   ]]
   local call = [[
   void dejavu_native_call(void) {
       int (*target)(void) = (int (*)(void))(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
       dejavu_native_call_return((long long)target());
   }
   ]]
   local id = native.install("libc.so", "getpid", replacement)
   local ok, result = pcall(function()
       local before = native.stats(id)
       local pid = native.call("libc.so", "getpid", call)
       local after = native.stats(id)
       assert(pid > 0 and after > before, "native probe did not observe the call")
       return string.format("pid=%d hits=%d", pid, after - before)
   end)
   native.uninstall(id)
   if not ok then error(result) end
   return result
   ```

   预期 PID 与目标进程一致，计数增量至少为 1。`native.stats(id)` 统计的是显式调用计数 helper 的次数，替换函数遗漏 helper 时计数会保持不变。其他线程也可能同时调用目标函数，增量可以超过 1。
4. 完成观测后卸载本次 Hook，并读取 `native.list()` 确认对应 ID 的状态。若安装或卸载报告权限恢复失败，保留报错中的 ID，检查 `protection-pending`，使用该 ID 重试 `native.uninstall(id)`。逐项清理自己安装的 Hook，保留其他分析任务的状态。

卸载恢复入口与保存的页面权限。已发布的 trampoline、JIT 代码和上下文保留到进程退出，允许已经进入的回调完成。重复安装和卸载会保留这些内存，目标进程结束后释放。回调中的自有状态需要另外同步，热路径保持短小。[Native API 与生命周期](../../zygisk/docs/native-hook-api.md#limitations-abi-1) · [实现](../../zygisk/src/native_hook_manager.cpp)

完成时报告库与符号、确认签名的依据、最小触发步骤、返回值与计数差、Hook ID 和清理状态。计数为零时，继续区分触发未发生、符号或进程选错、执行路径经过其他实现，以及计数 helper 缺失这几种情况。
