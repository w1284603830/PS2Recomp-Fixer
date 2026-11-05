# 代码复审与已确认错误修复（2026-11-06）

基线为 main `c63d1333d291e52055219177801d106f7a35b184`，源树
`6c2ea78f0d47c25e8c0c8c519ba28913c4f9a850`。本地父提交 `86b20b7` 源树相同。
本次复审遍历代码目录与构建／测试入口，重点追踪共享 Host、VFS、EE 指针、HLE 结构／DMA、音频输入及 HD 渲染。
没有逐行重新证明所有第三方指令、JIT、硬件与编码器实现；测试通过不表示全库无缺陷或所有游戏已验收。
本次修改已展示并获用户批准提交；下表保留提交前验证快照，云端结果以本次 PR 的 Checks 为准。

## 已重现或确认的错误

| 位置／触发条件 | 原行为 | 最小修复与测试 |
|---|---|---|
| VFS：WRONLY + CREAT 打开已有存档 | `wb` 意外清空已有内容 | 独立映射创建、截断与追加标志；测试打开 `abcdef` 后只写首字节仍保留尾部。 |
| VFS：写入专用句柄及缺失文件 | 内部 `r+b` 允许读取；append/truncate 模式可在没有 CREAT 时创建文件 | 使用原生二进制描述符，按访问权限操作；只在 CREAT 时创建，并测试只读／只写、缺失文件和连续读写。 |
| VFS：EXCL、Windows 大偏移 | 先检查存在再创建有竞态；Windows `long` seek 限于 32 位 | 原生 O_CREAT／O_EXCL 原子创建；64 位 seek。12 线程仅一个创建成功；3 GiB 位置测试不写大文件。 |
| 内存文件：CUR／END 加 INT64_MAX | 有符号加法溢出 | 加法前比较范围；失败保持原位置。 |
| VFS：disc 写入、路径后缀及编码 | CD 目录可被写；host／mc 的 `;1` 被当成 ISO 版本删掉；Windows narrow 路径依赖 ANSI codepage | CD 访问与 stat 明确只读；仅 CD 去 ISO 后缀；host 路径接口明确 UTF-8。测试全用临时合成文件。 |
| VFS：长会话 descriptor 计数 | signed 自增可溢出并碰撞 | 在 INT32_MAX 回到 3，跳过占用项；边界逻辑已静态审查，未执行数十亿次真实 open。 |
| `ps2ResolveGuestPointer` | 未处理的 segment 留下 phys=0，错误落到 RAM 开头 | 未支持映射返回失败；保留既有合法 RAM 镜像及 scratchpad 别名。它不是新的 TLB／硬件 MMIO 实现。 |
| VU／GS／MC 的 bulk HLE 访问 | 只检查起点，结构或字符串可能越过 backing allocation | 新增长度型 pointer overload；相关结构、向量、矩阵、包与字符串使用完整长度。边界失效不触碰 host 内存；SIF 逐字节镜像行为单独保留。 |
| SIF DMA descriptor／回传 metadata | 连续 memcpy 可跨 RAM 末尾；NULL metadata 会写低 RAM；scratchpad 尾部不完整结构可越界 | 使用现有 guest-range reader／writer；预检 metadata 完整范围；NULL 不写 metadata。测试跨镜像 descriptor 与尾部拒绝。 |
| VAG 大小／transfer | 损坏大小会超大 reserve、溢出块数或接受部分数据；`phys+size` 可回绕 | 分配前验证完整 16-byte blocks 与实际缓冲长度，使用减法边界检查；有效块预测系数与既有 SPU2／混响不变。 |
| HD CG 识别 | 纯色可匹配任意空 field；多个相同 native preview 会错误选择一个 HD frame | 纯色不作为身份；多候选保留 native upload；单一完整 RGB 匹配仍工作。 |
| HD 静态页浮点边界 | NaN／Inf 或巨大坐标可在检查前转整数 | 先检查有限值与 native 范围；带 float-cast-overflow sanitizer 覆盖真实候选路径。 |

旧 main 的真实 VFS translation unit 已用相同合成夹具复现三个失败：

```text
create-preserve: 原断言失败（文件尾部被清空）
permissions: 原断言失败（写入专用句柄能读取）
rom-overflow: UBSan 报 signed integer overflow
```

所有复现均在云端临时目录执行，没有读写用户存档、游戏部署目录或素材。

## 验证记录

| 类别 | 结果与界线 |
|---|---|
| 原 main 基线回归 | 118 项通过；说明已有覆盖没有检出上述新边界。 |
| 新边界专项 | 28 项通过；编译真实 VFS／ROM／path、音频 translation units 与提取的生产 HLE 函数，使用 ASan／UBSan。 |
| HD／neutral 专项 | 增至 28 项通过；包含纯色、多候选和非有限值。仅使用记录式设备，不是真实 GPU。 |
| 本候选完整回归 | 149 项通过；上游补丁反向／正向后逐文件一致。最终与参考入口适配的组合结果另行记录。 |
| 实际编译 | GCC C++20 的上述程序、完整 VU／GS／SIF／MemoryCard translation unit Linux 语法检查通过。 |
| Windows／MSVC | 提交前 NOT_RUN；新增 GitHub-hosted boundary job，Release 显式保留断言。实际结果以本次 PR 的 Checks 为准，不能沿用旧 main 的编译结果。 |
| 真实游戏／画面／听感 | NOT_RUN。未部署 EXE／DLL，未在用户电脑编译；没有新游戏兼容性或物理延迟通过声明。 |

测试命令：

```sh
python -m unittest discover -s tests -p test_storage_boundaries.py -v
python -m unittest discover -s tests -p test_neutral_base.py -v
ASAN_OPTIONS=detect_leaks=0 python -m unittest discover -s tests -v
```

PS2Recomp 源码增量保存在 `PS2Recomp_storage_bounds_20261106.diff` 并加入既有顺序清单；原大补丁不重写。
GS 修改单独留在其 vendored 源码。没有新增任何游戏地址、深度、图集或素材默认值。

## 采用后的直接回归

在各移植自己的隔离副本验证保存／加载与文件模式、带 `;数字` 的非光盘文件名、原有合法路径编码、SIF 收发与
scratchpad／镜像边界、涉及 libvu0／libgraph 的场景、VAG 加载／失败回退、HD CG 静态／重复帧与 UI 更新。
旧编码路径需由移植显式转 UTF-8；本补丁不猜测 Shift-JIS／Big5。
保守拒绝部分 ADPCM、纯色或无法唯一识别的 HD 候选，可能使这些输入回退或保留原生内容，须独立验收。
按照 [回归指南](REGRESSION_TESTING.zh-TW.md) 定位第一处分岔；不以防越界测试代替 SDK ABI 或画面正确性证明。
