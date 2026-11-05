# 《梦之曙光》参考入口的 PR #8 适配（2026-11-06）

比对基线：PR #8 前 main `cc3b5e17f43254db8a06c829172b14b7014e9d52`，合并后 main
`c63d1333d291e52055219177801d106f7a35b184`。本补丁只修改 `examples/dod_runner/` 与对应测试、说明；
没有把游戏地址、素材路径或渲染规则放回共享 Host／GS。本次修改已展示并获用户批准提交。

## 功能差异与处理

| 功能 | PR #8 前后核对 | 本次适配 |
|---|---|---|
| GameIndex 四项设置 | 参考入口仍保留 blending=2、HPO5、native_scaling=2、bilinear_upscale=2；共享默认已取消 | 保留在参考入口配置层，维持 INI → 环境变量的覆盖顺序。 |
| 粒子 | PR #7 已默认关闭；缺少 profile 是既定安全行为，不能称为 PR #8 导致的画面回归 | 新增明确选择入口；仅在人工确认 SLPM_662.75 且匹配所记录 ELF CRC32 时，才生成对应身份／profile 三元组。无配置仍关闭，强制关闭开关继续由 GS 执行。 |
| GS 标题 | 共享代码不再虚构 DoD 标题／序列号 | 参考入口明确传 `game_title`；纹理目录仍由该移植提供。 |
| DDS 排除 | 共享编译期清单移除；新 loader 读取素材包自己的 `texture_exclusions.txt` | 提供显式准备脚本，默认 dry run；需确认版本及 DoD_HD_CN 参考包。仅创建清单，不改纹理，不覆盖已有不同清单。其他包先独立审计。 |
| 高清静态 UI | 不再自动读取 EXE 旁 `ui_hd`；必须选格式、目录 | `ui_hd_dir` 非空且版本／CRC 确认后，传 `page-rgba-512x448-x3-v1` 及实际目录；预先要求 `pages.tsv`。空值保持关闭。 |
| 高清 CG 缩放 | 共享层不再自动强制最低 7×、临时关闭 NativeScaling | `reference_hd_cg_scaling=1` 可显式保留旧参考策略，需同样确认版本／CRC；它不负责安装 MPEG／生成码 hook，也不自动启用粒子。 |
| 画面比例 | 共享按 framebuffer／深度推断 world、UI、CG 的规则已移除；16:9 输出选项仍存在 | 保留用户显式 aspect 选择；日志改为说明当前只是输出比例，投影 hook 必须由移植具备，不再宣称已恢复旧 per-buffer 行为。 |
| GIF 半行修正 | 原共享传输会改写 XYOFFSET；现保留原始数据，旧片段归档 | 不自动接回。归档片段缺少独立身份／会话接线与当前画面证据；要保留须另作受版本保护的移植 adapter，测试同步／异步两条路径。 |
| CG cadence／bank 选择 | 原共享逻辑包含固定 buffer 与两场节奏；现仅保留观察机制 | 不自动接回。当前库不包含完整私人生成码 hook 集合，不能仅 include 归档头就宣称恢复。须核对 native／HD／fallback 生命周期及真实 GIF／输出后独立处理。 |
| 测试导航／frame clock | PR #8 移除默认 START/CIRCLE、40 秒及隐含 60 Hz | 此入口没有重新安装自动操作。测试者显式提供 boot sequence、frame 地址和测得频率；具体反例见通用迁移指南。 |
| Alpha／A-B 诊断 | PR #8 已修复分离 Alpha，并以多帧两半采样 | 保留底层新诊断；本参考不缩回单对画面。需要 Alpha 时显式加 dump_alpha=1，按 draw／buffer／ROI 核对，不能据截图数量判通过。 |
| VU 与队列 | kzvu 默认已中性化，但 DoD 的安装函数本来就显式使用 clamp=1、IbitHack=off；队列仍默认 2 | 该参考 VU 配置不变；不从其他游戏自动套入 1-field 队列或新的低延迟 preset。 |
| feedback／clamp 新选项 | 共用层新增独立、默认关闭的选择机制 | 不自动加到 DoD 参考配置；它们不是粒子总开关，需要这个版本自己的资格与画面证据。 |
| 输入／VBlank 新机制 | 新接口是独立读者与可选提交阶段，不会自动成为 DoD 原生 pad ABI | 本次不添加原生 pad hook，不更改游戏时钟，不把 Oni3 的版本数据或 End 策略移植给 DoD。 |

后两项历史渲染逻辑与“只增加配置即可恢复”的功能边界不同。本补丁交付配置适配与明确的缺口记录，
不宣称旧游戏画面已完整恢复，也没有证据断言当前安装版本已出错。现有 `dod.ini` 仍可能提供额外配置。

## 版本确认与使用

参考入口需要自己的生成码、注册表及既有游戏专属辅助文件。将 `render_profile.h` 与现有
`game_profile.h`、`widescreen_reference.h` 一并放在该移植的 runner 旁；本库没有商业 ELF 或完整生成码，
不能把配置测试称为完整 DoD 程序构建。

`pr8_adaptation.ini` 是供手动合并进 `dod.ini` 的模板，不会自动载入。默认增强全关：

```ini
confirmed_game_revision=
confirmed_elf_crc32=
reference_particles=0
reference_hd_cg_scaling=0
ui_hd_dir=
```

先自行核对合法 ELF、版本、生成码和参考 profile，再填写 `confirmed_game_revision=SLPM_662.75`，
以及该 ELF **完整文件的 CRC32，严格八位十六进制，不带 0x**。入口验证 ELF32／小端／MIPS 可执行格式，
并重新计算 CRC；丢失、格式错误或不匹配会在窗口、音频及 GS 配置前拒绝该增强请求。

**这里没有内置可信发行版 CRC 清单。** 版本字段是移植者独立核对后的明确断言；CRC 只固定所核对文件，
不作官方身份认证、密码学认证或画面验收。文件名被改成 SLPM_662.75、HPO 相同或勾选参考配置都不构成版本证据。
其他光盘、合盘修改版、补丁版、SLPM_662.76 与《鬼武者3》不能自动继承此参考增强。

可在自己的隔离副本计算 CRC32：

```sh
python -c "import pathlib,sys,zlib; print(format(zlib.crc32(pathlib.Path(sys.argv[1]).read_bytes()), '08x'))" /path/to/verified.elf
```

此命令只计算文件摘要，不证明版本适配；不要把测试夹具的 CRC 当成游戏 CRC。

确认后按需要单独开启：

```ini
reference_particles=1
reference_hd_cg_scaling=1
ui_hd_dir=ui_hd
```

`ui_hd_dir` 相对 EXE 解析；它不是默认素材位置。所选格式要求 preview 512×448、HD 1536×1344，
`pages.tsv` 列及 Alpha 约束遵循通用迁移指南。显式 UI 路径选择后，`PS2_UI_HD_DIR`／旧名 `DOD_UI_HD_DIR`
仍可覆盖，预检检查实际覆盖目录；单独设置环境路径不会自动选择格式。
UI 预解码差异最初用于**高清纹理移植优化**，每个包仍需自己的清单、内存预算及画面验证。

既有原始 `gs_options` 粒子三元组可以保留，但也须提供上述版本／CRC 确认；不要再同时设
`reference_particles=1`，否则身份／profile 来源冲突会拒绝启动。`reference_particles=0` 表示不使用快捷 preset，
不是覆盖原始 GS 选项的强制关闭。需要强制关闭时使用 `PS2_PARTICLE_EDGES=0`，其旧名与优先级保持不变。
同样，`ui_hd_dir` 与原始 static-page 格式／目录选项不可混用。原始图像／素材 GS 选项仍由使用者明确提供并按底层契约验证；日志中的 UI／CG preset 状态仅指本次快捷配置。普通 GS 设置仍由用户后置覆盖，HPO 改为不支持值时
GS 必须按原 profile 门槛跳过，入口不会放宽粒子资格。

`PS2_GS_OPTIONS` 优先于 `DOD_GS_OPTIONS`，包括显式空值；适配层在 runtime 环境别名安装前也遵守这个顺序。
命令行缺少值会直接报错，不再悄悄忽略最后一个参数或保留部分覆盖。

## 准备参考素材包排除清单

只对已独立核对的 DoD_HD_CN 参考包使用，不按游戏名自动判断包兼容性：

```sh
python examples/dod_runner/prepare_texture_pack.py --pack /path/to/qualified/pack --confirmed-game-revision SLPM_662.75 --confirmed-pack DoD_HD_CN-reference
```

查看 dry run 后，加 `--apply` 才创建清单。已有相同内容时保持原样；已有不同内容时拒绝覆盖，请人工审查合并。
`.dds` 与同名修复 `.png` 是不同项，参考清单只排除 DDS。入口发现所选包没有清单只作提醒，不自动修改文件。

## 验证与验收边界

```sh
python -m unittest discover -s tests -p test_dod_reference.py -v
ASAN_OPTIONS=detect_leaks=0 python -m unittest discover -s tests -v
```

专项覆盖默认关闭、有效／未知版本、CRC 缺失／错误、原始配置、重复／冲突、强制关闭、独立 UI／CG 开关、
环境优先级、重复配置不继承、缺值参数、合成 ELF CRC 与独立 zlib 一致，以及准备脚本的 dry run／不覆盖行为。
测试实际编译生产配置层、从 main 提取的 INI／路径／预检函数体，并使用现有 GS profile 策略；确认拒绝发生在记录式初始化边界前。不需商业游戏资源，不打开 GPU／窗口。

- 静态审查：逐项核对上表与实际调用位置，确认共享默认没有改变。
- 自动化／合成验证：专项 19 项通过；最终组合候选的完整回归单独记录。合成 ELF 不被宣称为真实版本。
- 实际编译：提交前已运行 GCC C++20 / UBSan 配置夹具；Windows GS 与边界测试以本次 PR 的 Checks 为准。完整 DoD EXE 未构建。
- 真机／真实游戏：NOT_RUN。未在用户电脑编译，未读取或部署其 EXE／DLL、存档或素材。

真实验收须固定源码、ELF／EXE／DLL、profile、资源及输入，覆盖原版／增强、冷／热载入、所有相关粒子／HUD、
alpha／depth／RT、UI 静态页、4:3／16:9、native／HD／fallback CG 的开始／结束／中断、音画同步。
A/B 检查有效 draw、buffer、ROI 与两半内容，必要时用冻结 replay。出现差异沿 EE → VIF／VU → GIF → GS 找第一处分岔，
先修所属层，再跑直接回归和完整发版矩阵。参见 [完整回归指南](REGRESSION_TESTING.zh-TW.md)。
