# VFS 路径编码审查与修复（2026-11-06）

核对基线：PR #9 合并后的 main `33487e197a9eca562d61090d750895052ed238f0`，源树
`13ea89d44a69cdb2310a5248bacea18e0b35b62e`。用户已审阅并批准提交本补丁、执行云端验证。下表为提交前快照，提交后的实际云端结果见对应 PR 的 Checks；未在用户电脑编译。
没有访问真实存档、光盘、商业 ELF、素材或部署目录。未确认任何游戏的实际文件名编码，未声称其存档已出错。

## 路径来源与覆盖范围

| 入口 | 已核对的调用链 | 编码转换及范围 |
|---|---|---|
| EE `fioOpen` | `Kernel/Syscalls/FileIO.cpp`：`getConstMemPtr` → `const char*` → `vfs().open` | 直接传入游戏字节，未转码。`fioMkdir/Chdir/Rmdir/Getstat/Remove` 同样进入 VFS 的 resolve/stat。 |
| IOP 模块文件 | `iop_module_loader.cpp` → `translateGuestPath` → `PS2IopHostAdapter` → VFS resolve | 未转码；resolve 后仍有 `hostPath.string()` 和窄字符串 path 构造。模块 loader 还保留翻译失败时使用原路径的 fallback。VFS 拒绝不能代表该链整体拒绝。 |
| IOP 虚拟光盘 | `iop_cdvd.cpp` 的 `readString` → `parsePs2Path`／虚拟 ISO 查找 | 不经过 VFS open；读取宿主文件还有 `.string()` 传递。不能用 VFS 测试代替该路径验证，也不能更改真实 ISO 的原始名称字节来适配宿主。 |
| EE ELF loader | `Helpers/Loader.h` → `translatePs2Path`（`Helpers/Runtime.h`） | 另一条窄字符串路径，不是 VFS resolve；未发现旧编码转换。 |
| libmc 存档 HLE | `MemoryCard.cpp`：`readPs2CStringBounded` → `normalizeGuestMcPathLocked` → `guestMcPathToHostPath` → `openMcHostFile` | 独立于 VFS，仍逐字节替换反斜杠、窄字符串构造 path、`fopen(hostPath.string())`；目录枚举也使用 `.string()`。本补丁没有重写这套存档 ABI。 |
| runner 宿主配置 | generic／DoD `main.cpp` 设置 `IoPaths`；`dod_utf8.manifest` 选择 UTF-8 ACP | 处理宿主配置、进程代码页；不是游戏字节的 Shift-JIS／GBK／Big5 转码器。公开 runner 未安装旧编码转换。私人移植及当前已安装二进制不在本次证明范围内。 |

因此，`vfs.open("mc0:...")` 的合成保存测试只覆盖 VFS 文件接口；不等于实际游戏调用 `sceMcOpen`、目录枚举、
图标／标题或完整存档协议已通过。DoD 和 Onimusha 3 必须分别核对各自实际入口、版本和字节，不能凭名称选编码。

## 已复现的问题及风险

1. **无效 UTF-8 未在解析前统一拒绝。** Linux 的 filesystem 可以保留任意非 NUL 字节，旧 VFS 会接受原始 GBK
   `B4 E6 B5 B5 2E 62 69 6E` 并创建非 UTF-8 名称。这违反已经公开的 UTF-8 契约；Windows 的转换行为不能由这次
   Linux 结果推定。候选明确统一拒绝无效 UTF-8，而不是把它静默解释成系统 ACP。
2. **DBCS trail byte 被当成分隔符。** Shift-JIS 的 `表.bin` 是 `95 5C 2E 62 69 6E`。旧 VFS resolve 返回成功，
   后缀变成 `95 2F 2E 62 69 6E`，即两个路径组件。GBK 的 `乗`（`81 5C`）和 Big5 的 `兝`（`A2 5C`）有同类边界。
   问题发生在 `parsePs2Path` 的字节替换，早于 filesystem 转码；应先转码，再解析。
3. **合法 UTF-8 与旧编码字节会重叠，无法自动识别。** GBK `C2 A0` 表示 `聽`，UTF-8 表示 U+00A0；Big5
   `C2 A1` 表示 `癒`，UTF-8 表示 `¡`。本次在两个不同的合成目标间实际验证：默认 VFS 按 UTF-8 访问；明确选择旧
   编码的适配器得到另一名称。传错编码可能选错文件，包括截断错误目标的风险。有效性检查不能消除这种歧义，必须先
   由移植者独立确认编码，不能使用“先试 UTF-8，失败再试其他编码”的策略。

旧 main 的生产 VFS 已用同一新夹具运行：`sjis/gbk/big5` 的 raw-reject 断言均失败。所有文件都是临时生成的。
这些是共享契约缺口／合成风险证据，不是已发生的游戏损坏证据。

## 最小共享修复

- 新增 `runtime/ps2_path_utf8.h`：不依赖 locale 的严格 UTF-8 校验，拒绝 NUL、孤立／缺失 continuation、overlong、
  surrogate、超过 U+10FFFF 的序列。
- 在 `PS2Vfs::open/stat/resolveHostPath` 的入口、任何分隔符解析之前检查。失败不打开、创建、截断或保留旧 result。
- 保留 UTF-8 默认契约、原生宽路径／`_wopen`、文件标志、CD 只读和原有有效路径行为。**不安装自动转码、编码探测、
  系统 ACP fallback、游戏名映射或旧文件改名。** 输入本来是合法 UTF-8 时仍按 UTF-8 处理。
- 将实际源码增量放入新的有序 `PS2Recomp_path_utf8_20261106.diff`；保留已有补丁，往返逐文件校验已通过。

无效 UTF-8 在 POSIX 上以前可能碰巧可用，现在会明确失败；依赖该行为的移植必须先补适配，不应静默部署此修改。
这不是宣称所有旧编码文件名已经被通用层支持。

## 可选的显式移植示例

[`path_encoding_example.h`](../examples/generic_runner/path_encoding_example.h) 不被 runner 自动包含或安装。
选择项为 `Utf8`、`Windows932`、`Windows936`、`Windows950`；Windows 使用指定 code page 的 Win32 API，POSIX
示例使用 iconv，拒绝替换和非无损往返。不读取系统 ACP。Windows code-page 扩展和严格 Shift-JIS／Big5 并非处处
等价，不能据少量测试声明覆盖完整字符集；未知映射应返回不支持，由端口选择精确转换器。

在**已经确认编码且有界读取**的移植调用边界调用，不能先拆分路径：

```cpp
std::string utf8Path;
if (!port_paths::decode(boundedGuestPath, qualifiedPortPathEncoding, utf8Path))
    return pathError; // Use the port's validated guest error ABI.
return runtime.vfs().open(utf8Path, flags, mounts, runtime.romDevice());
```

`qualifiedPortPathEncoding` 由独立验证的端口资料给出；本补丁没有添加游戏编码设置。失败保留输出不变，调用者必须
检查返回值，不得继续使用旧输出或原字节 fallback。混合编码来源应分开适配，不能给整个游戏猜一种编码。

libmc 的双向文件名、固定长度目录项及 wildcard、路径诊断、宿主根目录、IOP 模块／虚拟 ISO、ELF loader 都需各自
验证和接线。对这些链仅调用本例 decoder 不等于完成修复；宿主路径应保持 path／UTF-16，窄字符串交界要明确 UTF-8。
不要转码存档二进制 payload、`icon.sys` 标题字段或容器内部名称来“修文件名”。不要自动迁移已有存档名称。

## 合成矩阵与验收分层

| 输入集 | 独立生成 | 夹具检查 |
|---|---|---|
| UTF-8 | Python `utf-8`：`存档.bin`、`表.bin` | 直接读取／创建／保存、非 ASCII 宿主目录、合法边界和无效序列 |
| Shift-JIS 共通子集 | Python `shift_jis`：`記録.bin`、`表.bin` | 显式 CP932 转码后访问；原始无效 UTF-8 拒绝；`95 5C` 不被拆分 |
| GBK 共通子集 | Python `gbk`：`存档.bin`、`乗.bin` | 显式 CP936 转码后访问；`81 5C`；合法 UTF-8 歧义 |
| Big5 共通子集 | Python `big5`：`存檔.bin`、`兝.bin` | 显式 CP950 转码后访问；`A2 5C`；合法 UTF-8 歧义 |

共享夹具编译实际 VFS／path／ROM translation units。每次独立临时目录，使用人工二进制内容，检查创建／EXCL、
CREAT 保留尾部、显式 TRUNC、读取／stat、CD ISO 后缀、ASCII 兼容、重复选择无状态、转换失败不修改输出、
宿主目录中无额外文件且 sentinel 内容不变。

| 层次 | 本候选结果 |
|---|---|
| 静态审查 | 已完成上表调用链、默认契约和 Win32 参数核对；独立旁路明确保留为后续适配范围。 |
| Linux 合成专项 | 27 项通过；Windows native path 1 项明确跳过。实际生产 VFS，ASan／UBSan，POSIX iconv。 |
| 完整自动化回归 | 195 项通过，1 项 Windows 专属测试明确跳过（共发现 196 项），62.7 秒；包含原有 168 项及本次专项。ASan／UBSan，因隔离执行器限制关闭 leak scan。 |
| Linux CMake／CTest | GCC Release 配置、编译及链接通过；55/55 CTest 通过，1.38 秒。其中 28 项为原有边界案例，27 项为本次编码案例，和 unittest 覆盖重叠。 |
| Windows／MSVC | 提交前 **NOT_RUN**。本提交将 28 项编码 CTest 加入现有云端 boundary job；实际结果见本次 PR 的 Checks。总计 56 项包含原有 28 项，不安装 UTF-8 manifest。 |
| 完整移植／真实游戏 | **NOT_RUN**。没有确认 DoD／Onimusha 3 编码，没有读写真实存档或商业文件，没有在用户电脑编译。 |

```sh
ASAN_OPTIONS=detect_leaks=0 python -m unittest discover -s tests -p test_vfs_encoding.py -v
ASAN_OPTIONS=detect_leaks=0 python -m unittest discover -s tests -v
cmake -S tests -B build/boundaries
cmake --build build/boundaries --config Release
ctest --test-dir build/boundaries -C Release --output-on-failure
```

Windows 云端使用 Visual Studio 2022、x64、Release；显式保留断言。Windows API、MSVC filesystem、ACP／manifest
组合以及 libmc／IOP 旁路仍须分别看真实结果。Linux iconv 或静态检查不能替代这些验证。
真实游戏需在隔离存档副本中固定版本与实际文件名字节，覆盖旧存档读取、新建、覆盖、目录枚举／重命名、失败回退及
重启加载，再依[完整回归指南](REGRESSION_TESTING.zh-TW.md)定位第一处分岔。

官方平台契约：
[UTF-8 process code page](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page)、
[MultiByteToWideChar](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar)、
[WideCharToMultiByte](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-widechartomultibyte)、
[code-page identifiers](https://learn.microsoft.com/en-us/windows/win32/intl/code-page-identifiers)。
