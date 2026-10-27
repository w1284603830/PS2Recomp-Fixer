# PS2Recomp 上游測試判讀（2026-10-20，離線測試第 1 項）

## 怎麼跑

- 我們的工作樹：`build\tests`（configure 參數同 `docs\BUILD.md` 的 runtime，但 `PS2X_BUILD_RECOMP/ANALYZER/TEST=ON`、`PS2X_IOP_BUILD_TESTS=ON`、`BUILD_TESTING=ON`、`PS2X_ENABLE_FFMPEG=OFF`，FetchContent 全部指向 `third_party\src\*`，含 `NLOHMANN_JSON`）。只建置測試目標，不會編產生碼、不動 `build\runtime` 與 `dist`：
  `cmake --build build/tests --config Release --parallel --target ps2_iop_emulator_tests ps2_iop_import_tests ps2_iop_compatibility_tests ps2_iop_import_version_tests ps2_iop_spu2_tests ps2x_tests`
  執行檔：`build\tests\ps2xIOP\Release\*.exe`、`build\tests\ps2xTest\Release\ps2x_tests.exe`（約 1 分鐘）。
- 上游基線：`third_party\_downloads\PS2Recomp-eb8688b.zip` 解壓到 `work\tests\upstream-eb8688b-src\`，同參數建在 `build\tests-upstream`。
- 輸出：`work\tests\upstream-20261020\`（我們）、`work\tests\upstream-baseline-eb8688b\`（上游）。`ps2xTest` 測試檔與上游完全相同（我們沒改過測試）。
- 工作樹與上游的差異檔案清單：`work\tests\ps2recomp_vs_eb8688b.txt`（58 修改、23 新增，不含 runner 產生碼）。

## 結果

| | IOP 測試（5 個程式） | ps2x_tests |
|---|---|---|
| 上游 eb8688b | 4/4 通過（沒有 SPU2 測試） | 444/445（失敗 1：VU0 巨集表找不到 `instructions.h`，工作目錄問題） |
| 我們（build 274 原始碼） | 5/5 通過（含 SPU2、排程器修改後） | 417/445 |
| 我們＋PATH3 重設修正（build 275 原始碼） | 5/5 | **422/445**，剩 22 項差異＋同一個 VU0 環境問題 |

**真回歸：0。** 找到 1 個真問題並修正（第 108 列）：PATH3 的 IMAGE 跨傳輸續傳狀態 `g_dodPath3PendingImageQwc` 是全域變數，GIF 重設與記憶體初始化都不清，測試之間互相污染（5 項），遊戲裡則是 `sceGsResetPath` 之後下一次 PATH3 傳輸可能被當成舊 IMAGE 的像素。

## 22 項差異的分類（都已對到修改來源）

| 測試 | 分類 | 原因 |
|---|---|---|
| thread lifecycle, nested suspend… | 照實機 | `SuspendThread` 成功回傳執行緒編號（EE 核心如此；Urban Reign FMV，CRI 停止交握依賴） |
| GIF PACKED A+D writes DISPFB1/DISPLAY1、DISPFB2/DISPLAY2 | 照硬體 | A+D 0x59～0x5F 不是 GS 暫存器，硬體忽略；上游為 HLE `sceGsResetGraph` 捷徑映到特權暫存器 |
| GS CSR/IMR coherent 64/32-bit | 照硬體 | CSR 的事件位元寫 1 清除、FIELD/FIFO 為唯讀狀態（Urban §35）；上游當普通記憶體 |
| VIF FBRST RST clears VIF1 command state | 照 PCSX2 | RST 不清 CYCLE/MODE/NUM/MARK（第 102 列；PCSX2 只清內部狀態） |
| VIF STMOD、UNPACK V4-16 sign、bit15 TOPS、num zero、masked UNPACK | 照 PCSX2 | 測試沒先 STCYCL，CYCLE=0：PCSX2 `Vif_Unpack.cpp`（wl=0→256，cl=0）算出讀 0 個向量；上游把 0 當 1 |
| VIF1 DIRECT finds an image continuation…、DIRECT image tag can continue… | 照硬體 | 影像資料放在 DIRECT 宣告範圍之外，實機的 VIF 把它當下一個 VIFcode（未定義 → ER1 停止，第 104 列）；上游寬容續傳 |
| GIF DMA chain IRQ stops only when TIE is set | 照 PCSX2 | TIE 有實作；測試在同一實例連跑兩次，第一次的假資料被 GIF 狀態機當成未完成 IMAGE（PCSX2 Gif_Unit 也保留跨傳輸狀態） |
| GIF arbiter prioritizes PATH1 then PATH2 then PATH3 | 刻意 | 上游 #299：按優先權排序會讓貼圖上傳晚於使用它的繪製，改按提交順序（`DOD_GIF_PRIORITY_SORT=1` 回舊行為） |
| IPU init executes its guest helper…、IPU init skips missing optional helper… | 照 PCSX2 | IPU 為 PCSX2 移植，寫 IPU_CTRL.RST 觸發重設、讀回不保留；上游當變數 |
| SifCallRpc falls back to stack ABI…、register bind call updates…（[Error]） | 照實機／DoD 必要 | 阻塞式 `sceSifCallRpc` 讓呼叫者睡一個 alarm tick（實機睡到 IOP 回覆；DoD 讀第一批音效時的死結修正），透過排程器切換，測試沒有排程器迴圈接住 |
| SifInitRpc does not reset the running IOP | 效能 | IOP 以 4096 EE 週期為單位推進（上游 #328）加獨立 IOP 執行緒；測試只推 80 週期 |
| unresolved JALR marks internal labels as indirect fallback… | 效能 | 間接呼叫只標返回位址為進入點（第 41 列；原本整個函式每條指令都成進入點，約 4000 個函式無法最佳化）。測試裡 0x320C 正是返回位址，仍是進入點，只是分類不同 |
| sceGsSyncV … deterministic field parity | DoD 專用 | `sceGsSyncV` 讀 DoD 的 libgraph 記錄 0x3FD4A0（Codex，codex99）；測試環境沒有這份記錄 |
| sceGsSyncVCallback … callback stack | 照上游 #298 | 回呼堆疊改用核心區 0x80000–0x100000（RAM 頂端會壓到遊戲主堆疊）；測試要求 ≥ 0x01F00000 |
| VIF1 packet builders keep chain qwc live before terminate（10/20 下午新增，build 280 起 421/445） | 照原版 SDK | 上游讓 libvifpk 替換函式每次移動指標就改寫開著的 DMA 標籤 QWC；原版 libvifpk（遊戲 0x1049A8）只在關閉時**加**一次。DoD 只替換 3 個函式，兩者混用使 CNT 加兩次（VIF1 鏈結重疊、0x8CD 崩潰的根因，RUNTIME_PATCHES 第 113 列）。測試沒關閉封包就要求 QWC 已填好，原版也不會 |

## 下一步建議

- 把「照硬體／照 PCSX2」的測試改成期望正確行為（加 `[DoD]` 註解與列號），VIF UNPACK 那 5 項補 STCYCL 讓它們真的測到符號擴展、TOPS、mask、num=0；GIF TIE 測試在兩次之間寫 GIF_CTRL.RST。這樣整套 445 項就成為我們的回歸網，之後改 runtime 時一跑就知道有沒有打壞。
- DoD 專用與效能類的 4 項標成已知差異（或改測試只驗證功能等價的部分）。
- `ps2xIOP/tests/iop_sif_dma_intr_tests.cpp`（Codex 寫的 sifman 32 測試）沒登記進 CMake，可補上。
