# 场景技能

这里维护 Dejavu 常见场景的可复用技能。基础操作、设备约定和 ABI 说明见根目录的 [SKILL.md](../SKILL.md)。

| 场景 | 典型问题 | 技能 |
| --- | --- | --- |
| 方法调用追踪 | 点了一次按钮或触发了回调，实际执行了哪个已知方法？ | [dejavu-method-trace](dejavu-method-trace/SKILL.md) |
| 网络请求排查 | 测试请求是否经过目标进程，TLS 收发是否命中，数据应该在哪一层解释？ | [dejavu-network-debug](dejavu-network-debug/SKILL.md) |
| 加密调用路径分析 | 测试操作走 Java JCA 还是原生 libcrypto，观察结果能否与测试向量对应？ | [dejavu-crypto-trace](dejavu-crypto-trace/SKILL.md) |
| Java 主动调用验证 | 一个方法对指定输入返回什么，对象状态和类型转换是否符合预期？ | [dejavu-java-probe](dejavu-java-probe/SKILL.md) |
| Native 行为分析 | 哪个已加载库实现目标功能，函数是否被调用，参数和 ABI 是否匹配？ | [dejavu-native-debug](dejavu-native-debug/SKILL.md) |

根据问题选择一个场景技能，使用会话中指定的目标和测试操作。技能给出取证与判断方法，具体 API 和生命周期链接到项目文档。

每个技能使用独立目录，入口文件为 `SKILL.md`。

```text
SKILLS/
└── <skill-name>/
    └── SKILL.md
```

目录名使用小写字母、数字和连字符。入口文件的 YAML frontmatter 包含与目录名一致的 `name`，以及说明技能用途和适用场景的 `description`。

正文记录该场景需要的输入、操作步骤、验证方式和完成条件。共用的命令与接口约定链接到根目录指南或专题文档。内容以当前源码和测试为准。需要脚本、参考资料或素材时，在技能目录内增加 `scripts/`、`references/` 或 `assets/`。
