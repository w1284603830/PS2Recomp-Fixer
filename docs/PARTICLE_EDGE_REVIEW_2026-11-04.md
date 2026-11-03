# Issue #3 粒子配置隔离改造审查 — 2026-11-04

基线：公开 main `05807db13681af24443e6514602b028fcd622e9e`，源树
`db104dbd9db2ad4aa92b57fcd5d2f44030a4006d`。本地父提交 `ea949e9` 与该 main 源树相同。
范围是逐游戏配置隔离及必要的诊断、测试配套；没有证据表明其他游戏已经出现可见粒子错误。
本候选尚待批准，未提交 PR、未合并、未部署游戏 EXE/DLL。

## 具体修改

| 位置 | 修改及边界 |
|---|---|
| `GS/ParticleEdges.h` | 默认无 profile；严格匹配 `game_id`、`game_revision` 和 `particle_edges_profile`，未知、缺失、错误及重复配置关闭修正。HPO 和环境变量不能选中 profile。 |
| `GS/ParticleEdgesTypes.h` | 共用绘制资格及数值变换；资格参数由 profile 提供。当前变换仅支持已实现的 HPO5 坐标关系，其他 HPO 需实现独立变换。 |
| `GS/ParticleEdgeProfiles/DoD_SLPM66275.h` | 独立保存原公开实现的 HUD 深度、纹理来源、双线性／深度测试、STQ Q 和 mip inset 参数，不把它们作为通用默认值。 |
| `Bridge.cpp` | 新会话、关闭及打开失败清理 profile、环境快照、A/B 帧标记、绘制诊断和待处理擷取请求；失败路径回滚。重复 Open 不破坏仍在运行的会话。 |
| `GSRendererHW.cpp/.h` | profile 门槛先于 HPO／draw 条件；选中后才调用共用变换。保留各项排除理由，日志附游戏／版本／profile；HUD 分类也读取当前 profile。 |
| 两份 `pcsx2gs_api.h`、runtime 擷取说明 | 注释同步，C API 布局／版本不变；取消“未关闭即开启”的推断，明确 A/B 基线也可能关闭。 |
| 示例、文档、补丁组 | 提供不会自动载入的参考 INI；默认 runner 不自动启用。runtime 捕获／API 说明的增量补丁加入顺序清单，保持上游补丁组一致。 |

《鬼武者3》没有被分配新 profile；同 HPO 不能继承《梦之曙光》的配置。`SLPM_662.75` 是公开参考
runner 的标识，不是 ELF 自动识别或哈希认证，也不代表已验证同序列号的所有执行文件、其他碟或版本。
移植者须核对实际 ELF／构建身份，再明确选择并独立验证。见 [配置说明](PARTICLE_EDGE_PROFILES.md)。

额外的最小防御检查仅拒绝不完整 sprite pair 或非有限 STQ Q；有效数据的变换没有改成新的采样算法。
`PS2_PARTICLE_EDGES=0` 保留强制关闭功能，旧名 `DOD_PARTICLE_EDGES=0` 继续有效；二者同时存在时沿用
既有 `PS2_` 优先规则。环境值在每次 Open 重新读取，`1` 不能越过 profile 门槛。

## 分项验证结果

| 验证类别 | 结果 | 实际完成的内容与限制 |
|---|---|---|
| 静态审查 | PASS | 核对公共基线、profile 门槛顺序、独立参数、失败回滚、会话清理、force-off、诊断语义及 API 注释。两份 API 头文件逐字节相同。代码审查不是画面验收。 |
| 人工模拟 | PASS（合成驱动） | 手动运行并检查下方合成会话轨迹；没有启动窗口或游戏，没有 GPU 画面，不能称为人工视觉验收。 |
| 实际编译 | PASS（无界面测试程序） | GCC 13.3.0、C++20、UBSan 实际编译 profile 头文件、从生产文件提取的渲染门槛／变换和 Bridge Open／Close／reset 函数体；平台／GS 类型和服务由测试桩替代。不是完整 GS 翻译单元或 DLL 编译。 |
| Windows GS DLL 完整编译 | BLOCKED | 当前远端设备离线，本地没有 MSVC、Windows SDK、CMake 或 Windows 交叉编译器；未构建、部署新 DLL。 |
| 自动化回归 | PASS | 全库 91 项，其中粒子专项 13 项；上游补丁组反向／正向套用后逐文件字节一致。完整命令使用 `ASAN_OPTIONS=detect_leaks=0`，仅因受跟踪执行环境限制关闭泄漏扫描，地址／未定义行为检查保留。 |
| 真实游戏验证 | NOT_RUN | 未运行商业游戏、真实顶点／GIF 回放或 GPU 输出；粒子、HUD、alpha/depth、RT／镜面及 mip 采样的画面效果均未重新验收。 |

手动合成会话输出：

```text
SYNTHETIC ONLY: no game, GPU or visual acceptance
unconfigured/HPO5: profile-off
explicit reference: applied
other game/same HPO: profile-identity-mismatch; stale capture=0
failed GS open: profile-off
```

兼容性差异测试用冻结的公开旧函数作为独立基线，对比 **12,000 个合成 draw** 的判断、顶点字节与填充矩形。
其中 **3,184 个实际执行变换**：1,586 个 FST、1,598 个 STQ，2,104 个 mip、1,080 个 base-only。
测试断言这些路径确实被覆盖，避免全被跳过却判通过。它证明所覆盖输入的数值兼容性，不证明真实画面相同。

自动化还覆盖无配置、有效配置、空值／重复／未知／身份不符、跨版本和同 HPO 不继承、force-off 优先级、
20 轮连续会话切换、失败／空／重复 Open、旧 A/B 请求清理、draw／顶点排除、独立参数及不支持的变换。

复现命令：

```sh
python -m unittest discover -s tests -p test_particle_edges.py -v
ASAN_OPTIONS=detect_leaks=0 python -m unittest discover -s tests -v
```

## 发布前仍需补齐

1. 在具备工具链的 Windows 环境完整构建 GS DLL，确认新头文件及真实 GS 类型、日志格式和链接均通过。
2. 为每个准备启用的游戏／版本固定 ELF、EXE/DLL、profile、资源及输入状态，按
   [完整回归流程](REGRESSION_TESTING.md) 验证原生／放大输出、粒子／HUD／字型、atlas／mip、alpha／depth
   及 RT／镜面／后处理。确认目标 draw 确实走到 `applied`；必要时采用冻结状态回放。
3. 不用一作的通过代替其他游戏／版本，不把 UV inset 宣称为遮挡或深度修复，不把相邻帧 A/B 当作冻结回放。

本次请求批准的内容是代码隔离与上述测试、文档，不是对未运行的真实游戏画面作通过声明。
