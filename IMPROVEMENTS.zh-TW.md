# 我們對 PS2Recomp 的改進（相對上游 ran-j/PS2Recomp eb8688b）

完整差異：`patches/PS2Recomp_vs_upstream_eb8688b.diff`（97 個檔，+17,262／−494 行；在上游 eb8688b 原始碼裡 `git apply -p1` 可重現本資料夾的內容，只差換行符）。
每一項的來龍去脈、驗證方式都在 `docs/DoD_RUNTIME_PATCHES.md`（下表括號裡的數字是那份記錄的列號）。程式碼裡的修改多半標有 `[DoD]` 註解，**這只是歷史標記，不代表只適用新鬼**；真正只屬於新鬼的部分在最後一節。

## 一、重編譯器 ps2xRecomp（產生的 C++ 正確性）

- MMI／FPU／VU0 巨集指令語義全面對照 PCSX2 修正：PMFHL.LW、PEXT/PPAC 類、FPU 比較／MIN／MAX、VU0 FMAC 更新 MAC 旗標、VCLIP 正負位元、CFC2/CTC2 讀寫 R 暫存器遮罩、VRNEXT LFSR（39、41、70、143 前置）
- 分支 BLEZ/BGTZ/BLTZ/BGEZ（含 likely／AL）改比 64 位元（56）
- 只經函式指標呼叫的函式補起點：`tools/find_missing_function_starts.py`、`scan_code_pointers.py`；switch 跳轉表目標：`scan_jump_tables.py`（42、83）
- 片段函式改走母函式入口：`prefer_parent_entries.py`（123）

## 二、EE 核心、執行緒、中斷

- 執行緒優先度允許 0、ChangeThreadPriority 回傳舊值、Suspend/WakeupThread 回傳執行緒 ID（13、15、64）
- VSync／alarm 以真實時間推進，不再因核心呼叫不計週期而只剩 2 Hz（17）；VBlank 用 NTSC 59.94 Hz（76），主機時間不再超實時償還（88、99）
- DI/EI 臨界區不被中斷或時間片搶占（66）；被遮罩的中斷保留狀態、重新允許時才進（52）；INTC_STAT 寫 1 清除語義（45）
- checkpoint 讓出時遞迴入口誤判為已返回（44）
- 中斷／alarm 處理堆疊不再從 RAM 頂端切（蓋掉遊戲堆疊）、用完回收（14、137）
- 每條客體執行緒的 VU0 暫存器分開，但 R（亂數）全執行緒共用、vf0 初始化為 (0,0,0,1)（58、143）
- SetupHeap 上限、SIF 命令封包不用遊戲 heap（72、74）
- 卡住傾印含客體執行緒表（139）

## 三、DMA、記憶體、計時器

- IPU DMA ch3/ch4、SPR DMA chain 模式、DMA 位址 bit31（25、26、51）
- D_ENABLEW→D_ENABLER、CHCR 讀取不清 STR、D_STAT 清除語義（53、65、52）
- DMA chain 第三層巢狀 CALL（114）、超過 4096 tag 不截斷（43）
- 任何寬度的寫入都會派送 DMAC 完成中斷（71）
- EE 計時器 T3 以 H-BLANK 計數、延遲計算的計時器（46、118）
- GS CSR.FIFO 讀值（69）

## 四、VIF／GIF／GS

- VIF1 UNPACK 語義對照 PCSX2（WL=0、fill、V2/V3/V4-5、STMOD、mask），並改成預先選好的快速路徑（43、121）
- VIF1 chain 改為邊執行邊讀（SIGNAL 處理器之後寫入的資料才讀得到）（28）
- GIF FLG=3、IMAGE 跨傳輸續傳、PATH3 續傳狀態在重設時清除（34、108）
- GS 暫存器 0x59–0x5F 不再當特權暫存器（33）；GS SIGNAL/FINISH 產生 EE 中斷（27）
- libvifpk 替換函式改寫 DMA 標籤 QWC 的根因（113）
- sceGsResetPath／sceVpu0Reset 真的重設（102）；GsSetCrt 寫 SMODE1（38）

## 五、PCSX2 硬體 GS（pcsx2gs.dll）與 VU1（kzvu）

- `third_party/src/pcsx2gs`：PCSX2 2954ebe 的 GS（DX11/DX12）包成 DLL，C 介面 `pcsx2gs_api.h`；runtime 端 `gs/dod_pcsx2_gs.cpp` 在獨立 GS 執行緒餵資料、子視窗呈現（36、37、67）
- GS 修正：PACKED 續傳越界、Target::Update 崩潰、場偏移（60、90、100）；高清紋理替換的上傳預算、保留、多執行緒載入、場景預讀（129、140）；FXAA/SSAA（130）；關窗崩潰（131）
- 全螢幕／無邊框：只由 D3D 子視窗呈現、父視窗不再交換黑色 OpenGL 畫面；獨佔全螢幕焦點判斷用主視窗（145）
- 放大解析度：特效 sprite 周圍不再出現 1 像素細線（小數座標、雙線性、圖集取樣；與 PCSX2 #5851／#6396 同類）。場景粒子的貼圖範圍向內縮「實際讀到的最粗 mip 層」的半個 texel，有無 mipmap 都處理（`DoDNativeParticleEdges`；`DOD_PARTICLE_EDGES=0` 關閉）（146）
- 顯卡選擇：`gs_options` 的 `adapter=<名稱>`；未指定時照 Windows「圖形設定」對該執行檔的選擇，否則用高效能硬體顯卡（以前 D3D11 固定用 adapter 0，很多筆電因此跑在內顯）；實際使用的顯卡寫到 `logs\gpu.txt`（147）
- 主機端選單 API v3（`pcsx2gs_api.h`）：靠左面板（`begin_panel`）、提示文字（tooltip）、ImGui 字型合併系統中文字型（150）
- `third_party/src/kzvu`：PCSX2 microVU（646df006a4）做成靜態庫取代上游 VU1Interpreter（50）；後備直譯器 65536 週期截斷修正（138）

## 六、IPU／MPEG 影片

- 完整 IPU（AC5 的 `ps2_ipu.c`，Apache-2.0，經 Urban Reign）：IDEC/BDEC/VDEC/CSC，快照重跑不丟資料（24、40、54、55）
- sceIpuInit HLE 不再寫死別款遊戲位址（29）；libdma Recv 系列（31）
- FFmpeg 解 MPEG 的 HLE 路徑（`PS2X_ENABLE_FFMPEG`）
- 高清影片替換（`dod_hd_cg.h`）改由背景執行緒預先解碼最多 8 張：遊戲執行緒不再拿著 MPEG 鎖解 1080p HEVC，播放與聲音不再卡頓（149）

## 七、IOP（ps2xIOP，直接跑原版 IRX）

- loadcore `start(argc, argv)`、SIFMAN 開機即初始化、sceSifBindRpc／GetOtherData、EE→IOP RPC 走真的伺服器執行緒（3、4、5、20、47）
- sysclib look_ctype_table、wmemcopy/wmemset（19、61）；stdio printf 格式化（7）
- 臨界區（CpuSuspendIntr）內不處理 DMA 完成、不切換執行緒（68）；LWL/LWR 延遲槽、heap 重用、執行緒外 WaitSema/DelayThread（75）
- IOP 跑在獨立主機執行緒（78、84）
- **SIO2＋DualShock2**（手把、震動）（32、127）
- **SPU2 完整模擬**：聲部、ADPCM、高斯插值、包絡／音量滑動對照 PCSX2、AutoDMA 速率、殘響（85、89、106、111、112、115）

## 八、光碟（CD／DVD）

- EE 端 `sceCdSearchFile` 直接索引 ISO9660 真實 LBN，sceCdCallback／sceCdRead 通知（8、9）
- 雙層 DVD9 第 1 層（95）；換盤（sceCdTrayReq）（80、81）

## 九、主機端

- 手把：SDL2 動態載入、Sony 手把、震動（`dod_gamepad.cpp`）（127）
- 限幀：單一 deadline pacer，VSync 關閉，遊戲時鐘不受影響（128、`dod_host_pacer`）
- UTF-8 程序代碼頁 manifest：任何字元的資料夾都能執行（144）
- 主機音量（`dod_audio_settings.h`）、按鍵設定解析（`dod_input_settings.h`）
- 雙顯卡筆電：範例 runner 匯出 `NvOptimusEnablement`／`AmdPowerXpressRequestHighPerformance`（自己的 runner `main.cpp` 照抄即可）（147）

## 十、效能

IOP 推進、chain 緩衝池、getenv 快取（77）；計時器（118）；GS 命令區塊不先清零（120）；VIF1 UNPACK 快速路徑（121）；排程器快速跳轉（122）。

## 十一、診斷工具（環境變數，平常不設就不作用）

`DOD_PERF`、`DOD_FPS`、`DOD_FRAME_CAPTURE_DIR`、`DOD_GS_CAPTURE`、`DOD_LAYER_LOG`、`DOD_VIF_EVENTS`、`DOD_ADMA_WATCH`、`DOD_SPU2_WAV`／`DOD_SPU2_STATS`、`DOD_RAM_DUMP`、`DOD_HANG_DUMP_AT`、`DOD_RNG_TRACE`、`DOD_PAD_SCRIPT`、`DOD_TEST_AUTOSTART`、`DOD_TEST_EXIT_AFTER`、`PS2X_PRESENT_STATS` 等。`DOD_LINE_CAPTURE_DIR`（按 F8 或 `DOD_LINE_CAPTURE_AT=<秒數,…>`）：存 GS 輸出、之後幾幀每次繪製的 PCSX2 傾印與逐筆記錄（`dod_draws.tsv`、`dod_sprites.tsv`，以及把繪製分成 3d／particle／world-sprite／hud／post／fill／cg 的摘要；`DOD_LINE_CAPTURE_MODE=light|ab`）（148）。`DOD_PRESENT_SHOT=<資料夾>:<第幾次呈現>:<每幾次>:<張數>`：含選單的最終畫面（150）。外部腳本在 `tools/`（反組譯、指令／HLE／IOP 匯入清點、GIF 解析、執行緒取樣剖析、和 PCSX2 對照、WAV 分析、GitHub issue 抓取）。

## 十二、只屬於新鬼、換遊戲時要改或拿掉的地方

| 位置 | 內容 | 換遊戲時 |
|---|---|---|
| `examples/dod_runner/main.cpp`（不在建置內） | 新鬼的入口：dod.ini、依位址掛的 hook、換盤、寬螢幕開關、亂數追蹤、高清紋理場景預讀（場景號 0x7E1344） | 當範本，自己寫該遊戲的 `ps2xRuntime/src/runner/main.cpp` |
| `ps2xRuntime/src/lib/Kernel/Stubs/GS.cpp` `sceGsSyncV` | 寫死新鬼 libgraph 參數位址 `kDodGsGParamAddress = 0x003FD4A0` | **必改**：換成該遊戲 `sceGsGetGParam` 回傳的位址，或改回不 HLE 這個函式 |
| `ps2xRuntime/src/lib/Kernel/Stubs/dod_hd_cg.h`（MPEG.cpp 引用） | 1080p CG 替換：讀執行檔旁 `movies_hd\movies.tsv`（新鬼影片編號） | 沒有 `movies_hd` 資料夾時播放前就回退；不用就設 `DOD_CG_HD_DIR=0` 或拿掉 |
| `ps2xRuntime/src/lib/gs/dod_cg_cadence.h` | 劇情 CG 換幀節奏，靠新鬼產生碼裡的 hook 送資料 | 新遊戲沒有那些 hook；可用 `DOD_CG_CADENCE=keep` 關閉 |
| `ps2xRuntime/src/lib/gs/dod_widescreen.h`＋pcsx2gs `GS/DoDWidescreen.h`、`GSState.cpp` 寬螢幕區塊 | 新鬼的 16:9（世界投影常數在新鬼產生碼裡） | runner 只在 `aspect=16:9` 時送 `widescreen=1`；新遊戲不要開 |
| pcsx2gs `GS/DoDHdStatic.h`、`DoDHdCg.h` | 新鬼高清靜態頁、CG 的 GS 端 | 依新鬼紋理／影片資料；新遊戲沒有對應資源 |
| `ps2xRuntime/CMakeLists.txt` | `dod_branding.rc.in`（圖示資源，現在沒有就略過）、`dod_utf8.manifest`（通用，建議保留） | 自己的圖示放 `src/runner/dod_branding.rc.in` |
| `ps2xRuntime/src/lib/dod_cheats.[h,cpp]`（`ps2_runtime.cpp`、`ps2_pad.cpp` 呼叫；滑鼠輸入在 `gs/dod_pcsx2_gs.cpp`） | 新鬼的 F2 遊戲內金手指選單：道具數量每幀寫入新鬼的 RAM 位址；改指令的金手指是 `g_dodCheats[n]` 開關，由腳本插進新鬼的產生碼（不在本倉庫）。每次啟動全部關閉 | 選單本身通用（靠左面板、滑鼠／鍵盤／手把、跟著畫面大小縮放）：換成該遊戲的項目與位址，或刪掉這個檔與兩處呼叫 |

另外 `[DoD]` 修正裡有一些是「新鬼先踩到、但依 PCSX2／實機行為修」的（例如 SPU2、IOP 臨界區、VIF1），對其他遊戲同樣有效；若新遊戲出現行為差異，先查 `docs/DoD_RUNTIME_PATCHES.md` 對應列的說明與環境變數開關。
