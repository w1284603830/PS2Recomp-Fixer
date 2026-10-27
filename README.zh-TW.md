# PS2Recomp-Fixer：照 PCSX2 行為強化的 PS2Recomp 基底

[English](README.md)

這是 [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp)（commit `eb8688b`，PS2 靜態重編譯器）的分支。製作商業 PS2 遊戲《新鬼武者 DAWN OF DREAMS》完整 PC 移植時，順帶把它強化了：

- 新鬼本身有兩張光碟，含 3D、CRI Sofdec／ADX 影片，以及跑在 IOP 上的音效驅動；所有房間與影片都用自動巡檢跑過。
- 執行期、重編譯器與 IOP 共做了約 145 項修正，大多照 PCSX2 的行為修。
- 補上了上游執行期缺少的硬體模擬。

**這個倉庫不含任何遊戲。** 它是可重複使用的基底：重編譯器、執行期、IOP 模擬、PCSX2 的 GS 與 VU 模組、工具和測試。要移植遊戲，請使用你自己合法取得的遊戲。

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
- 大量可選診斷（`DOD_*` 環境變數）與離線分析工具。

完整清單：[IMPROVEMENTS.zh-TW.md](IMPROVEMENTS.zh-TW.md)（英文 [IMPROVEMENTS.md](IMPROVEMENTS.md)）。逐行差異在 `patches/PS2Recomp_vs_upstream_eb8688b.diff`，可在上游 `eb8688b` 用 `git apply -p1` 套用。

## 目錄

| 路徑 | 內容 |
|---|---|
| `third_party/src/PS2Recomp/` | 改進後的 PS2Recomp：`ps2xRecomp`（重編譯器）、`ps2xAnalyzer`、`ps2xRuntime`、`ps2xIOP`、`ps2xTest` |
| `third_party/src/pcsx2gs/` | PCSX2 的 GS 包成 `pcsx2gs.dll`，C 介面在 `pcsx2-gs/include/pcsx2gs_api.h` |
| `third_party/src/kzvu/` | PCSX2 microVU 做成的 `kzvu.lib`（來自 KillzoneRecomp） |
| `fetch_deps.ps1` | 下載沒有修改、鎖定版本的相依（檢查 SHA256） |
| `build_all.ps1` | 建置全部並跑測試 |
| `tools/` | 重編譯流程腳本（函式表修正、跳轉表、同步產生碼、ISO 抽檔）；`tools/diag/` 是分析工具 |
| `examples/dod_runner/main.cpp` | 原移植專案的遊戲入口，寫自己的入口時參考（不在建置內） |
| `patches/` | 與上游 `eb8688b` 的差異 |
| `docs/` | 原移植專案的開發記錄：每項修補的原因與驗證、建置筆記、上游測試判讀 |

原始碼之間用相對路徑互相引用，請保持 `third_party/src/<模組>` 的結構。

## 需求

- Windows 10/11 x64；CPU 需支援 SSE4.1（PCSX2 GS 需要）
- Visual Studio 2022，含「使用 C++ 的桌面開發」與「C++ CMake 工具」（實測 MSVC 19.44、CMake 3.31）
- git、Windows PowerShell 5.1
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
- 參數：`-BuildDir <資料夾>`；只建一部分用 `-Part kzvu|gs|ps2recomp`；不跑測試加 `-NoTests`。
- 這裡建出的 `ps2EntryRunner.exe` 用的是上游的通用 `src/main.cpp` 和空函式表，只證明執行期能連結。遊戲需要自己的產生碼和入口（見下節）。

## 移植一款遊戲

1. 從光碟映像抽出開機 ELF 與 IRX：`python tools/iso_extract.py <iso> <輸出資料夾> <最大位元組>`
2. 用 Ghidra 匯出函式表：`analyzeHeadless … -postScript ExportPS2Functions.java <toml> <csv>`（腳本在 `ps2xRecomp/tools/ghidra`）
3. 修正函式表，依序執行：
   1. `scan_code_pointers.py`：補只經函式指標呼叫的函式；
   2. `fix_function_map.py`：去掉資料區的假標籤、處理不連續函式；
   3. `scan_jump_tables.py`：補 switch 跳轉目標；
   4. 執行時出現 `[guest-branch:missing-target]`，再用 `find_missing_function_starts.py` 補。
4. 辨識 SDK 函式：`ps2_analyzer <elf> analyzer.toml`，再用 `merge_stubs.py` 合併。
5. 重編譯：`ps2_recomp <設定.toml>`，把輸出放進 `ps2xRuntime/src/runner/` 與 `ps2xRuntime/include/`。`sync_generated*.py` 只複製有變的檔，避免全部重編。
6. 寫該遊戲的 `ps2xRuntime/src/runner/main.cpp`，從上游 `src/main.cpp` 或 `examples/dod_runner/main.cpp` 開始。要用這個基底的新增功能：
   - `dodKzvuInstall(runtime, "microvu")`：VU1 用 PCSX2 microVU（不呼叫就用內建 VU1 直譯器）
   - `dodpcsx2::configure("pcsx2", 倍率, 0, "gs 選項")`：PCSX2 GS 的倍率與選項。有 `pcsx2gs.dll` 時預設就用 PCSX2 GS。
   - `runtime.replaceFunction(位址, …)`：替換或包裝遊戲函式
7. 檢查 [IMPROVEMENTS.zh-TW.md](IMPROVEMENTS.zh-TW.md) 最後一節列出的原遊戲專用處。**`Kernel/Stubs/GS.cpp` 的 `sceGsSyncV` 讀原遊戲 libgraph 的固定位址，一定要改。**
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

原移植專案是用那款遊戲驗證的（房間與影片巡檢、實機遊玩）。換一款遊戲要另外測。

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
