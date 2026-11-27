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

## 十五、2026-11-27：跨裝置相容性與 runtime 額外負擔（共用主機服務）

取自某個移植 2026-11-21 的審查與修正（完整說明：[docs/CROSS_DEVICE_OPTIMIZATION.zh-TW.md](docs/CROSS_DEVICE_OPTIMIZATION.zh-TW.md)）。該移植的累積差異裡還夾帶那個移植自己的改動（可重定位模組支援、EE 解譯器、遊戲內選單、貼圖邊緣規則、診斷工具），這些都沒有收進來，留下的每個區塊都先分類過。只在一台電腦上量測（12 執行緒 Alder Lake CPU、獨立顯卡、Windows 10）；舊 CPU（無 AVX）的行為是用 Intel SDE 檢查，不是真實舊 CPU。增量補丁：`patches/PS2Recomp_fixer_compat_perf_20261127.diff`。

| 位置 | 內容 | 原因／依據 |
|---|---|---|
| `kzvu` `CMakeLists.txt`、`shim/unity/KzvuMicroVU.cpp`、`src/kzvu.cpp` | kzvu 不再用 `/arch:AVX2`；兩處明確的 256 位元寫法（`kzvuVu1CodeChanged`、`CopyVf31`）改成 128 位元 SSE4.1 | 沒有 AVX 的 CPU（Nehalem／Westmere）會在靜態初始化時遇到非法指令，第一個 VU1 程式又會再停一次。microVU JIT 仍在執行期選 SSE4／AVX／AVX2；PCSX2 自己的基線就是 SSE4.1 |
| `ps2xRuntime` `ps2_host_cpu.[h,cpp]` | `cpuMeetsBaseline()`：記錄一行 `[cpu]`（型號、核心數、SSE4.1/4.2、有作業系統支援的 AVX、AVX2、FMA、BMI2），缺 SSE4.1 時回報而不是當機。`examples/generic_runner` 會呼叫 | 讀的是 OSXSAVE／XCR0，不只是 CPUID 位元 |
| `ps2xRuntime` `gs/dod_pcsx2_gs.[h,cpp]` | `attach()` 在遊戲執行緒啟動前先在 GS 工作緒開啟 GS，裝置或交換鏈確定建立失敗時退回內建 CPU 光柵與主機端顯示。`PS2_GS_TEST_OPEN_FAIL=1` 模擬失敗。新增 `flips()`、`scale()`、`setOsdLine()` | 開啟失敗以前會讓視窗整個工作階段什麼都不畫 |
| `pcsx2gs` `GSDevice11.cpp`、`GSTextureCache.cpp` | `EndPresent` 對失敗的 `Present` 只記錄一次並附裝置移除原因。`PrepareDownloadTexture` 檢查新建立的貼圖（上游檢查的是擁有者指標的位址，永遠非空），失敗時保留舊貼圖 | 暫存配置失敗卻回報成功，呼叫端會解參考空指標 |
| `ps2xRuntime` `Kernel/Stubs/MPEG.cpp` | FFmpeg 解析器從可重用、尾端補零的副本讀取（`AV_INPUT_BUFFER_PADDING_SIZE`）；MPEG-1/2 切片多執行緒（4 核以上取 `min(4, 核心數/2)`，`PS2_MPEG_THREADS=1` 還原單執行緒）；新增 `mpeg_decode` 計數 | FFmpeg 要求輸入後面有可讀的補位。切片多執行緒輸出逐位元相同；沒有拿 MPEG-HLE 遊戲實測 |
| `ps2xRuntime` `ipu/ps2_ipu.c` | 資料不足的 IDEC 從上一個 macroblock 邊界繼續，不再從第一個重新解整個畫面；IDCT 跳過全零係數列（運算與順序相同）。`PS2_IPU_IDEC_RESTART=1`／`PS2_IPU_IDCT_REF=1` 還原舊路徑 | `DOD_PERF` 下每 16 MiB 的 FNV-1a 輸出雜湊，到 352 MiB 的每個里程碑都與舊路徑一致；量測的 CG 上 IPU 時間約減半 |
| `ps2xRuntime` `ps2_host_os.[h,cpp]` | `configureHostProcess()`（關閉 EcoQoS 節能降頻）、`setCurrentThreadRole()`（4 個以上邏輯 CPU 時 EE 與 GS 執行緒高一階，`PS2_THREAD_PRIORITY=0` 維持一般；GS 執行緒有名稱）、`installCrashHandler()`（`logs\crash-<時間>.dmp` 加一行 `[crash]`，保留最新 4 個）、`gpuReport()`／`autoScaleCap()`（內顯 2 倍、獨顯 4 倍；測試用 `PS2_GPU_TIER`） | 12 個背景程式滿載時，提高優先權讓遊戲維持 30 幀/秒，一般優先權則掉到 13-18 |
| `ps2xRuntime` `ps2_host_report.[h,cpp]` | 效能顯示列（`30 fps (60 fields/s) | EE 48% | GS 10% | D3D11 3x`）與診斷 zip（摘要、執行記錄、GPU 記錄、設定檔、最新崩潰傾印；絕不含存檔）。移植呼叫 `setOverlay()`／`exportDiagnostics()` 才啟用；測試用 `PS2_PERF_OVERLAY=1` | 一張截圖就能分辨 CPU 不夠、GPU 不夠還是遊戲本身慢 |
| `ps2xRuntime` `ps2_runtime.cpp`、`EeScheduler.cpp`、`ps2_memory.cpp`、`dod_vif1_unpack.h`、`gs/ps2_gif_arbiter.cpp`、`ps2_runtime_macros.h`、`CMakeLists.txt` | 行為不變的熱路徑減負：直接派發命中時只查一次表；每次呼叫都有保護的 static 改成普通 static；越界例外移到內聯之外；環繞的 RAM 存取移到內聯之外；VIF1 UNPACK V4-32（無遮罩、無 STMOD）改成 16 位元組複製；沒有 A+D 暫存器的 PACKED GIF tag 整段跳過。`ps2_runtime` 用 LTCG 連結（`PS2X_RUNTIME_LTCG`，預設開），runner 加 `/GS-` 與 16 MB 堆疊 | 用 VIF1 差異測試（3,000,000 次）與堆疊框大小掃描檢查；強制內聯試過後捨棄（最大函式的框大小加倍，預設堆疊會溢位） |
| `tools/perf/` | `thread_cpu_sample.py`、`thread_profile.py`（搭配連結器 map 做 RIP 取樣）、`cpu_cap.py`（弱機 CPU 上限）、`isa_scan.py`、`frame_sizes.py` | 與遊戲無關的量測工具；用法見指南 |

未涵蓋：真實的無 AVX 或內顯機器、MPEG-HLE 播放，以及其他遊戲的行為。指南列出開關與新移植的檢查清單。
## 十六、2026-11-27：VCALLMSR 與 VU 記憶體視窗（硬體語意）

移植某個遊戲時發現的兩個 EE 端缺陷，已對照 EE 手冊與 PCSX2 檢查。兩者都是硬體行為，與任何遊戲無關。增量補丁：`patches/PS2Recomp_vu_window_vcallmsr_20261127.diff`。

| 位置 | 內容 | 原因／依據 |
|---|---|---|
| `ps2xRecomp` `vu_translation_helpers.cpp` | `VCALLMSR` 從 CMSAR0 啟動 VU0 微程式（`(ctx->vu0_cmsar0 & 0x1FF) << 3`）。產生碼以前讀的是 `ctx->vi[27]` | VCALLMSR 唯一的編碼指名 VI27，在 CTC2/CFC2 編號裡就是 CMSAR0（EE Core Instruction Set Manual；PCSX2 讀 `VU0.VI[REG_CMSAR0]`）。`ctx->vi[]` 只有 VI0..VI15，舊寫法讀到陣列之外，從不相干的位址啟動 VU0。測試 `VCALLMSR starts VU0 at CMSAR0` |
| `ps2xRuntime` `runtime/ps2_address.h` | 特殊位址範圍涵蓋四塊 VU 記憶體，從 VU0 微記憶體 `0x11000000` 到 VU1 資料 `0x1100FFFF`。舊範圍從 VU0 資料開始、到 VU1 微記憶體結尾為止 | 產生碼對 VU1 資料記憶體（與 VU0 微記憶體）的寫入走了 RAM 快速路徑，被遮罩寫進 RDRAM，永遠到不了 VU1。測試 `generated stores treat the whole VU window as special and reach VU1 data`（邊界、KSEG1 別名、128 位元寫入落在 VU1 資料而不是 RDRAM） |

同一個移植的 VIF1 飛行記錄器與其他診斷工具沒有收進來。

## 十七、2026-11-27：IOP 模組入口的 WaitEventFlag 與單一 sysmem/loadcore 記憶體池

後來的移植裡的兩個 IOP 修正（第一個已在不同移植間移植過）。增量補丁：`patches/PS2Recomp_iop_arena_waiteventflag_20261127.diff`。兩者都有合成 IRX 映像的 IOP 測試涵蓋；都還沒有在本基底上跑過任何遊戲。

| 位置 | 內容 | 原因／依據 |
|---|---|---|
| `ps2xIOP` `iop_kernel.[h,cpp]`、`iop_emulator.cpp` | 從模組入口常式（同步呼叫、沒有 IOP 執行緒可暫停）呼叫的 `WaitEventFlag` 會等工作執行緒與傳輸中斷，不再立刻回傳 `-418`：`SetEventFlag`、`DeleteEventFlag` 會喚醒呼叫端，旗標位元在通知當下就擷取，等待超過一個 IOP 秒就逾時並記錄，其他執行緒這段時間執行的指令數會還給入口的指令額度 | 等待工作執行緒的啟動常式會被放棄（或看到 `EVF_NOT_READY`）。測試：同一個時間片內先設後清、等待時刪除、忙碌的工作執行緒（約 370 萬條指令） |
| `ps2xIOP` `iop_memory.[h,cpp]`、`iop_sysmem.cpp`、`iop_module_loader.[h,cpp]`、`iop_emulator.cpp` | 載入的 IRX 映像與 `AllocSysMemory` 共用同一個 IOP 記憶體池：`0x00010000..0x001F0000`（1,920 KB；以前堆從 `0x00120000` 開始，模組區用不到、配置上限只有 832 KB）。可重定位模組從池中取得空間，`stopModule` 會釋放；固定位址的模組預留自己的空間。`AllocSysMemory` 模式 1（ALLOC_LAST）從頂端配置，配置／釋放／查詢都會正規化 KSEG0 位址，`QueryTotalFreeMemSize` 回傳總量（不是最大連續塊），大小運算不會繞回 | 真實 IOP 上載入的模組與 sysmem 配置共用同一個池。**這會改變所有遊戲得到的 `AllocSysMemory` 位址**；依賴舊配置的移植要自己檢查。測試：ALLOC_FIRST/LAST/ADDR、KSEG0、碎片化、超大與無效模式、可重定位 IRX 之後配置超過 1.3 MB、`stopModule` 歸還空間 |

沒有收進來：同一份來源補丁的 VBlank handler 保護。本基底的 `iop_vblank` 沒有遞送 handler（只實作 Wait 系列），在把遞送機制另外移植進來之前沒有東西可保護。

## 十八、2026-11-27：VIF1 V3-32 上傳路徑、位址檢查、執行結果、MPEG ingress 上限

從 DoD 與 Onimusha 3 兩個移植樹對照本基底的分類結果裡挑選（它們多數差異是本基底已經往前走了，或是遊戲內容；下表之外較大的項目是刻意留下的）。增量補丁：`patches/PS2Recomp_vif_hostsvc_20261127.diff`。

| 位置 | 內容 | 原因／依據 |
|---|---|---|
| `ps2xRuntime` `dod_vif1_unpack.h` | 帶遮罩位元的 UNPACK V3-32，當每個 MASK 列的 XYZ 都沒遮罩（略過模式、STMOD 關）：每個向量複製 12 位元組，W 來自資料，或依選擇取 ROW、COL | 與一般迴圈相同的位元組、位址與寫入週期步進；VU1 頂點資料流會用到。`tools/diag/vif1_unpack_diff` 新增 `v3` 模式專打這條路徑（隨機遮罩幾乎不會讓 XYZ 全未遮罩）：3,000,000 次聚焦測試全部相同，刻意改錯 W 來源在第 110 次就被抓到 |
| `ps2xRuntime` `runtime/ps2_address.h` | `Ps2IsSpecialAddress` 先判斷是否低於 32 MB | 所有特殊區域（BIOS、暫存記憶體、I/O、GS 暫存器、VU 視窗、kseg2/3）都在 RAM 之上，結果不變。測試 `special-address RAM early-out keeps the full classification` 在整個 4 GB 空間與各區域邊界上和完整分類比對。不強制內聯（見 `ps2_runtime_macros.h` 的說明） |
| `ps2xRuntime` `PS2Runtime::executionFailed()`、`generic_runner` | 一次執行遇到 guest 例外或執行了映像裡沒有的函式（即使缺函式政策是繼續）就為真；通用 runner 此時以 1 結束 | 被捕捉的 guest 例外以前會被當成成功結束回報給 shell。把「執行到缺失目標」視為失敗是一種政策選擇；想要舊行為的移植忽略這個存取函式即可。沒有自動化測試涵蓋 |
| `ps2xRuntime` `dod_mpeg_dma_ingress.h`、`dod_to_ipu_dma.h` | MPEG DMA ingress 上限的 `capacity()` 與 `DOD_MPEG_INGRESS_KB`（64..8192）；CH4 蒐集使用與佇列相同的上限。預設仍是 2 MB | 有個移植需要 384 KB 上限，讓影片函式庫停掉整個環的 CH4 並只送下一張圖需要的資料；那是個別遊戲的發現，所以只取機制 |

看過但沒收：雙人輸入與逐玩家震動（約 600 行，必須重新併入本基底的輸入模型）、遊戲內選單（依賴較新的 GS 選單表與雙人輸入）、EE 停滯監看的 IOP 領先上限（與本基底的設計相反，頂多做成選用政策）、延遲 VIF1 鏈的記憶化（只有效能，它的差異測試在該移植裡）、各移植專屬的診斷。

## 十九、2026-11-27：IOP 領先上限（選用）與延遲 VIF1 鏈記憶化

第十八節「沒收」清單裡後來又收進來的兩項。增量補丁：`patches/PS2Recomp_iop_lead_vif_memo_20261127.diff`。

| 位置 | 內容 | 原因／依據 |
|---|---|---|
| `ps2xRuntime` `ps2_iop_lead.h`、`ps2_runtime.cpp`（EE 停滯監看） | `DOD_EE_STALL_WATCHDOG=capped`：EE 執行緒被卡住時，IOP 最多只能比 EE 領先一個 NTSC 場（4,920,115 個 EE 週期），不再無上限跟著真實時間線跑。**預設關閉**；`0` 仍是停用監看，預設行為不變 | 預設情況下被卡住的 EE（讀碟、編譯著色器）不會讓聲音佇列斷糧，但停滯期間 IOP 會遠遠跑在 EE 前面，把語音或音樂綁在 EE 畫面上的遊戲可能漂移（有個移植看到長時間載入之後語音比事件動畫早）。上限是用「停滯超過聲音佇列時可能出現的聲音空檔」換掉這個漂移。哪種取捨正確是個別遊戲的發現，所以做成開關。`host_timing_tests` 的 `iop-lead` 案例涵蓋上限、接近 `UINT64_MAX` 時不繞回的運算，以及自訂上限 |
| `ps2xRuntime` `ps2_memory.cpp`、`ps2_vif1_interpreter.cpp` | `dodVif1Ensure` 回傳尚未複製的第一個區段在鏈中的偏移（或 `UINT32_MAX`）；`processVIF1Data` 記住它，低於它的位元組不再重複詢問 | 直譯器每個指令都會詢問；那個移植在一段遊玩擷取中量到詢問次數從 1,200,719 降到 164,538。區段仍然只在第一次需要資料時才複製，所以「標籤之間的 GS 中斷看得到新資料」這個不變量不變。新測試 `VIF1 chain data is read when its tag is reached, not when the chain starts`（第一個封包之後由回呼改寫第二個 DIRECT 封包的來源 RAM）：這次改動前後都通過，用 `DOD_NOLAZY=1` 則失敗，所以確實守住延遲複製。詢問次數沒有在這裡重新量測 |
