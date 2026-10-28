# 新鬼武者 PC 移植｜建置流程（2026-10-17）

## 目前使用的重編譯設定（2026-10-20 更新）

目前使用 `work/recomp/dod_upstream20.toml`（函式表 `dod_functions_fixed8.csv`，含 switch 表目標），輸出到 `work/recomp/out20-jumptables/`。產生碼每條指令都附原始反組譯註解，要找某個地址就 grep 這個資料夾的 `// 0x地址`。

2026-10-20 整理：`out2`～`out19`、`backup*`、`sync-backup-*` 這些中間輸出已移到資源回收筒（清單在 `work/cleanup_20261020.log`），只保留 `out`（原版函式，供 ABI 核對）、`output`、`out20-jumptables`。其他 `*.toml` 仍在，要重現舊輸出就用對應的 toml 重跑 `ps2_recomp`。

2026-10-21，build 293／295 起：劇情 CG 的固定選行與換幀 hook 在兩個產生碼檔（`FUN_002ad970_0x2ad970.cpp`、`FUN_002ad690_0x2ad690.cpp`），共用輔助檔是 `src/runner/dod_cg_progressive.h`；295 另依賴 `src/lib/gs/dod_cg_cadence.h` 與 GS bridge 的有序 metadata。詳見 RUNTIME_PATCHES 第 125～126 列。**每次重新產生並同步 runner 後、建置前**，保留輔助檔並執行：

```powershell
python -I tools/scripts/apply_cg_progressive.py third_party/src/PS2Recomp/ps2xRuntime/src/runner
python -I tools/scripts/apply_widescreen.py
python -I tools/scripts/apply_cg_subtitles.py
python -I tools/scripts/apply_dod_cheats.py third_party/src/PS2Recomp/ps2xRuntime/src/runner
```

2026-10-28 起：F2 金手指選單（RUNTIME_PATCHES 第 150 列）在 22 個產生碼檔的 18 條指令加了開關（讀 `g_dodCheats[n]`，定義在 `src/lib/dod_cheats.cpp`）；重新產生 runner 後必須跑上面最後一行，否則選單裡的指令型金手指會沒有作用（資料型的「道具數量」仍有效）。腳本只接受它認得的原始指令文字，產生碼改了會直接報錯。

腳本可重複執行；不認得指令文字就拒絕修補，並可從 293 的 hook 升級。不要只還原產生碼而漏套這一步，否則劇情 CG 的選行／換幀修正會遺失。`DOD_CG_FIELD=keep` 回復舊選行，`DOD_CG_CADENCE=keep` 只關閉 295 的換幀；平常都不設定。295 實跑與限制見 `work/cg-cadence-20261021/RESULTS.md`。

2026-10-23 寬螢幕：`apply_widescreen.py` 重套 8 個投影／裁切常數，涵蓋 5 個重複 entry 檔；須保留 `src/lib/gs/dod_widescreen.h`。只改常數、不直接複製外部 pnach 的指令或暫存器；本版 0x167f58 使用 v1，上游 pnach 的 v0 不適用。`aspect=16:9` 才啟用，缺省／無效值回 4:3。GS 的 `DoDWidescreen.h`、`GSState.cpp`、`GS.cpp` 與 bridge 必須配套建置；EXE／GS DLL 未成套部署會造成比例錯誤。詳見 `work/widescreen-20261023/交接_真正16比9_20261023.txt`。

以下是 10-18 的歷史紀錄：當時使用 `dod_native_gs_init.toml`（輸出 `out5-native-gs-init/`）。以 `dod_merged2.toml` 為基礎，移除 LoadImage 兩個及 GS 環境初始化九個不相容的 HLE 綁定，讓它們執行原版重編譯程式碼。完整地址與依賴見 `work/codex-boot-20261018/GS_LIBGRAPH_NATIVE_ABI_REVIEW.md`。更早的 `dod_native_libgraph.toml`／`out4-native-libgraph/` 只修 LoadImage。

`tools/scripts/sync_generated_preserve.py` 可比較、備份並同步內容有變的產生碼；若檔案集合有增減會拒絕執行，不會刪除舊檔。首次修正的比對報告在 `work/codex-boot-20261018/sync-native-libgraph.json`。報告中的 `ps2_recompiled_stubs.h` 隨後還原：多出的兩個函式宣告無害，且與 `ps2_recompiled_functions.h` 相同；有效修改是兩個函式實作與註冊表。共用標頭變更可能觸發全部 58,674 個產生碼檔案重新建置。

目前 MPEG 電影路徑需要 `PS2X_ENABLE_FFMPEG=ON`。codex111 已下載並連結上游鎖定的 FFmpeg 7.1 MSVC shared 套件；來源與 SHA256 見 `work/codex-boot-20261018/ffmpeg-download-sha256.json`。部署時除了 runner EXE，也須將 `build/runtime/ps2xRuntime/Release/*.dll` 複製到 `dist/`。CMake 將 FFmpeg 設為 runtime 私有依賴，避免它的 include 變動觸發全部產生碼重編。成功編譯與 fixture 解碼不代表遊戲 CG 已驗收，實跑現況仍以交接為準。
本機 CMake 完整路徑：`C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`。

後續同步可加 `--preserve-redundant-stub-header`：只在移除的 stub 宣告仍完整存在於 functions header 時保留原標頭，避免無意義的全量 PCH 重建。第二階段同步報告為 `sync-native-gs-init.json`，只更新九個函式和註冊表。

診斷執行使用 `tools/scripts/run_probe.ps1 -Name <新的名稱> -Seconds 60 -CaptureGif -CaptureFrame -TracePad`，證據獨立存放於 `dist/logs/<名稱>/`，不覆寫既有紀錄。`-Seconds 0 -Interactive` 會保留遊戲視窗供實際操作；腳本不自動按鍵。`DOD_GIF_CAPTURE_DIR` 最多保存 2048 個 PATH2 DIRECT 原始封包。`DOD_FRAME_CAPTURE_DIR` 每五秒匯出應用程式本身的最終繪圖緩衝區，最多720張；沒有這個變數時不擷取。PNG 編碼在單一背景 worker，GL readback 仍在視窗執行緒。`DOD_PAD_TRACE` 記錄最多1024次按鈕狀態變化，並每秒輸出原版主迴圈／callback／VBlank 計數（最多600行），不能只用 host FPS 判定遊戲更新速度。

效能基準加 `-Perf`，且不要加 frame/GIF capture。腳本預設已改為 `--trace 0 --gsdump 0`；需要重型追蹤時必須明確用 `-ExtraArgs @('--trace','1','--gsdump','1')`。`DOD_PERF` 每五秒直接量測 host frame、GS/VRAM等待、raster、transfer、scanout 與呈現時間；新的 tick 統計使用 GS 原有 atomic VBlank counter，分辨重複呈現與一次跨過多個 tick。Debug-history 的 present 數目不可靠，不能作 FPS 驗收依據。

以下保留原始建置步驟；步驟 7、8 的輸入輸出應採用上述新設定。主選單驗證狀態以最新交接文件為準。

路線：PS2Recomp 靜態重編譯。Ghidra 匯出函式表 → `ps2_recomp` 把 R5900 ELF 轉成 C++ → 連結 `ps2xRuntime`（含 `ps2xIOP` 直接跑原版 IRX）→ `dod.exe`。
所有輸入與版本鎖在 [`sources.lock.json`](../sources.lock.json)。

## 目錄

| 路徑 | 內容 |
|---|---|
| `discA_cn.iso`、`discB_cn.iso` | 漢化 cn104 光碟映像（使用者自備，不可散布） |
| `third_party/_downloads/` | 7 個鎖定版本的原始碼 zip |
| `third_party/src/` | 解壓後的原始碼；`PS2Recomp/` 是工作樹（會放入產生的程式碼與我們的 `src/runner/main.cpp`） |
| `tools/scripts/` | 本專案腳本（解壓、ISO 抽檔、函式表修正、同步產生碼） |
| `game/discA/` | 從 Disc A 抽出的啟動檔：`SYSTEM.CNF`、`SLPM_662.75`、`IMAGE/MODULES/*` |
| `work/ghidra/` | Ghidra 專案 `DoD` |
| `work/recomp/` | 函式表、TOML、產生的 C++（`out/`） |
| `build/recomp/`、`build/runtime/` | CMake 建置目錄 |
| `launcher/` | 啟動器原始碼與建置腳本 |

## 步驟

1. 解壓依賴：`python -I tools/scripts/extract_archives.py third_party/_downloads third_party/src PS2Recomp=… raylib=… …`
2. 建 ps2_recomp（只開 Recomp）：
   `cmake -S third_party/src/PS2Recomp -B build/recomp -G "Visual Studio 17 2022" -A x64 -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_SOURCE_DIR_<ELFIO|TOML11|FMT|LIBDWARF|RABBITIZER>=third_party/src/<…>`
3. 抽啟動檔：`python -I tools/scripts/iso_extract.py discA_cn.iso game/discA 16777216`
4. Ghidra 分析並匯出（約 2 分鐘）：`analyzeHeadless work/ghidra DoD -import game/discA/SLPM_662.75 -overwrite -scriptPath third_party/src/PS2Recomp/ps2xRecomp/tools/ghidra -postScript ExportPS2Functions.java work/recomp/dod.toml work/recomp/dod_functions.csv`
   - 自動選用 `r5900:LE:32:default`；10,246 函式＋48,354 可執行標籤，58 個 syscall 包裝被辨識為 stub。
5. 補函式起點並修正函式表：
   `python -I tools/scripts/scan_code_pointers.py game/discA/SLPM_662.75 work/recomp/dod_functions.csv work/recomp/dod_functions_scanned.csv`
   `python -I tools/scripts/fix_function_map.py work/recomp/dod_functions_scanned.csv work/recomp/dod_functions_fixed2.csv`
   switch 表的 case 目標若沒有記錄，`jr` 會落到 `[guest-branch:missing-target]`：`python -I tools/scripts/scan_jump_tables.py game/discA/SLPM_662.75 <in.csv> <out.csv>` 把它們加為 entry 標籤（目前用 `dod_functions_fixed8.csv`、`dod_upstream20.toml`）。
6. SDK 函式辨識：建 `ps2_analyzer`（另需 nlohmann/json v3.11.3），`ps2_analyzer game/discA/SLPM_662.75 work/recomp/analyzer.toml`；
   `python -I tools/scripts/merge_stubs.py work/recomp/dod_fixed.toml work/recomp/analyzer.toml work/recomp/dod_merged.toml`，
   再把 `ghidra_output`／`output` 改成 `dod_functions_fixed2.csv`／`work/recomp/out3`（`dod_merged2.toml`）。
7. 重編譯：`ps2_recomp.exe work/recomp/dod_merged2.toml`（約 95 秒，58,674 檔、418 MB，0 個未處理指令）。
8. 同步產生碼（只複製內容有變的檔）：`python -I tools/scripts/sync_generated.py work/recomp/out3`。
   只新增函式時用 `sync_generated_added.py <out> third_party/src/PS2Recomp/ps2xRuntime <backup> <report.json>`（新函式放進 `zz_dod_added_functions_*.cpp`，不必重跑 configure），接著 `sync_restore_include_only.py <runner> <backup>` 把只多一行 include 的檔案還原，避免整個 runner 重編。
   檔案數有增減時要重跑 CMake configure（unity 分批與 VS 專案清單在 configure 時產生）。
9. 建 runtime 與執行檔（**在 PowerShell 執行**，Git Bash 會把 `/DWIN32` 之類旗標改寫成路徑）：
   `cmake -S third_party/src/PS2Recomp -B build/runtime -G "Visual Studio 17 2022" -A x64 -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=ON -DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_SCCACHE=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_SOURCE_DIR_RAYLIB=third_party/src/raylib "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /MP /bigobj /utf-8" "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /MP /utf-8"`
   `cmake --build build/runtime --config Release --target ps2EntryRunner --parallel`
10. 啟動器：`launcher/Build.ps1`（Windows 內建 .NET 4 `csc.exe`，不需安裝）。存檔轉換工具：`launcher/converter/Build.ps1 -OutputDirectory <資料夾>`（同樣用內建 csc，產生 `DodSaveConverter.exe`；測試見 `work/saveconv-20261028/run_tests.py`）。

### 2026-10-26：一行建置＋測試副本（只改 runtime／GS 時）

`tools/scripts/build_runtime.ps1`（參考 godhand-recomp 的 `build.py`；不含步驟 1～8 的重新產生碼）：在 PowerShell 執行
`powershell -NoProfile -ExecutionPolicy Bypass -File tools\scripts\build_runtime.ps1 [-Target runtime|gs|both] [-TestDist <資料夾>]`。
- 用既有的 `build/runtime`（`ps2EntryRunner`）／`build/pcsx2gs`（`pcsx2gs`）編譯，輸出記錄在 `work/diag/build-<標記>-<runtime|gs>.log`；失敗時印出前 20 個錯誤。
- 連結器 map 存成 `work/diag/dod<標記>.map`、`pcsx2gs_<標記>.map`（標記預設「日期＋第一個沒用過的字母」，不覆蓋舊檔）；崩潰堆疊和 `prof_symbolize.py` 要用同一版的 map。
- 印出新檔與 dist 的 SHA256（前 16 碼）。
- `-TestDist`：資料夾不存在就從 dist 複製一份（真正的複本，不是硬連結；不複製 logs、audio_trace、高清紋理——dod.ini 用絕對路徑指著 dist 的紋理），已存在就只換 `dod.exe`／`pcsx2gs.dll`。**永遠不寫 dist**，部署另外照使用者指示做。
- 測試腳本 `run_probe.ps1`、`room_sweep.ps1`、`movie_sweep.ps1`、`parallel_sweep.ps1` 看環境變數 `DOD_PROBE_DIST`：設成測試副本的資料夾，就用那裡的 `dod.exe`、shader cache，記錄寫到 `<測試副本>\logs`；沒設時照舊用 dist。例：`$env:DOD_PROBE_DIST='F:\OnimushaDoD\work\x\testdist'` 後再啟動 `parallel_sweep.ps1 -Name X -SkipMovies`。
- 兩份全房間報告的差異：`python -I tools/scripts/diag/compare_room_reports.py <舊-rooms.md> <新-rooms.md> [<logs 資料夾> <名稱>-r]`（後兩個參數另外統計 `[ee]` 呼叫堆疊、`[vu1]` 長程式、`[crash]`、`invalid VIFcode`）。

## 踩過的坑

- **函式表會讓輸出暴增**：ELF 只有一個 RWX 段，Ghidra 在資料區（`0x408DA0`–`0x686100`）產生大段「可執行標籤」；另有函式本體不連續（如 `FUN_002a8dc0` 實際 32 bytes，但最大位址到 `0x3388B8`）。未修正時產出 753 MB、單檔 56 MB；修正後 420 MB。真正的程式碼在 `0x100000`–`0x3FEC88`，另有 `0x6804C0`–`0x6807E0` 一小段函式保留。
- **CD 讀取**：EE 端 `sceCdSearchFile` 直接索引光碟映像的 ISO9660 目錄（真實 LBN）；有映像時不再退回 `cdRoot`（`game/discA` 只有 Disc 1 的開機檔，退回會讓 Disc 2 被當成 Disc 1），找不到時 `sceCdGetError` = 0。IOP 端讀同一個映像。換盤時兩邊一起切換（`RUNTIME_PATCHES.md` 第 80、81 列）。
- **Esc**：鍵盤的 ○ 對應 Esc，而 Esc 是 raylib 預設的關閉鍵；`main.cpp` 呼叫 `SetExitKey(KEY_NULL)`。

## 執行

2026-10-23 啟動器 v0.1.8：正式／測試的Launch使用`CreateNoWindow=true`且`UseShellExecute=false`，從啟動器開遊戲時不另建主控台，不隱藏遊戲視窗；命令列直接啟動dod.exe仍可診斷。回歸測試的GUI驅動器`NoConsoleLaunchTests.cs`須用`/target:winexe`，探針`ConsoleLaunchProbe.cs`用console target，經私人desktop執行。詳`work/launcher-no-console-20261023/交接_啟動器隱藏主控台_20261023.txt`。

2026-10-23 啟動器 v0.1.7：`BackdropPanel` 在原生捲動後刷新子項；透明容器與下拉框按絕對位置取同一張虛化背景快取，首次／尺寸改變才重建。`launcher/tests/BackdropTests.cs` 檢查原生更新區域、局部畫面對齊及快取。UI測試請用 `launcher/tests/IsolatedUiRunner.cs` 編成的EXE包住測試程式，透過私人Windows desktop執行，不切換或操作使用者桌面。指令、回退、證據及實際驗收限制見 `work/launcher-paint-20261023/交接_啟動器重繪與虛化_20261023.txt`。

2026-10-23 啟動器 v0.1.6：`launcher/Build.ps1` 同時建置正式／測試版；需保留 `launcher/Assets/background.png`、`dod.ico`、`dod-logo.png` 與 `launcher/app.manifest`。背景與ICO嵌入啟動器，不需要旁掛圖片。Windows runner 的 CMake 會從 `launcher/Assets` 產生 `dod_branding.rc`，嵌入ICO與PNG；`main.cpp` 在初始化後將PNG設為遊戲視窗圖示。重產 runner 時保留 branding resource、標題及 `SetWindowIcon` 區塊。此輪只增RC與重編 runner，沒有重建58,788份產生碼。介面採 system-DPI awareness、視窗大小限制於工作區、可捲動頁面；跨不同DPI螢幕時由Windows縮放。按鍵有效變更立即原子寫入 `key_*`，其他設定仍按「保存设置」或「启动游戏」才保存。

排版與即時儲存回歸：`launcher/tests/LayoutTests.cs`、`InstantKeysTests.cs` 可用 .NET Framework csc 另編EXE（參考 Windows.Forms/Drawing），參數為已建置的啟動器EXE與獨立測試目錄。排版測試需要候選EXE旁有 `pcsx2gs.dll`。100%–200%測試為字體／控制項縮放模擬，不取代其他實機顯示設定驗收。完整指令、結果及回退見 `work/launcher-redesign-20261023/交接_啟動器重製_20261023.txt`。

`dod.exe` 讀同目錄的 `dod.ini`（啟動器寫入）：`disc_mode`（`dual` 雙盤／`merged` 合盤；未設時只有 `disc_merged` 存在才用合盤）、`disc_a`、`disc_b`（雙盤：從 Disc A 啟動，遊戲要求換盤時自動換入另一張）、`disc_merged`（合盤：雙層 DVD9 單一映像，例如 `Onimusha_JP_DVD9.iso`，不換盤）、`window`（如 `1280x960`）、`fullscreen`（0/1）、`gs_scale`（渲染精度；未設或 `auto` 時依顯示分辨率自動選，寫數字 1–8 可覆蓋）、`key_*`（鍵盤鍵位，raylib 鍵碼「主,副」）、`elf`、`savedata`；`disc = B` 可強制從 Disc B 啟動、`iso = <映像>` 只用一張映像。命令列 `--key value` 可覆蓋。測試用啟動器 `DodLauncherTest.exe`（`launcher/Build.ps1` 同時產生）多一個「声音追踪」：遊戲保留最近 60 秒聲音與事件，按 F9 存到 `audio_trace\<日期-時間>\`（見 RUNTIME_PATCHES 第 98 列）。

聲音：IOP 上的 SPU2 模擬（`RUNTIME_PATCHES.md` 第 85 列）。`DOD_SPU2=0` 關閉；`DOD_SPU2_WAV=<檔>` 錄下輸出，`python -I tools/scripts/diag/wav_analyze.py <wav> <png>` 印出各段音量／削波／跳點並畫頻譜圖；`DOD_SPU2_STATS=1` 每 5 秒印音量與佇列。

2026-10-23 啟動器 v0.1.5：`volume = 0..100` 為遊戲總音量，缺省／無效文字為100；有效整數限制在0至100，0靜音，不放大超過原音量。啟動器保存後下次啟動生效，命令列 `--volume 50` 可覆蓋。runner 在音訊裝置初始化後呼叫 raylib `SetMasterVolume`，控制音樂、音效與CG的最終輸出，不改模擬SPU2寄存器／播放時鐘或Windows系統總音量。`src/lib/dod_audio_settings.h` 是runner私有標頭，重產runner時需保留main中的套用步驟；不用改共用產生碼標頭。聲音診斷的SPU2 WAV／trace在裝置音量之前，不能用該錄音振幅宣稱滑條無效。測試與回退見 `work/volume-20261023/`。


## 2026-10-21：build 296 手把依賴

`dod_gamepad.cpp` 使用相鄰 `third_party/src/SDL2-2.30.9/include` 的官方 SDL2 標頭，實際動態載入 EXE 同目錄的 `SDL2.dll`（現有2.30.9不換）。VC SDK 原包在 `third_party/_downloads/SDL2-devel-2.30.9-VC.zip`，正式輸出附 `dist/SDL2-LICENSE.txt`。新私有標頭不進58,788個產生碼共用include。重新產生CMake專案可能需數分鐘。

重跑本輪控制器測試：在 `work/gamepad-20261021` 執行 `build-tests.cmd`（需本目錄SDL2.dll、且無實體手把接上，避免測試震動送到玩家裝置）及 `build-fallback.cmd`。詳細工具路徑、重建、回歸和驗收界線見同目錄 `RESULTS.md`。先前CG產生碼重套腳本要求保持。

## 2026-10-26：PC高清CG直接播放

重生成runner后，除既有`apply_cg_progressive.py`外，必须运行`tools/scripts/apply_hd_cg.py`，恢复`entry_0010b890_0x10b890.cpp`的Delete guard。`dod_hd_cg.h`为私有MPEG解码适配，不加入全部生成码共用include。

默认从EXE同层`movies_hd`读取71支HEVC、`movies.tsv`和`movie<ID>.pts`。它们是PC外部资源，不能写入PS2槽位；缺片在播放前回退。`DOD_CG_HD_DIR=0`可关闭。用`build_runtime.ps1 -Target both -Tag <新tag> -TestDist <隔离目录>`建置候选，正式部署另行核准/依据用户当前授权。原防抖文件不改。最终构建、报告、备份和使用限制见`Docs/交接_PC_CG直接替换_20261026.txt`。
