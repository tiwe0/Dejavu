---
name: dejavu-java-probe
description: 使用 Dejavu 主动调用 Java 方法来核对重载、参数转换、返回值或异常行为，适用于已知类和方法签名的静态调用、实例调用及对象链路分析。
---

# 核对 Java 方法行为

先确定目标进程、类名、方法名、完整 JNI 签名和要验证的行为假设。依据源码或已有反编译结果确认重载及调用是否有副作用，再选择输入和可观察结果。从仓库根目录使用 `./dejavu exec -p PROCESS` 或 `./dejavu java -p PROCESS` 执行，替换 `PROCESS` 为本次目标。

1. 用完整 JNI 签名选择方法。例如 `(Ljava/lang/String;)I` 接收一个 String 并返回 int，构造器返回类型为 `V`。类名字符串作为 `java.invoke` 的首参时执行静态调用，实例方法需传入对象句柄。基本类型按签名转换，`I`、`J` 传整数，`Z` 传布尔值，对象与数组参数传有效句柄或 `nil`。
2. 先用无外部副作用的标准类验证调用，再用同样的参数与返回值检查方式验证待分析方法。以下 Lua 在一次执行中创建实例、取得字符串、调用静态方法并释放对象，预期返回 `123`。

   ```lua
   local builder = java.new("java.lang.StringBuilder", "(Ljava/lang/String;)V", "123")
   local ok, value = pcall(function()
       local text = java.invoke(builder, "toString", "()Ljava/lang/String;")
       return java.invoke("java.lang.Integer", "parseInt", "(Ljava/lang/String;)I", text)
   end)
   java.release(builder)
   if not ok then error(value) end
   return value
   ```

3. 根据结果核对假设。String 返回普通 Lua 字符串，其他对象和数组返回整数句柄，`V` 返回 `nil`。Java 抛出的异常会转换为包含异常描述的 Lua 错误。缺少方法、类型转换失败和已释放句柄也会报告 Lua 错误，分别检查签名、输入和句柄所属进程后再调整调用。同步调用会占用控制线程直到方法返回，客户端超时后先核对执行状态。
4. 对自有测试 DEX，`java.load_dex(device_path)` 返回 ClassLoader 句柄，后续用 `loader .. ":" .. class_name` 选择该加载器中的类。确认文件是目标可读的原始 DEX。普通类解析使用应用 ClassLoader。结束时释放自己创建的对象、数组、中间返回对象和加载器句柄。

句柄对应进程内 JNI global reference，跨 CLI 调用可复用，目标进程退出或 `java.release` 后失效。对象和数组最多同时保留 1024 个句柄，数组本身作为不透明句柄传递。加载器句柄释放后，已加载的类仍可能存活。保存每个中间对象的句柄，保证后续能够释放。[主动调用契约](../../zygisk/docs/java-invoke-api.md) · [实现](../../zygisk/src/java_invoker.cpp)

完成时记录目标 PID、类与完整签名、最小输入、预期和实际返回值或异常，以及创建句柄的释放结果。若目标方法会阻塞或改变业务状态，保留实际观测到的结果与未确认部分，避免用重复调用代替证据。
