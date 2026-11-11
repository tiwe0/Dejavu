---
name: dejavu-method-trace
description: 用 Dejavu 追踪一次应用操作是否进入指定 Java 或 Kotlin 方法，核对重载、调用阶段和返回结果。适用于页面行为定位、回调未触发及业务路径验证。
---

# 方法调用追踪

把一次可复现的应用操作与具体方法的执行对应起来，记录进程、方法签名、触发动作和命中证据。

## 准备目标

从已有代码、日志或测试样本确定进程名、类名、方法名和 JNI 签名。重载使用完整参数及返回类型，Kotlin 编译产物按实际 JVM 方法确认。使用会话中选定的设备和目标，确认该进程已启用注入且 Lua RPC 可用。[基础操作](../../SKILL.md)

## 先验证入口

在目标的持久 Lua 会话中安装一个带本次测试标记的入口追踪。以下用 Activity 生命周期方法演示，实际操作时替换为已确认的方法。

```lua
assert(dejavu_method_trace_id == nil, "a method trace is already recorded in this session")
dejavu_method_trace_id = hookx.trace(
    "android.app.Activity", "onResume", "()V",
    {message = "dejavu-method-trace-test-01"})
return dejavu_method_trace_id
```

用 `hook.list()` 核对安装记录，触发一次目标动作，再查看 `./dejavu log --dump` 中本次标记及目标 PID。`hookx.trace` 记录方法进入，日志写入设备 logcat。重跑前先检查已保存的 ID，避免遗失当前会话的清理入口。[追踪实现](../../zygisk/config/hookx.lua)

## 根据问题增加观察

- 入口有记录而预期结果缺失时，核对原方法是否抛出异常。原方法抛异常后会跳过 `after`，并传播原异常对象。
- 需要整数返回值时可使用 `hookx.log_result_int`，参数为 `String` 时可使用 `hookx.log_string_arg`。先确认类型和索引，再按本次问题选取必要字段。这些 helper 也写入 logcat。
- 需要同时关联参数与结果时，编写同一 Hook 的 `before_hook` 和 `after_hook`。回调使用当前线程的借用引用，记录需要的数据后及时返回。
- 未命中时先核对进程、重载、实际声明或实现方法、ClassLoader 和触发时机。进程启动脚本在应用绑定后执行，需要较早的调用可能已发生。[Hook 示例](../../zygisk/docs/cookbook.md) · [回调契约](../../zygisk/docs/hook-c-api.md) · [启动时机](../../zygisk/docs/early-hooks.md)

## 收尾与结果

完成观察后，使用实际保存的 ID 清理本次追踪。

```lua
if dejavu_method_trace_id ~= nil then
    hook.uninstall(dejavu_method_trace_id)
    dejavu_method_trace_id = nil
end
return hook.list()
```

`hook.uninstall` 成功后释放该 Hook 的回调代码。`hook.remove` 用于逻辑禁用，`hook.clear` 会逻辑禁用全部用户 Hook。共享会话中按本次 ID 清理。[生命周期](../../zygisk/docs/hook-c-api.md#hook-lifetime)

最终给出命中的方法与触发动作、观察到的阶段或结果、仍未覆盖的路径，以及本次 Hook 的清理结果。入口日志只能证明该方法被执行，完整调用栈需要另外设计采集。
