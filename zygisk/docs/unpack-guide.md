# Hook 脱壳方案（dlopen → OpenCommon / RegisterDexFile）

本文描述 Dejavu 的 hook 型 DEX 脱壳方案，入口是统一 CLI：

```sh
./dejavu unpack -p <进程> --out <设备可写目录>
./dejavu unpack -p <进程> --status
./dejavu unpack -p <进程> --disable
```

## 架构

1. **提前 enable**：CLI 通过 root 设置 `debug.dejavu.unpack.dir` 并重启目标
   进程。Agent 在 `HookManager::initialize()`（postAppSpecialize，应用代码
   运行前）读取该属性，自动 `unpack.enable(dir)`。
2. **dlopen hook**：Agent 用 Dobby hook `libdl.so!dlopen` 与
   `android_dlopen_ext`，记录库加载事件；若之后才加载 `libart.so`，在回调里
   补装 ART hook。
3. **ART hook**：
   - `ClassLinker::RegisterDexFile`（以及 ROM 存在时的
     `RegisterDexFileLocked`）：从 `DexFile&` 对象读取 `begin_`/`size_`，
     重建 DEX 镜像并写入输出目录。
   - `DexFile::OpenCommon`（如果 libart 符号表里存在）：用 8 参数透传 hook
     检查参数指向的 DEX 缓冲；部分 ROM（如本机 Android 15 Settings）该符号被
     strip，会退化为只走 RegisterDexFile。
4. **指针 tag**：ART 传参带 top-byte tag（`0xb4`），访问内存前统一
   `addr & 0x00FFFFFFFFFFFFFF`。
5. **过滤**：默认跳过 file-backed 映射的系统/应用 dex（可从 APK 离线提取，
   且全量 dump 会让启动变慢甚至 ANR）；只 dump 匿名内存（`[anon:dalvik-DEX
   data]` 等）中的解密 DEX。Lua 可 `unpack.include_file_backed(true)` 关闭过滤。

## 真机验证（OnePlus PKX110 / Android 15）

- `unpack.enable` 成功安装 dlopen hook 与 RegisterDexFile/Locked hook
  （`opencommon=0x0`，该 ROM 无符号）。
- 手工用 `DexClassLoader` 加载一份从 APK 提取的 30,217,436 字节 DEX：
  hook 捕获并 dump，`register_dexfile_000.dex` 与源文件逐字节一致
  （`DEX_DUMP_IDENTICAL size=30217436`）。
- 自动早期 enable + 重启 Settings：捕获启动流程，默认过滤下
  `skipped_file_backed=446`、只 dump 3 个匿名内存 DEX，进程存活；
  关闭过滤时会 dump 全部系统 DEX，曾导致 Settings 输入 ANR（因此默认过滤）。
- 输出目录示例：
  `/data/user_de/0/com.android.settings/files/dejavu-unpack`。

## 限制与后续

- 仅 arm64、API 26-35；符号/ABI 随 ROM 变化，需要真机验证。
- 加固（VMP、抽取壳、按需解密）需要结合类加载遍历与多次 dump；当前是
  "DEX 注册时刻" 的捕获层。
- 只用于自有应用/研究设备；错误签名或高负载时 dump 可能导致崩溃/ANR。
