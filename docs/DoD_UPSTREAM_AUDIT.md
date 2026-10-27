# 參考專案問題／修正對照表

逐條核對參考專案（GitHub issue/PR、留言與各專案的進度文件）記錄的問題與解法，判斷新鬼武者是否受影響，並套用適用的修正。
原始資料：`work/diag/gh/full/`（`tools/scripts/diag/fetch_gh_issues.py` 抓取，`gh_digest.py` 摘要，`fetch_pr_diffs.py` 取 PR diff）。

狀態：**已套用**（本工程已有或本次移植）、**不適用**（附原因，多數經實測或掃描 ELF 確認）、**待辦**、**觀察**。
「DoD 用量」來自 `tools/scripts/diag/cop2_census.py`、`mmi_census.py` 與 ELF 掃描。

## 抓取範圍（2026-10-19）

| 專案 | issue/PR | 留言 | 文件 |
|---|---|---|---|
| ran-j/PS2Recomp | 357 | 249 | — |
| thiagordsfx/urban-reign-recomp | 5 | 2 | `docs/PROGRESS.md`（2200 行） |
| drakolordx7/KillzoneRecomp | 2 | 0 | `docs/findings.md`、`perf_research.md`、`patches/README.md` |
| KIexster/god-of-war-recomp | 137 | 23 | issue #62/#66 附 PDF |
| nathanialf/ico-pc | 126 | 566 | 原生重寫，issue 多為使用者端畫面／音效 |
| Sinan-Karakaya/DQ8-Recompiled | 61 | 91 | |
| AshfordFamily/recvx-decomp | 77 | 45 | 比對式反組譯，非 runtime |
| snuri00/ps2-web-recomp | 16 | 22 | |
| sal063/Ace-Combat-5-Static-recompilation | 11 | 6 | |
| danielgomesvieira2000/burnout3-pc | 8 | 5 | |
| jcsix694/def-jam-vendetta-recomp | 4 | 16 | |
| LightVelox/sotc-vibe-pc | 0 | 0 | `Docs/*.md` |

## A. 重編譯器（需重新產生程式碼）

| 來源 | 內容 | DoD 用量 | 狀態 |
|---|---|---|---|
| PS2Recomp #292 | VU0 巨集 FMAC 運算不更新 MAC 旗標（`cfc2 $vi17` 永遠 0）；寫入 vf0 未還原 | `cfc2 vi17` 28 處（如 0x17C464 的平面測試讀符號位） | **已套用**（build 211，SSE 版 `ps2_vu0_set_mac`，1500 處） |
| PS2Recomp #293、GoW #66 | VU0 VCLIP 正負位元相反、用帶號 w 比較 | `vclipw` 2 處（0x135CD0/0x135DCC，接著讀 clip 旗標） | **已套用** |
| PS2Recomp #321 | FPU C.EQ/LT/LE、MIN/MAX 與 VU MAX/MINI 以 IEEE 比較，指數 255（PS2 的 ±max，如除以零）被當 NaN | FPU 比較 2475、MIN/MAX 37、VU MAX/MINI 數處 | **已套用** |
| PS2Recomp #318 | VFTOI 溢位正數也得 0x80000000，應依符號飽和 | vftoi0/4 共 19 | **已套用** |
| PS2Recomp #357 | ADD/SUB/MUL 結果為 ±0 時走慢速路徑（上游實測整體快約 3 倍） | 全部 FPU 加減乘 | **已套用** |
| PS2Recomp #135、GoW | MOVZ/MOVN 複製 128 位元，應只搬低 64 位元 | — | **已套用** |
| PS2Recomp #304 | 函式最後一條是分支時延遲槽被換成 NOP | — | **已套用** |
| PS2Recomp issue #260 | 在「必定跳躍」分支（b、beq x,x、bgez $0、函式內 j）的延遲槽恢復時往下執行而非跳到目標 | — | **已套用**（自行實作，上游只有 issue） |
| PS2Recomp #193 | CFC2/CTC2 第 22 號控制暫存器應為 Q | cfc2 vi22 8、ctc2 vi22 4 | 已有 |
| PS2Recomp #317、#198 | VDIV/VSQRT/VRSQRT 規則（VRSQRT 忽略 fs 等） | vdiv 24、vsqrt 21、vrsqrt 4 | 已有（RUNTIME_PATCHES 41） |
| PS2Recomp #291、GoW #16 | MMI 置換指令通道順序 | pexew/pexcw 等 0；prot3w 2 | 已有（41，內嵌程式碼） |
| PS2Recomp #322 | MMI HI/LO 雙管線（PMULTW/PDIVW 等） | 上述指令 0；pmfhl.lw 5（已修）、mult1/div1 等正確 | 不適用／已有 |
| PS2Recomp #262/#268 | VCALLMSR 讀 vi[27] 越界 | vcallms/vcallmsr 0 | 不適用 |
| PS2Recomp #309/#310、GoW #15 | VU0 巨集 LQI/LQD/SQI/SQD/ILWR/ISWR 欄位與記憶體目標 | 0 | 不適用 |
| PS2Recomp #201 | LQC2 寫入 vf0 | lqc2→vf0 0（ELF 掃描） | 不適用（其餘由 #292 涵蓋） |
| PS2Recomp #323/#348 | 離開函式的 J 直接尾呼叫，遊戲改寫 J/JAL 時看不到 | 未見自我修改程式碼 | 觀察（會增加每次 J 的派送成本） |
| PS2Recomp #286、KZ 0014 | 未解析 JALR 讓整個函式升格為恢復入口 | — | 已有（41） |
| Urban 記憶卡 §4 | 資料區的值等於延遲槽位址時被當函式起點，原函式被截斷 | 函式表中 2 筆落在延遲槽，經檢查皆無害 | 不適用 |
| Urban §47–48 | MSVC 對巨大函式放棄最佳化（C4883），需 `/d2OptimizeHugeFunctions` | 最大 1.8MB 的 FUN_002f4610 單獨編譯無 C4883 | 不適用（已驗證） |

## B. EE runtime／排程器

| 來源 | 內容 | 狀態 |
|---|---|---|
| PS2Recomp #257/#284 | D_ENABLEW 寫入要映到 D_ENABLER | **已套用**（build 205） |
| PS2Recomp #302、#222 | DI 期間不送中斷、不做時間片搶占 | **已套用**（build 205；`DOD_IGNORE_DI=1` 關閉） |
| PS2Recomp #136 | 號誌呼叫成功回傳 sid | 已有 |
| Urban §7、ps2tek | ChangeThreadPriority 回傳舊優先權（CRI ADXM 鎖用來還原） | 已有 |
| Urban（FMV）| Suspend/Resume/(i)WakeupThread 回傳 ID（CRI 停止交握） | 已有（build 204） |
| PS2Recomp #343 | 只有 32 位元寫入後才派送 DMAC 完成中斷；派送在佇列空時仍上鎖建 vector | **已套用**（build 211，原子旗標快速返回） |
| PS2Recomp #347 | HLE 核心記錄放在遊戲的記憶體（SIF 命令封包用遊戲 heap） | **已套用**（SIF 封包移到 0x78000–0x80000 核心區）；guestMalloc 加記錄，實測 DoD 執行中 0 次 |
| PS2Recomp #298 | 中斷堆疊從 RAM 頂端切出，壓到遊戲主堆疊 | 已有（改用 0x80000–0x100000） |
| PS2Recomp #299 | GIF 仲裁器按路徑優先權排序，造成貼圖上傳晚於使用它的繪製 | **已套用**（按提交順序；`DOD_GIF_PRIORITY_SORT=1` 回舊行為） |
| PS2Recomp #303 | VU0 程式碼區與 VU1 資料區未走特殊位址路徑 | 待辦（產生碼共用表頭，排入下一次全量重建） |
| PS2Recomp #294 | VIF1 暫存器讀到的是 CPU 寫入值 | 不適用（ELF 中無直接存取） |
| PS2Recomp #295/#341 | VIF1 i-bit 中斷（INTC 5）與停頓 | 不適用（遊戲只註冊 INTC 11 與 0） |
| Urban §25 | `SetupHeap(addr,-1)` 上限應為主堆疊底而非 0x01F00000 | **已套用**（build 212，`DOD_HEAP_TO_STACK=0` 回舊行為） |
| Urban 錯誤 2、3 | INTC_STAT 寫 1 清除；Timer 3 以 H-BLANK 計數 | 已有 |
| Urban §35 | GS CSR.FIFO 讀值應為「空」 | **已套用**（build 210，`DOD_GS_CSR_FIFO=0` 關閉） |
| Urban §24、PS2Recomp #350 | VIF1 DMA 方向位元（DIR=0：GS→記憶體回讀） | 觀察（DoD 尚未見 VRAM 回讀） |
| 本工程 | 中斷處理在執行緒上執行時被搶占／暫停 | 觀察（跳過影片問題的真因是 IOP，見 C） |

## C. IOP

| 來源 | 內容 | 狀態 |
|---|---|---|
| Urban §34 | CRI_ADXI.IRX 用 CpuSuspendIntr 保護串列；IOP 在臨界區內仍處理 DMA 完成／切換執行緒 → 串流區塊遺失、永久卡住 | **已套用**（build 209；正是跳過影片卡死的真因：1ms 計時器下 0/2 → 4/4；`DOD_IOP_INTR_MASK=0` 關閉） |
| PS2Recomp #289 | IOP LWL/LWR 未接收延遲槽中的載入值（非對齊讀取錯誤） | **已套用** |
| PS2Recomp #315 | IOP heap 釋放後不重用 | **已套用**（first-fit；`DOD_IOP_HEAP_BUMP=1` 回舊行為） |
| PS2Recomp #266 | 執行緒外的 WaitSema 不等待、不取號誌 | **已套用**（`DOD_IOP_SEMA_OUTSIDE_WAIT=0` 關閉） |
| PS2Recomp #290 | IOP 同步呼叫超出指令預算時無聲放棄 | **已套用**（記錄 `[IOP:call-abandoned]`，`DOD_IOP_CALL_BUDGET` 可調） |
| PS2Recomp #338 | RPC 伺服器要在 sceSifRpcLoop 執行緒上跑 | 已有（本工程 handleRpcRequest） |
| GoW #130、KZ findings | 執行緒外的 DelayThread 立刻返回，輪詢型 RPC 伺服器用光預算 | **已套用**（延遲期間讓 IOP 排程器繼續跑） |
| GoW #129 | IOP `sceCdStop` 未實作讓等待它的執行緒卡死 | DoD 未呼叫（新增「缺少 import」記錄後，實測遊戲中 0 筆） |
| 本工程 | 多數 IOP 函式庫缺少 import 時沒有記錄 | **已套用**（`[IOP] missing import`） |
| KZ 0015、Urban §36 | IOP 獨立執行緒 | **已套用、預設開啟**（build 216，`DOD_IOP_THREAD=0` 回同步）— A/B：開啟平均 59.75／59.54、最低 58.9／57.9，關閉 59.53／59.28、最低 57.0／55.9；跳過影片 6/6 |
| Urban §30/§33 | IOP 每指令掃 DMA 表、每指令查 import 表 | 待辦（目前已達 60 幀，留作餘裕） |
| PS2Recomp #308、Urban/GoW/DQ8 | IOP 上的 SPU2 模擬（讓遊戲自己的 LIBSD/音效驅動出聲） | **已套用 #308**（build 221），加上 DoD 需要的 AutoDMA（CRI ADX）、AVOL/BVOL 修正與主機輸出（`RUNTIME_PATCHES.md` 第 85 列）。GoW #95 的 NAX 修正在 #308 已等效（NAX 讀在播放中的區塊內）；#254 是另一套原生 IOP 設計，未套用。殘響未做 |

## D. 繪圖／影片

| 來源 | 內容 | 狀態 |
|---|---|---|
| Urban GS 執行緒（gs_threaded_backend） | PCSX2 GS 在獨立執行緒繪製與 present | **已套用**（build 206，`DOD_GS_THREAD=0` 關閉） |
| GoW #66 | 3D 模型扭曲檢查清單（VU1 週期上限、運算元夾限、vf0、VCLIP、VU1 MAX/MINI） | VU1 為 PCSX2 microVU（kzvu），vf0 已處理，VCLIP 已修 |
| PS2Recomp GS 系列（#148/#188/#230/#231/#246/#280–282/#340 等） | 內建軟體 GS 的修正 | 不適用（畫面由 PCSX2 GS 負責） |
| PS2Recomp libgraph HLE（#185/#186/#311/#331/#333–336） | sceGs* HLE stub 的修正 | 不適用（DoD 執行原生 libgraph） |

## E. 效能（60fps）

| 來源 | 內容 | 狀態 |
|---|---|---|
| PCSX2 對照 | 原版遊戲中為 60fps（狀態列 FPS 60 / VPS 60） | 目標確認 |
| Urban §52、GoW #120 | 每個回跳分支記 32 週期高估迴圈時間，配合 DI 區段延後 VBlank，兩幀擠進同一 field | 觀察（週期 50% 實測只多 2–3 幀；現已 60 幀） |
| PS2Recomp #328、GoW #118 | IOP 以 4096 EE 週期為單位推進（原本每個檢查點都呼叫） | **已套用** |
| 本工程 | `[fps]` 統計用的原子加法在每個檢查點執行（占 EE 執行緒 3.1%） | **已修** |
| KZ 0016（chain 緩衝池） | VIF1 chain 緩衝每幀重新配置、逐 tag 擴張 | **已套用** |
| 本工程 | 每次 VIF1 DIRECT、每次 SPR DMA 都呼叫 `getenv` | **已修** |
| KZ 0018 | 排程器每次迭代上鎖兩次、建 deque、讀主機時鐘 | **已套用** |
| 結果 | build 215：遊戲中 **59.3–60.0 幀**（原 30），跳過影片測試全過 | 達成 |
| KZ 0016（工作執行緒）、GoW #48–49 | VIF1/VU1 移到工作執行緒 | 待辦（餘裕；目前 EE 執行緒仍有約 20% 在 VIF1/VU1） |
| KZ 0019 | 延遲 EE 計時器、VU0 呼叫成本 | 待辦（餘裕）。10/20 實測（第 117 列）：`PS2Memory::advanceEeTimers` 佔 EE 執行緒樣本 8.8%（約 EE 實際工作的 17%，比全部遊戲產生碼還多），是 runtime 最大單項 |
| SotC、KZ 研究 | 編譯參數 `/GS-`、`/Gy /Gw`、`/OPT:REF,ICF`、runtime LTCG | 待評估 |
