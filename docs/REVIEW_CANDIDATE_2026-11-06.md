# 提交审查：DoD PR #8 适配与通用错误修复

当前 GitHub main 已再次核对为 `c63d1333d291e52055219177801d106f7a35b184`。
2026-11-06，用户已批准提交以下两组修改并执行云端验证。用户电脑未用于编译／测试，未部署游戏。
以下为提交前验证快照；本次提交的 Windows 与云端检查结果以对应 PR 的 Checks 为准。

## 可分别审阅的两组修改

| 补丁 | 具体范围 | 行为与默认值 |
|---|---|---|
| 《梦之曙光》参考入口 | `examples/dod_runner/main.cpp`、`render_profile.h`、模板、pack 准备脚本、测试与文档 | 原 GameIndex 选择留在参考入口；版本断言＋实际 ELF CRC 匹配后，才允许显式参考粒子／UI／HD-CG 快捷配置。默认关闭。无共享 Host／GS 修改。 |
| 先前全库审查的通用修复 | VFS、EE pointer spans、VU／GS／MC／SIF HLE、VAG、HD 匹配、测试与补丁组 | 修复已重现的文件清空／权限／seek 错误与明确越界风险；不新增游戏地址、深度、atlas 或素材默认值。 |

[DoD 功能前后对照与全部配置](DOD_RUNNER_PR8_ADAPTATION.md)

[通用错误复现、修复与影响范围](REPOSITORY_ERROR_REVIEW_2026-11-06.md)

两个补丁可独立套用到当前 main。组合时只需合并测试 README 的新增说明段；本地组合候选已保留两段并验证。
旧 GIF 半行、CG cadence 和 per-buffer 比例分类片段仍未运行时接线：缺少完整已核对 hook 集合与当前游戏证据，
不能自动 include 后宣称恢复。本次交付的可验证配置适配不会替代这些单独的移植／效果验收工作。

CRC32 固定由移植者独立核对过的文件，程序不据文件名／HPO 判断版本，也没有虚构官方 release CRC 清单。
若没有确认 SLPM_662.75 与匹配 CRC，参考增强请求会在初始化前拒绝。原始 particle profile 配置也遵守该门槛，
强制关闭仍为 `PS2_PARTICLE_EDGES=0`。其他版本／游戏需自己的资料与资格。

## 验证结果

| 验证层 | 结果 |
|---|---|
| 静态比对／代码审查 | PASS：对比 PR #8 前后实际源代码；共享默认、profile 边界、完整长度、失败路径与配置优先级已核对。 |
| DoD 专项 | PASS：19 项，含实际 INI／相对路径／preflight 函数体及初始化前拒绝边界、独立 zlib CRC 对照和不覆盖素材清单。 |
| 通用专项 | PASS：28 个 storage／memory／VAG 案例；neutral／HD 专项增至 28 项。旧 VFS 的三个失败已用相同合成夹具重现。 |
| 完整组合回归 | PASS：168 项，GCC C++20、ASan／UBSan／float-cast-overflow；55.5 秒。仅因工具执行环境限制关闭 leak scan。 |
| 实际源文件编译 | PASS：真实 VFS／ROM／path／audio translation units；完整 VU／GS／SIF／MemoryCard 的 Linux 语法检查。HLE 与 runner preflight 用记录服务编译实际函数体。 |
| 补丁／API／工具 | PASS：上游有序补丁反向／正向逐文件一致；两份 GS C API 头相同；Python 编译检查、文档链接及补丁 apply --check 通过。CTest 临时目录包装重复运行通过。 |
| Windows 构建 | 提交前 NOT_RUN：本提交启用 GitHub-hosted GS／boundary jobs。以本次提交的 Checks 为准，不能沿用旧 main 的结果。 |
| 完整 DoD EXE／真实游戏 | NOT_RUN：本库不含商业 ELF 与完整生成码；未部署 DLL／EXE，未验收粒子、UI、CG、比例、听感或物理输入延迟。 |

提交与 CI 固定到本次审查的源树，云端结果单独记录；真实游戏／版本验收仍依 [完整回归指南](REGRESSION_TESTING.zh-TW.md) 独立进行。
