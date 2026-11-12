# 我們對 PS2Recomp 的改進（相對上游 ran-j/PS2Recomp eb8688b）

完整差異：`patches/PS2Recomp_vs_upstream_eb8688b.diff`（97 個檔，+17,351／−494 行；在上游 eb8688b 原始碼裡 `git apply -p1` 可重現本資料夾的內容，只差換行符）。
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
- 高清影片路線不再讓影片卡死：它的 CH4 佇列遠比 IPU 超前，每段影片約第 26 張時會讀到 CD 還沒補上的環形緩衝區（上一段影片的殘留、再重送影片開頭）；殘留裡的序列結束碼以前會讓影片提前結束，遊戲就永遠等不到畫面。現在這條路線的序列結束碼不再結束影片（由遊戲自己的 sceMpegReset 結束），高清畫面用完就沿用最後一張。超前讀取本身（讓 CH4 依 IPU 速度送資料）尚未處理（151）

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

`DOD_PERF`、`DOD_FPS`、`DOD_FRAME_CAPTURE_DIR`、`DOD_GS_CAPTURE`、`DOD_LAYER_LOG`、`DOD_VIF_EVENTS`、`DOD_ADMA_WATCH`、`DOD_SPU2_WAV`／`DOD_SPU2_STATS`、`DOD_RAM_DUMP`、`DOD_HANG_DUMP_AT`、`DOD_RNG_TRACE`、`DOD_PAD_SCRIPT`、`DOD_TEST_AUTOSTART`、`DOD_TEST_EXIT_AFTER`、`PS2X_PRESENT_STATS` 等。`DOD_LINE_CAPTURE_DIR`（按 F8 或 `DOD_LINE_CAPTURE_AT=<秒數,…>`）：存 GS 輸出、之後幾幀每次繪製的 PCSX2 傾印與逐筆記錄（`dod_draws.tsv`、`dod_sprites.tsv`，以及把繪製分成 3d／particle／world-sprite／hud／post／fill／cg 的摘要；`DOD_LINE_CAPTURE_MODE=light|ab`）（148）。`DOD_PRESENT_SHOT=<資料夾>:<第幾次呈現>:<每幾次>:<張數>`：含選單的最終畫面（150）。`DOD_MPEG_ES_DUMP=<資料夾>`：存下高清路線每段影片收到的視訊資料，用來和影片原檔比對；`DOD_CG_TEST_SEQEND_AT=<張數>`：在那一張插入序列結束碼（151）。外部腳本在 `tools/`（反組譯、指令／HLE／IOP 匯入清點、GIF 解析、執行緒取樣剖析、和 PCSX2 對照、WAV 分析、GitHub issue 抓取）。

## 十二、只屬於新鬼、換遊戲時要改或拿掉的地方

| 位置 | 內容 | 換遊戲時 |
|---|---|---|
| `examples/dod_runner/main.cpp`（不在建置內） | 新鬼的入口：dod.ini、依位址掛的 hook、換盤、寬螢幕開關、亂數追蹤、高清紋理場景預讀（場景號 0x7E1344） | 當範本，自己寫該遊戲的 `ps2xRuntime/src/runner/main.cpp` |
| `ps2xRuntime/src/lib/Kernel/Stubs/GS.cpp` `sceGsSyncV` | libgraph 紀錄位址不再寫死：`game.ini` 的 `gparam`、`PS2_GS_GPARAM` 或 `setGsGParamAddress()`（原遊戲是 `0x003FD4A0`） | 遊戲的 libgraph 是原生執行時，傳入該遊戲 `sceGsGetGParam` 回傳的位址；沒給就用 HLE 自己的紀錄 |
| pcsx2gs `compat/Bridge.cpp` `ApplyGameFixes` | PCSX2 的逐遊戲修正（`GameIndex.yaml`）原本固定成原遊戲的條目，現在不套用任何一組 | 把該遊戲的條目放進 `gs_options`（`blending=…;half_pixel_offset=…;native_scaling=…;bilinear_upscale=…`） |
| `ps2xRuntime/src/lib/Kernel/EeScheduler.cpp`、`ps2_runtime.cpp` 診斷 | `DOD_PAD_TRACE` 的遊戲位址、`DOD_RAM_DUMP` 範圍、`DOD_VU0_TRACE` 範圍原本是原遊戲的 | 現在是參數：`PS2_PAD_TRACE_ADDRS`、`PS2_RAM_DUMP_RANGE`、`PS2_VU0_TRACE_RANGE`（docs/GAME_PORTING.md）；所有 `DOD_*` 開關也可寫成 `PS2_*` |
| `ps2xRuntime/src/lib/Kernel/Stubs/dod_hd_cg.h`（MPEG.cpp 引用） | 1080p CG 替換：讀執行檔旁 `movies_hd\movies.tsv`（新鬼影片編號） | 沒有 `movies_hd` 資料夾時播放前就回退；不用就設 `DOD_CG_HD_DIR=0` 或拿掉 |
| `ps2xRuntime/src/lib/gs/dod_cg_cadence.h` | 劇情 CG 換幀節奏，靠新鬼產生碼裡的 hook 送資料 | 新遊戲沒有那些 hook；可用 `DOD_CG_CADENCE=keep` 關閉 |
| `ps2xRuntime/src/lib/gs/dod_widescreen.h`＋pcsx2gs `GS/DoDWidescreen.h`、`GSState.cpp` 寬螢幕區塊 | 新鬼的 16:9（世界投影常數在新鬼產生碼裡） | runner 只在 `aspect=16:9` 時送 `widescreen=1`；新遊戲不要開 |
| pcsx2gs `GS/DoDHdStatic.h`、`DoDHdCg.h` | 新鬼高清靜態頁、CG 的 GS 端 | 依新鬼紋理／影片資料；新遊戲沒有對應資源 |
| `ps2xRuntime/CMakeLists.txt` | `dod_branding.rc.in`（圖示資源，現在沒有就略過）、`dod_utf8.manifest`（通用，建議保留） | 自己的圖示放 `src/runner/dod_branding.rc.in` |
| `ps2xRuntime/src/lib/ps2_cheats.[h,cpp]`（`ps2_runtime.cpp`、`ps2_pad.cpp` 呼叫；滑鼠輸入在 `gs/dod_pcsx2_gs.cpp`） | 通用的 F2 遊戲內金手指選單，**選用功能，預設屏蔽（CMake `PS2X_ENABLE_CHEAT_MENU` 預設 OFF，改編入空殼 `ps2_cheats_disabled.cpp`），如有需要可以使用**：項目是從該遊戲自己的 `cheats.ini` 讀入的 RAM 寫入（格式見 `docs/CHEATS.md`）；本倉庫不附任何遊戲的金手指。每次啟動全部關閉 | 靠左面板、滑鼠／鍵盤／手把、跟著畫面大小縮放。改遊戲指令的金手指檔案不支援（重編譯函式不從記憶體讀指令）；需要的話用選用的 F3 修改器（`ps2_trainer.h`，`PS2X_ENABLE_TRAINER` 預設 OFF，見 `docs/TRAINER.md`）在自己產生碼裡加開關 |

另外 `[DoD]` 修正裡有一些是「新鬼先踩到、但依 PCSX2／實機行為修」的（例如 SPU2、IOP 臨界區、VIF1），對其他遊戲同樣有效；若新遊戲出現行為差異，先查 `docs/DoD_RUNTIME_PATCHES.md` 對應列的說明與環境變數開關。

## 十三、2026-11-12 維護：後續移植的通用修正、選用金手指工具、新移植腳手架

做法：把以本基底延伸的三個本地移植樹，各自對照它起步的公開 commit，與公開 `main`（`5e907ef`）逐檔比對內容，只收共用機制；遊戲資料、產生碼、移植專屬掛鉤與診斷都留在各自的移植裡。增量補丁：`patches/PS2Recomp_maint_20261112.diff`。

| 位置 | 內容 | 原因／依據 |
|---|---|---|
| `ps2xRecomp` `types.h`、`config_manager.cpp`、`ps2_recompiler.cpp` | `[general]` 新增 `native = ["名稱@0x位址", ...]`：指定位址的函式即使名稱和 runtime 取代的 SDK 函式相同，也保留為重編譯的原始碼（不受自動名稱綁定、`stubs`、`skip`、J/JAL 重定位降級影響）。只寫名稱會被拒絕；位址找不到函式時 `initialize()` 失敗 | 某遊戲自己的單參數 `SetEventFlag`／`ClearEventFlag`（遊戲記憶體裡的位元陣列）被名稱綁定到 kernel 事件旗標 HLE，遊戲自己的事件狀態因此默默遺失。測試：`native addresses override automatic and explicit SDK stubs` |
| `ps2xRecomp` `control_flow_emitter.cpp` | 在延遲槽位址恢復時，只有確實停在「這個」分支延遲槽的 context（`in_delay_slot && branch_pc == 該分支`）才走該分支的控制流；剛好是別的分支延遲槽的跳轉目標不再被誤導 | 沿用上游 issue #260 的恢復規則，去掉它的誤判 |
| `ps2xRecomp` `ps2_recompiler.cpp`、`function_table_emitter.cpp` | 巢狀在真實函式裡的匯入 `entry_*` 區塊改由完整的擁有者函式派發（取最內層、結果固定），排程器恢復時不會從截斷的區塊掉出去 | 只有合成的 `entry_*` 名稱會讓位；具名函式與 HLE 綁定維持原登錄 |
| `ps2xIOP` `iop_kernel.[h,cpp]`、`iop_emulator.cpp` | `thmsgbx` 信箱：CreateMbx、DeleteMbx、SendMbx/iSendMbx、ReceiveMbx/PollMbx、ReferMbxStatus/iReferMbxStatus，含 FIFO／優先序佇列與等待者、封包環狀連結、KE_* 錯誤碼、刪除／ReleaseWait 喚醒、暫停狀態；`QueryIntrContext` 正確回報中斷／回呼上下文（DMA、guest 回呼、計時器回呼）；執行緒外的 `ReceiveMbx` 會推動真正的 IOP 生產者，不回傳假的空結果 | ps2sdk `iop/system/threadman` 的 `thmsgbx.h`／`thmsgbx.c`、`kerr.h`（`f1ca58a468ea`）。新測試組 `ps2_iop_mailbox_tests`（6 項） |
| `ps2xRuntime` `Kernel/Stubs/MemoryCard.cpp` | 記憶卡檔案描述符依 MCMAN 分配：從 0 起找最低空位、關閉後重用、各自獨立的讀取位置 | 期待第一個檔案拿到描述符 0 的遊戲。擴充 `ps2_runtime_io_tests` |
| `ps2xRuntime` `Kernel/Stubs/Pad.cpp` | libpad HLE 支援震動：`scePadSetActAlign` 檢查並保存致動器對應、`scePadSetActDirect` 驅動小馬達（開關）與大馬達（0–255）、`scePadInfoAct` 回傳 DualShock 2 致動器描述；開關埠與重設時停止馬達 | 走 libpad EE HLE 路徑的遊戲原本沒有震動 |
| `ps2xRuntime` `vu/dod_kzvu.cpp` | `PS2_VU1_CLAMP`／`PS2_VU0_CLAMP`（0..3，預設 1）分別設定 microVU 夾制模式；無效值會報告並維持預設 | PCSX2 GameIndex 對部分遊戲用較強的夾制；VU0 不能沿用 VU1 的遊戲專屬值 |
| `ps2xRuntime` `Kernel/Stubs/GS.cpp`（sceGifPk 關閉封包）、`gs/ps2_gif_arbiter.cpp`、`vu/ps2_vu1_core.cpp`（XGKICK 長度） | GIFtag 的 FLG=3（IMAGE2）在所有解析處都等同 IMAGE：NLOOP 以 qword 計，不是 NREG 組數。GS 前端與 VIF1 解譯器原本就已如此 | 上游 PS2Recomp `e5636981a1`（#384）、PCSX2 `Gif_Unit.h` 的 `GIF_FLG_IMAGE2`；移植上游測試 `GIF FLG=3 (IMAGE2) packet writes host-to-local data like IMAGE` |
| `ps2xRuntime` `ps2_cheats_disabled.cpp`、`ps2_trainer.[h,cpp]`、`ps2_trainer_disabled.cpp`、`CMakeLists.txt`、`build_all.ps1` | **選用金手指工具，兩者預設皆為屏蔽（關閉）狀態，如有需要可以使用：** `PS2X_ENABLE_CHEAT_MENU`（F2，`cheats.ini`）與 `PS2X_ENABLE_TRAINER`（F3，各移植為自己的程式碼掛鉤登錄開關／選項的面板；關閉時查詢是編譯期常數）。`build_all.ps1 -CheatMenu`／`-Trainer` | `docs/CHEATS.md`、`docs/TRAINER.md`；測試 `test_ps2_trainer.py`、`input_readers` 的 `trainer-capture` |
| `tools/new_port.py` | 從光碟映像或開機 ELF 在倉庫外建立移植資料夾：依 SYSTEM.CNF 取出並檢查開機 ELF、`recomp.toml`（已建置 ps2_analyzer 時用它，否則寫含 `native` 的最小範本）、`game.ini`、`port.json`、附確切指令的 `PORT_STEPS.md` | `tests/test_new_port.py`（合成的 ISO9660 映像） |
| `docs/ECOSYSTEM_SURVEY.zh-TW.md` | 30 個相關專案的調查：以內容判定的程式碼繼承關係、三種 IOP 路線（HLE 服務／解譯 IRX／重編譯 IRX）、VU、GS，以及本基底從中採用的部分 | |

## 十四、2026-11-12：執行期載入的程式碼（overlay），選用

生態調查列的第 1 個後續項目。其他專案處理遊戲執行中才載入的程式碼都是遊戲專屬的產生器；本基底的一個下游移植改用「載入位址處的標頭」判斷常駐映像，但位址與標頭格式寫死在 runtime 裡。這次把機制通用化：所有數值都由移植自己的設定提供，預設不登錄任何東西。用法見 [docs/GAME_PORTING.md](docs/GAME_PORTING.md) 第 6 節。增量補丁：`patches/PS2Recomp_overlays_20261112.diff`。

| 位置 | 內容 | 原因／依據 |
|---|---|---|
| `ps2xRuntime` `ps2_overlays.[h,cpp]` | 映像登錄：載入位址、大小、相對載入位址某個位移的 1..256 位元組簽章、該映像的 {guest 位址, 函式} 表與名稱。查詢時以 EE RAM 裡目前的簽章挑選映像（載入位址高的先試，所以大映像範圍內的小映像擁有自己的範圍）。登錄時複製並驗證全部資料（範圍、對齊、排序、名稱不重複），同一載入位址上若有某種 RAM 內容能同時符合兩個簽章就拒絕登錄。被動目錄讓產生碼自行加入，`installCatalog()` 才會啟用。查詢不加鎖，多個 guest 執行緒同時查詢也安全（每個區域的常駐快取每次都重新驗證） | 同一載入位址常有多個映像；只有部分映像才有的簽章（例如共用的魔術字）會派發到錯的映像，所以歧義在登錄時就報錯，不在執行時猜 |
| `ps2xRuntime` `ps2_runtime.cpp`、`runtime/ps2_memory.h` | `lookupFunction`／`hasFunction` 先過內聯的範圍檢查（一次減法加比較；沒登錄時一定不成立）才查登錄。常駐映像擁有整個範圍：不會落回被它覆蓋的主 ELF 程式碼。`loadELF` 一開始就清空登錄。缺函式報告會列出常駐映像。新增 `PS2Memory::getRDRAM() const` | 登錄屬於單一遊戲，和 `loadELF` 會重設的其他設定檔一樣 |
| `ps2xRecomp` `types.h`、`config_manager.cpp` | `[overlay]` 段：`name`、`load_address`、`signature_offset`、`signature_size`（名稱、載入位址、簽章大小必填），選填 `image_size`、`input_format`，原始映像另有 `code_offset`、`code_size`、`bss_size`、`entry`。嚴格驗證，未知鍵視為錯誤 | |
| `ps2xRecomp` `elf_parser.[h,cpp]` | 原始映像：沒有 ELF 魔術字的檔案在記憶體裡呈現為 ELF（資料／`.text`／資料／`.bss`，位於載入位址），既有的函式探索、Ghidra 函式表與 `entry_points` 都照常運作 | |
| `ps2xRecomp` `ps2_recompiler.cpp`、`code_generator.[h,cpp]`、`function_emitter.cpp`、`function_table_emitter.[h,cpp]` | overlay 輸出：命名空間 `ps2x_overlay_<名稱>`、檔名 `<名稱>_*` 並以相對路徑 include，不產生 ELF 啟動碼與密集函式表；`<名稱>_overlay.cpp/.h` 含入口表（與主表相同的入口、恢復點與 stub）、從映像讀出的簽章、`ps2x_register_overlay_<名稱>()` 與目錄項目。簽章必須是映像內已初始化的資料，每個函式都必須在映像範圍內 | 多個 guest 位址與函式名稱都相同的映像能連結進同一個執行檔 |
| `ps2xTest/src/ps2_overlay_tests.cpp`、`tests/overlay_registry_tests.cpp`、`tests/test_overlay_registry.py` | `ps2x_tests` 16 項（登錄、`PS2Runtime` 派發與 `loadELF` 清空、`[overlay]` 解析、原始映像與 ELF 輸入、單檔與逐函式輸出、拒絕的輸入），以及獨立的登錄測試：CTest（MSVC）與 g++ ASan/UBSan 各跑一次，含讓常駐快取不斷換手的多執行緒查詢 | 只用合成的映像與位址 |
