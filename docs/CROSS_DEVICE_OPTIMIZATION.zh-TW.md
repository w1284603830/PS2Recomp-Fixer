# PS2 重編譯移植的跨設備效能與相容性

[English](CROSS_DEVICE_OPTIMIZATION.md) · [移植與配置](GAME_PORTING.md) · [回歸測試](REGRESSION_TESTING.zh-TW.md) ·
[主機時序](HOST_TIMING.md)

本文件處理以本基底製作的移植常見回報：「部分電腦 CG 卡頓」「部分電腦無法播放 CG」「遊戲卡頓」「志強（Xeon）
電腦進不了遊戲」。以下每一項都是與遊戲無關的共用機制、主機服務或建置設定，在一個移植（奧特曼格鬥進化重生，
SLPS-25529）、一台電腦（i5-12400F、RTX 3080、Windows 10）上實作並量測。數字只屬於那個移植；你的遊戲要自己量
才能下結論。原始碼變更標記為 `[Fixer]`。

## 0. 先量測：看什麼、怎麼看

| 問題 | 方法（工具在 `tools/perf/`） |
| --- | --- |
| 哪個執行緒在拖慢遊戲？ | `thread_cpu_sample.py <pid> <秒> out.json`：每秒、每個具名執行緒（`GameThread` = EE、`dod GS`、`dod IOP`）的 CPU %，分場景看。整個行程的 CPU 會掩蓋真正的瓶頸。 |
| 那個執行緒的時間花在哪？ | `thread_profile.py <pid> GameThread 40 runner.map out.json`（每秒 1000 次取樣 RIP，用 EXE 的 `/MAP` 解析）；DLL 加 `--map pcsx2gs.dll=pcsx2gs.map`。正常約一半樣本在 `ntdll`（等下一幀）。 |
| 玩家看起來順不順？ | 看遊戲幀/秒（畫面翻頁，DISPFB 變化），不要只看輸出場數/秒。CPU 被搶時，場數可能維持 60，遊戲卻只剩一半速度。 |
| 在慢電腦上會怎樣？ | `cpu_cap.py <pid> --cpus 2,4 --step T:C ...`：路線走到要測的內容後，才把行程限制為 C 個邏輯核心的用量。新舊版同時、在不同核心上跑；雜訊約數 fps。 |
| 別的程式在忙時會怎樣？ | 每個邏輯核心放一個忙碌行程，同時跑遊戲，看遊戲幀/秒。 |
| 改動有沒有改變遊戲行為？ | 跨版本決定性：關閉追趕、固定亂數種子、按邏輯幀輸入，在固定邏輯幀傾印 EE 記憶體。先量「同版本跑兩次」的雜訊（主機時序計數器），再比較新舊版。 |
| 解碼器改動是否逐位元相同？ | 在固定里程碑雜湊送給遊戲的資料（`DOD_PERF=1` 時 IPU 每 16 MiB 印一次 FNV-1a），新舊路徑對照。 |
| 舊 CPU 能不能跑？ | `isa_scan.py exe out.json`（靜態），再用 Intel SDE `-<chip> -chip_check <XED 晶片>` 跑**真的會走到該路徑**的程式，並做陽性對照（舊程式必須報錯）。只加 `-snb`/`-wsm` 只會假裝 CPUID。 |
| 堆疊框架有多大？ | `frame_sizes.py exe out.json`（每個函式的 `sub rsp` 與 `__chkstk` 大小）。 |

## 1. 舊 CPU（志強）啟動失敗或進 3D 當掉

- VU 重編譯器前端（kzvu/microVU）不要用 `/arch:AVX2` 編譯：MSVC 會在任何地方產生 VEX 指令，包括在 `main` 之前執行的
  靜態初始化（沒有 AVX 的志強一啟動就當）。PCSX2 的基準是 SSE4.1，JIT 會在執行期選 SSE4/AVX/AVX2。
- 也要搜尋手寫的 `_mm256_*` / `__m256` 內建函式：編譯旗標管不到它們。kzvu 的 VU1 程式快取（`kzvuVu1CodeChanged`）
  在任何微程式上傳後的第一次 VU1 執行（也就是第一個 3D 畫面）做 256 位元測試，現已改為 SSE4.1。只跑開機的 SDE
  測試走不到這裡：要在 `sde -wsm -chip_check WESTMERE` 下跑 kzvu 自帶的 `kzvu_test` / `kzvu0_test`
  （`-DKZVU_BUILD_TEST=ON`）。
- `ps2host::cpuMeetsBaseline()` 記錄 `[cpu] 型號 | 廠商 | 核心數 | 指令集`；缺 SSE4.1 時執行器跳出訊息框，而不是
  非法指令崩潰。
- SDE 的假象：`-wsm` 下任何 DXGI `Present` 都會失敗（「OSXAVE BIT IS NOT SET」），連 60 行的 D3D11 小程式也一樣。
  這個晶片要用 CPU 光柵化測（`PS2_GS_TEST_OPEN_FAIL=1`）。

## 2. GPU、GS 主機服務與呈現

- **GS 開不起來不能變成黑屏。** 在客體啟動前於工作執行緒開啟 GS；失敗就停掉工作執行緒、銷毀子視窗，改由 CPU 光柵化
  呈現（用 `PS2_GS_TEST_OPEN_FAIL=1` 測）。`Present` 失敗時記錄一次 `GetDeviceRemovedReason`。
- **回讀：** PCSX2 的 `GSTextureCache::PrepareDownloadTexture` 檢查的是 `unique_ptr` 的位址而不是建立出來的材質
  （上游至今仍是如此）；改成先建立到暫存物件，失敗時保留舊材質。
- **用哪張顯示卡：** GS 在 Windows 10 1803 以後用 `IDXGIFactory6::EnumAdapterByGpuPreference` 挑高效能顯示卡，除非
  移植指定 `adapter=`。執行檔另外匯出 `NvOptimusEnablement` 與 `AmdPowerXpressRequestHighPerformance`，照顧舊系統和
  OpenGL 退回路徑。
- **依顯示卡等級決定內部解析度**（`ps2host::gpuReport()` / `autoScaleCap()`）：`gs_scale = auto` 仍按顯示尺寸選
  （能蓋住畫面的最小 448 線倍數），再加上限：獨顯 4 倍、內顯 2 倍（D3D12 回報統一記憶體即為內顯；沒有 D3D12 時以
  專用顯存小於 1 GiB 判斷；APU 可能把 2 GB 預留記憶體報成「專用」）、軟體渲染 1 倍，並記錄一行 `[gpu]`。玩家
  明確選的倍率永遠不受限。新發行包的設定請預設 `gs_scale = auto`，首次執行才會自動調整。測試時可用
  `PS2_GPU_TIER=integrated|discrete|software` 模擬。
- **GS 執行緒的 CPU：** 量測的戰鬥中 GS 工作執行緒 89% 時間在等待（只佔一顆核心約 11%）。熱點是 PCSX2 上游已向量化或
  為正確性必需的程式（`ClearGSLocalMemory` 1.1%、材質雜湊、頂點範圍、雜湊快取老化）；移植的橋接層 0.4%。動 GS 之前
  先剖析；重建 GS 可能遺失 GS 端修正，部署前要比對新舊 DLL 的字串。
- **建議但尚未做：** 發行包附上預熱的 `shadercache_pcsx2`（DX11 快取與顯示卡無關，首次遊玩就能避開大部分著色器編譯
  卡頓；GS DLL 或 GS 選項一改就要重新產生）；以及先把 DX11 計時查詢（`GSDevice11`）以「GPU 時間」放進效能資訊，再做
  任何 GPU 優化。

## 3. 動畫（CG）

- **IPU（硬體 MPEG 解碼）遊戲：** 資料不夠時，IDEC 以前會還原到指令開始的快照，每來一段新輸入就整格重解（3–4 倍
  工作量）。改成在每個巨集區塊邊界記下檢查點、從那裡續解。參考 IDCT 跳過全零係數列（加上 ±0.0 不會改變總和）。兩者都
  逐位元相同；量測的開場 CG 每 5 秒 IPU 時間由 1041–1452 ms 降到 423–536 ms，EE 執行緒平均 / p95 由 40.6% / 68.3% 降到
  23.5% / 28.2%；只給 0.35 顆核心時，CG 由 41 提升到 58 場/秒。
- **MPEG HLE（FFmpeg）遊戲：** 給 `av_parser_parse2` 的資料要先複製並補 `AV_INPUT_BUFFER_PADDING_SIZE` 個零位元組
  （SIMD 起始碼掃描會讀過頭）；MPEG-1/2 用 slice 多執行緒（`min(4, 核心數/2)`，`PS2_MPEG_THREADS=1` 改回單執行緒）。
  要把解碼移出全域鎖之前，先量 `mpeg_decode` 計數。

## 4. 主機作業系統

- **關閉 EcoQoS：** `SetProcessInformation(ProcessPowerThrottling)` 關掉執行速度節流（Windows 接受時也保留計時器
  精度）。否則失焦、用電池或省電模式時，Windows 可能把遊戲放到效率核心並降頻。
- **執行緒優先權：** 4 個以上邏輯核心時，EE 與 GS 執行緒高一級（`PS2_THREAD_PRIORITY=0` 恢復一般）。以 12 個背景
  忙碌行程實測：有優先權維持 30 遊戲幀/秒，沒有只剩 13–18。它改變主機時序，會讓載入階段約 100 位元組的客體狀態與一般
  優先權時不同（戰鬥本身沒有分歧）；連線對戰雙方必須使用同一版本與設定。
- **堆疊：** 生成函式的框架可能非常大（量測的移植有 0.7 MB，巢狀呼叫會撐爆 1 MB 堆疊）。執行器連結 `/STACK:16777216`
  （只保留位址空間）。
- **崩潰報告：** 未處理例外過濾器與 `std::terminate` 處理器寫出 `logs\crash-<時間>.dmp` 與一行 `[crash]`（錯誤碼、
  模組＋偏移），只保留最新幾份；`PS2_TEST_CRASH=<秒>` 用來測試。量測工作中，它上線第一天就抓到一次真實的堆疊溢位。

## 5. 執行時期熱點（EE 執行緒）

戰鬥時 EE 執行緒的典型剖析：約 50% 等待、客體 AOT 程式約 19%、執行時期約 11%、分派與排程約 7%、microVU 輸出約 6%、
記憶體與 DMA 約 4%、GIF/VIF 約 3%。

- 去掉不是工作的每次呼叫成本：每次客體呼叫都會評估的診斷掛鉤（開關在啟動時讀一次就好）；每個區塊都會走到的函式內
  `static const x = getenv(...)`（MSVC 每次呼叫都要檢查 TLS 初始化守衛；改用第一次使用時讀取的一般靜態變數，且要在
  PS2_ 環境別名安裝之後）。
- `dispatchGuestBranch`：生成函式表直接命中時只查一次，不再 `hasFunction()` + `lookupFunction()` 各查一次。每次呼叫的
  排程檢查點要保留（客體時序），查表這層也要保留（遊戲替換、修改過 ELF 的直譯器、可重定位模組都靠它）。
- 只對執行時期函式庫做連結時期程式碼產生（`ps2_runtime` 設 `INTERPROCEDURAL_OPTIMIZATION_RELEASE`，執行器 `/LTCG`），
  一萬多個生成碼檔案不要。生成碼加 `/GS-`。
- **不要把逐指令輔助函式 `__forceinline` 進生成碼。** 以一個 3.2 萬行的生成函式實測：

  | 標頭寫法 | 堆疊框架 | 指令數 |
  | --- | --- | --- |
  | 原始 | 689 KB | 47.7 萬 |
  | 全部強制內聯 | 1,607 KB | 93.7 萬 |
  | 只內聯 3 個暫存器輔助函式 | 1,126 KB | 71.5 萬 |
  | 只內聯位址判斷 | 740 KB | 55.1 萬 |
  | 不強制內聯，環繞 RAM 存取移出去 | 689 KB | 46.5 萬 |

  強制內聯版每場戰鬥都堆疊溢位。改動生成碼會引用的標頭時，單獨編譯最大的生成檔，看它的框架大小。
- GIF：掃描 SIGNAL/FINISH/LABEL 時，沒有 A+D 描述的 PACKED 區塊整段跳過。VIF1 UNPACK V4-32（無遮罩、無模式）：改為
  16 位元組複製，用差異測試 `tools/diag/vif1_unpack_diff` 驗證（300 萬次隨機測試全部相同）。
- 量測的戰鬥中，兩輪合計 EE 執行緒平均 50.5% → 46.9%；幅度小，符合這些項目的預期。沒做：跳過客體等待 VSync 的空轉
  （屬於遊戲 profile；主機追趕已限制它）、生成碼之間直接呼叫（要重新產生全部生成碼並處理替換與直譯器路徑）、延後計算
  VU0 MAC 旗標。

## 6. 給玩家的診斷工具

- 經由 GS 畫面文字顯示一行效能資訊，每秒更新：`30 fps (60 fields/s) | EE 48% | GS 10% | D3D11 3x`。EE 接近一顆核心
  100% = CPU 不夠；幀數減半但場數穩定 = 客體跟不上；GS 高而 EE 低 = GPU 或驅動。
- 診斷包（zip：摘要含 CPU/系統/記憶體/GPU/繪圖器/最近取樣/崩潰數、執行記錄、GPU 記錄、設定檔、最新崩潰傾印；絕不含存檔）。兩者都是 `ps2_host_report` 的服務（`setOverlay()`、`exportDiagnostics()`），由各移植自行接到設定或選單；隱藏測試可用 `PS2_PERF_OVERLAY=1` 直接開啟效能顯示。

## 7. 新移植的檢查清單

1. EXE 與 DLL 的靜態指令集掃描；開機與 VU 測試的 SDE 指令檢查。
2. GS 開啟失敗的退回路徑（`PS2_GS_TEST_OPEN_FAIL=1`）；`[cpu]`、`[host]`、`[gpu]` 行都有出現。
3. 動畫與最重的遊玩場景的單執行緒 CPU；剖析瓶頸執行緒。
4. 解碼器改動用逐位元雜湊；執行時期改動用跨版本決定性（含同版本雜訊基準）。
5. 弱機對照（新舊版同時）與背景負載測試，看遊戲幀/秒。
6. 改動生成碼引用的標頭後，掃描堆疊框架大小。
7. 崩潰傾印測試；診斷包測試。
8. 只宣稱實際跑過的：一台電腦不等於所有電腦；只跑開機的 SDE 不等於跑過 3D。

## 開關（測試與支援）

`PS2_GS_TEST_OPEN_FAIL=1`、`PS2_TEST_CRASH=<秒>`、`PS2_THREAD_PRIORITY=0`、`PS2_GPU_TIER=...`、`PS2_PERF_OVERLAY=1`、
`PS2_MPEG_THREADS=<n>`、`PS2_IPU_IDEC_RESTART=1` 與 `PS2_IPU_IDCT_REF=1`（A/B 用的舊 IPU 路徑）、`DOD_PERF=1`（階段計數、
IPU 報告與雜湊）、`DOD_FPS=1`（每 300 場的幀數與場數）。
