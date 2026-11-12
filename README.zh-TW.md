# PS2Recomp-Fixer：照 PCSX2 行為強化的 PS2Recomp 基底

[English](README.md)

這是 [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp)（commit `eb8688b`，PS2 靜態重編譯器）的分支。製作商業 PS2 遊戲《新鬼武者 DAWN OF DREAMS》完整 PC 移植時，順帶把它強化了：

- 新鬼本身有兩張光碟，含 3D、CRI Sofdec／ADX 影片，以及跑在 IOP 上的音效驅動；所有房間與影片都用自動巡檢跑過。
- 執行期、重編譯器與 IOP 共做了約 150 項修正，大多照 PCSX2 的行為修。
- 補上了上游執行期缺少的硬體模擬。

**這個倉庫不含任何遊戲。** 它是可重複使用的基底：重編譯器、執行期、IOP 模擬、PCSX2 的 GS 與 VU 模組、工具和測試。要移植遊戲，請使用你自己合法取得的遊戲。

**目標是讓任何移植者都能重用的通用基底。** 硬體與主機服務共用；遊戲／版本的位址、函式庫 ABI、渲染例外由各移植專案明確提供。粒子邊緣等修正對每一款遊戲都須獨立驗證。後續開發遵循 [AGENTS.md](AGENTS.md)，現況與來源核對見 [docs/REUSABLE_BASE.md](docs/REUSABLE_BASE.md)。

## 重點

- **PCSX2 硬體 GS**（DX11/DX12）包成 `pcsx2gs.dll`，在獨立執行緒餵資料。支援提高解析度、FXAA／SSAA、高清材質包非同步載入、獨佔／無邊框全螢幕。
- **PCSX2 microVU** 透過 `kzvu` 跑 VU1（與 VU0 micro mode），取代按週期預算截斷的 VU1 直譯器。
- **SPU2 模擬**：聲部、ADPCM、高斯插值、包絡、AutoDMA、殘響。在 IOP 上跑自己音效驅動的遊戲也有聲音。
- **SIO2＋DualShock 2**（原版手把驅動直接在 IOP 上跑），SDL2 手把含震動。
- **IPU**（IDEC/BDEC/VDEC/CSC）可播 libmpeg／Sofdec 影片；HLE MPEG 走 FFmpeg。
- IOP 跑在獨立執行緒，臨界區與 RPC 行為照真機核心。
- EE 核心：
  - 執行緒優先度、Suspend／Wakeup 的回傳值修正；
  - DI/EI 臨界區不被打斷，被遮罩的中斷保留狀態；
  - NTSC 59.94 Hz 真實時間 VBlank；
  - 中斷處理堆疊不再蓋掉遊戲堆疊。
- DMA／VIF／GIF：VIF1 UNPACK 語義、chain CALL/RET 深度、IMAGE 續傳、D_ENABLE、CHCR/STR、SPR chain。
- 重編譯器：MMI／FPU／VU0 巨集指令語義、64 位元分支比較、VU0 亂數指令。
- 光碟：真實 ISO9660 LBN、雙層 DVD9、換盤。
- 主機端：單一 deadline 限幀器；UTF-8 代碼頁，資料夾用任何文字命名都能執行。
- 大量可選診斷（`PS2_*` 環境變數，原本的 `DOD_*` 名稱仍可用）與離線分析工具。
- 重編譯器 `native = ["名稱@0x位址"]`：遊戲自己的函式與 runtime 要取代的 SDK 函式同名時，仍保留為重編譯碼；IOP `thmsgbx` 信箱；MCMAN 式記憶卡描述符；libpad 震動。
- **選用功能，預設為屏蔽（關閉）狀態，如有需要可以使用**：F2 金手指面板（項目由各遊戲自己的 `cheats.ini` 提供）與 F3 修改器面板（各移植登錄自己的開關），見[下方](#選用金手指工具預設屏蔽)；本倉庫不附任何遊戲的金手指。
- `tools/new_port.py`：從你的光碟映像建立移植資料夾，含開機 ELF、重編譯設定、`game.ini` 與接下來的確切指令。

完整清單：[IMPROVEMENTS.zh-TW.md](IMPROVEMENTS.zh-TW.md)（英文 [IMPROVEMENTS.md](IMPROVEMENTS.md)）。PS2Recomp 的逐行差異以[有順序的補丁組](patches/README.md)提供：對上游 `eb8688b` 先套用保留的基礎補丁，再依清單套用維護增量補丁；庫內源碼已包含整組修改。

## 目錄

| 路徑 | 內容 |
|---|---|
| `third_party/src/PS2Recomp/` | 改進後的 PS2Recomp：`ps2xRecomp`（重編譯器）、`ps2xAnalyzer`、`ps2xRuntime`、`ps2xIOP`、`ps2xTest` |
| `third_party/src/pcsx2gs/` | PCSX2 的 GS 包成 `pcsx2gs.dll`，C 介面在 `pcsx2-gs/include/pcsx2gs_api.h` |
| `third_party/src/kzvu/` | PCSX2 microVU 做成的 `kzvu.lib`（來自 KillzoneRecomp） |
| `fetch_deps.ps1` | 下載沒有修改、鎖定版本的相依（檢查 SHA256） |
| `build_all.ps1` | 建置全部並跑測試 |
| `tools/` | 重編譯流程腳本（函式表修正、跳轉表、同步產生碼、ISO 抽檔）；`tools/diag/` 是分析工具 |
| `examples/generic_runner/main.cpp` | 不綁任何遊戲、由 `game.ini` 設定的通用入口：複製成你的 `main.cpp` 即可 |
| `examples/dod_runner/main.cpp` | 原移植專案的遊戲入口（雙碟、高清影片、測試掛鉤），寫自己的入口時參考（不在建置內） |
| `patches/` | 與上游 `eb8688b` 的差異 |
| `docs/GAME_PORTING.md`、`docs/CHEATS.md`、`docs/TRAINER.md` | 各遊戲專用的值放在哪裡；選用的 F2 `cheats.ini` 金手指與 F3 修改器（英文，附中文摘要） |
| `docs/ECOSYSTEM_SURVEY.zh-TW.md` | 相關重編譯／原生移植專案的調查：程式碼譜系、IOP 路線、VU、GS |
| `docs/` | 原移植專案的開發記錄：每項修補的原因與驗證、建置筆記、上游測試判讀 |

原始碼之間用相對路徑互相引用，請保持 `third_party/src/<模組>` 的結構。

## 需求

- Windows 10/11 x64；CPU 需支援 SSE4.1（PCSX2 GS 需要）
- Visual Studio 2022，含「使用 C++ 的桌面開發」與「C++ CMake 工具」（實測 MSVC 19.44、CMake 3.31）
- git、Windows PowerShell 5.1
- Python 3.9 以上（建置測試的逐項結果檢查需要）
- 約 2 GB 磁碟空間（相依加完整建置）

## 建置

在 PowerShell 執行。不要用 Git Bash，它會把 `/DWIN32` 這類旗標改寫成路徑。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File fetch_deps.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File build_all.ps1
```

- `build_all.ps1` 依序建置：
  1. `kzvu.lib`
  2. `pcsx2gs.dll`
  3. PS2Recomp：`ps2_recomp.exe`、`ps2_analyzer.exe`、`ps2_runtime.lib`、`ps2EntryRunner.exe` 與各測試程式

  建完後跑測試，輸出到 `build\`。
- 主測試依 suite／名稱／狀態核對歷史例外；新失敗、空白／不完整報告會使建置失敗，需 Python 3.9 以上。
- 參數：`-BuildDir <資料夾>`；只建一部分用 `-Part kzvu|gs|ps2recomp`；不跑測試加 `-NoTests`。
- 這裡建出的 `ps2EntryRunner.exe` 用的是上游的通用 `src/main.cpp` 和空函式表，只證明執行期能連結。遊戲需要自己的產生碼和入口（見下節）。

## 新移植快速開始

```
python -I tools/new_port.py --iso D:\games\MyGame.iso --out D:\ports\mygame --title "My Game"
```

它會讀 SYSTEM.CNF、複製並檢查開機 ELF，寫出 `recomp.toml`（已建置 `ps2_analyzer` 時用它產生）、通用 runner 用的 `game.ini`，以及列出其餘指令的 `PORT_STEPS.md`。移植資料夾必須在本倉庫之外，遊戲檔案一律不提交。手動流程見下方。

## 選用金手指工具（預設屏蔽）

兩者預設都**不編入**（預設為屏蔽狀態）：不處理 F2／F3、不讀金手指檔、不寫遊戲記憶體。如有需要可以使用：

| 工具 | 開啟方式 | 文件 |
|---|---|---|
| F2 金手指：由遊戲自己的 `cheats.ini` 提供 RAM 金手指 | `-DPS2X_ENABLE_CHEAT_MENU=ON` 或 `build_all.ps1 -CheatMenu` | [docs/CHEATS.md](docs/CHEATS.md) |
| F3 修改器：各移植為自己的程式碼掛鉤登錄開關／選項的面板（`ps2_trainer.h`） | `-DPS2X_ENABLE_TRAINER=ON` 或 `build_all.ps1 -Trainer` | [docs/TRAINER.md](docs/TRAINER.md) |

修改器關閉時，移植裡的 `ps2trainer::on()`／`choice()` 會編譯成常數，等同原本的遊戲程式碼。

## 移植一款遊戲

1. 從光碟映像抽出開機 ELF 與 IRX：`python tools/iso_extract.py <iso> <輸出資料夾> <最大位元組>`
2. 用 Ghidra 匯出函式表：`analyzeHeadless … -postScript ExportPS2Functions.java <toml> <csv>`（腳本在 `ps2xRecomp/tools/ghidra`）
3. 依該遊戲 ELF 的載入區段、程式／資料範圍、間接呼叫與 switch 目標核對函式表。
   `scan_code_pointers.py`、`fix_function_map.py`、`scan_jump_tables.py`、`find_missing_function_starts.py`
   含固定參考遊戲範圍，已移至 `examples/dod_runner/tools/`；其他版本需先改用自己的範圍與驗證，見[工具邊界](tools/README.md)。
4. 辨識 SDK 函式：`ps2_analyzer <elf> analyzer.toml`，再用 `merge_stubs.py` 合併。
5. 重編譯：`ps2_recomp <設定.toml>`，把輸出放進 `ps2xRuntime/src/runner/` 與 `ps2xRuntime/include/`。`sync_generated*.py` 只複製有變的檔，避免全部重編。
6. 寫該遊戲的 `ps2xRuntime/src/runner/main.cpp`：複製 `examples/generic_runner/main.cpp`（設定來自 `game.ini`，見 [docs/GAME_PORTING.md](docs/GAME_PORTING.md)），或從上游 `src/main.cpp`、`examples/dod_runner/main.cpp` 開始。要用這個基底的新增功能：
   - `dodKzvuInstall(runtime, "microvu")`：VU1 用 PCSX2 microVU（不呼叫就用內建 VU1 直譯器）
   - `dodpcsx2::configure("pcsx2", 倍率, 0, "gs 選項")`：PCSX2 GS 的倍率與選項。有 `pcsx2gs.dll` 時預設就用 PCSX2 GS。
   - `runtime.replaceFunction(位址, …)`：替換或包裝遊戲函式
7. 傳入該遊戲專用的值（[docs/GAME_PORTING.md](docs/GAME_PORTING.md)）：PCSX2 的逐遊戲 GS 修正放在 `gs_options`；遊戲的 libgraph 是原生執行時，再給 libgraph GParam 紀錄的位址（`gparam`，`sceGsSyncV` 使用）。IPU／MPEG／字型 HLE 也要用該遊戲已驗證的資料配置，並在成功 `loadELF` 後套用，不再沿用其他遊戲的地址。剩餘渲染專用假設見 [通用基底界線](docs/REUSABLE_BASE.md)。
8. 執行檔旁要放：
   - `pcsx2gs.dll`
   - `resources\`（從 `pcsx2gs/pcsx2-gs/resources` 複製）
   - `SDL2.dll`（`third_party/src/SDL2-2.30.9/lib/x64`）
   - FFmpeg DLL（建置時自動複製）
   - 你的遊戲檔案

原移植專案用過的完整指令在 `docs/DoD_BUILD.md`。

## 測試

2026-10-27 從乾淨狀態建置的結果，詳見 [BUILD_VERIFICATION.md](BUILD_VERIFICATION.md)：

| 測試 | 結果 |
|---|---|
| `ps2x_tests` | 421／446 通過。另外 25 項是已判讀的已知差異：測試期待上游的行為，而這個基底照硬體或 PCSX2 行為。清單與原因見 BUILD_VERIFICATION.md |
| IOP 測試（5 個程式） | 全部通過 |
| GS 快取測試 | 45／45 通過 |

以上是歷史結果，不代表後續修改或其他遊戲已驗收。當前地址配置的無介面測試見 [tests/README.md](tests/README.md)，不能代替 Windows 建置與實際遊戲驗證。

主機節奏補償、DXGI 佇列控制及 UI 圖集預解碼見 [HOST_TIMING.md](docs/HOST_TIMING.md)。額外排空等待、DXGI 上限與圖集清單需由各遊戲實測配置；未提供清單不會自動預載。既有 SPU2 混響精度保留，調校後仍須按下方指南驗證冷／熱載入、音畫同步、輸入與畫面，不能以單一顯卡結果宣稱所有遊戲通過。
UI 預解碼這段差異最初是為**高清紋理移植**而做的優化；基底只提供共用載入機制，各遊戲自行提供素材、圖集清單與記憶體預算。

粒子邊緣修正預設關閉，需明確匹配遊戲、版本與 profile，不能僅因 HPO 相同而啟用。《夢之曙光》參數已隔離為選用參考配置，《鬼武者3》必須提供自己的配置與驗證。設定及驗證限制見 [PARTICLE_EDGE_PROFILES.md](docs/PARTICLE_EDGE_PROFILES.md)。

本次通用化已拆出固定遊戲標題、紋理排除、畫面比例、半行 GIF 與 CG 緩衝區判斷；遊戲專用片段與固定地址工具移至參考範例。
新增獨立輸入讀取、可選 VBlank 提交時點、明確高清素材格式及多幀 Alpha／A-B 診斷。配置、遷移與檢測步驟見
[通用化指南](docs/NEUTRAL_BASE_MIGRATION.md)，具體修改與分項驗證見[本次審查紀錄](docs/NEUTRAL_BASE_REVIEW_2026-11-05.md)。
UI 預解碼仍標註為最初針對高清紋理移植所作的優化，各遊戲需提供自己的素材與驗證。

《夢之曙光》參考入口的專屬配置、PR #8 前後功能差異、版本／ELF 確認與尚需單獨處理的歷史渲染邏輯，見
[專屬適配說明](docs/DOD_RUNNER_PR8_ADAPTATION.md)。參考增強不會自動啟用，也不會寫回共用預設。

### 完整回歸，以及如何依結果修正

請依 [完整回歸指南](docs/REGRESSION_TESTING.zh-TW.md)（[English](docs/REGRESSION_TESTING.md)）建立各遊戲的測試計畫，流程整理自新鬼武者與鬼武者 3 的交接：

- **區分證據層級。** 編譯成功、空 runner、靜態入口零缺失或房間巡檢，不等於遊戲通過。執行 `python -m unittest discover -s tests -v` 與建置內附測試，逐項檢查失敗名稱，不能只看 exit code 或數量；CTest 找到零個測試也不算通過。
- **覆蓋自然遊玩。** 冷啟動 → 選單 → 新遊戲／原生讀檔 → 移動／戰鬥 → 切場，再涵蓋各章節、Boss、過場、存檔／退出重開讀檔、結局及可適用的二周目。原版與增強模式分開，並測 HUD／字型／粒子、音訊／時鐘、IOP／SIF、影片／靜圖與常用輸出設定。
- **保留各作要求。** 新鬼需測兩碟、實際換碟及後續流程，支援合併映像時另測該模式。鬼武者 3 需檢查主 ELF／overlay 入口、房間、事件逐段與自然連播；強制跳房可能缺少沿用的 overlay 或劇情狀態。任一作的結果都不能代替其他遊戲。
- **找第一個差異。** 固定 commit、實際啟動 EXE／DLL 雜湊、profile、存檔與被消費的輸入，以相同原生／參考狀態沿 EE → VIF／VU → GIF → GS 比較。音訊核對真實來源及編碼版本；相鄰幀 A/B 不等於凍結狀態，draw 套用也不等於畫面驗收。
- **修正後驗證同一候選。** 分清硬體、ABI／遊戲配置、素材及測試工具的原因；地址與特殊渲染規則留在獨立 profile。補有意義的回歸案例，統一建置受影響元件，先重測失敗及相鄰路徑，再對驗收候選跑完整矩陣。並行負載下的欠載／卡死，先以單實例確認原因。
- **如實記錄範圍。** 使用 `PASS`／`FAIL`／`BLOCKED`／`NOT_RUN`，分別記錄路徑確實執行與人工實看／對聽。保留未解失敗和版本證據，未測不得記通過。所有遊戲的粒子都需獨立適配驗證，兩個參考遊戲也一樣；發佈包不含遊戲資料或測試存檔。


## 作者

She39（原 PC 移植與這個基底）。

## 授權

GPL-3.0（`LICENSE`）。第三方程式碼的來源與授權見 [THIRD_PARTY.md](THIRD_PARTY.md)。

致謝：

- [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp)
- [PCSX2](https://github.com/PCSX2/pcsx2)
- [KillzoneRecomp](https://github.com/drakolordx7/KillzoneRecomp)（kzvu 與數個執行期修補）
- [urban-reign-recomp](https://github.com/thiagordsfx/urban-reign-recomp)（PCSX2 GS 橋接、IPU 整合）
- AC5-Recomp（IPU）
- 原始碼註解中引用其發現的其他 PS2Recomp 衍生專案

本專案與 Sony Interactive Entertainment、Capcom、PCSX2 團隊無關。PlayStation 2 是 Sony Interactive Entertainment 的商標。
