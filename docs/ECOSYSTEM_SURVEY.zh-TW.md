# PS2 靜態重編譯／原生移植生態調查（2026-11-12）

[English](ECOSYSTEM_SURVEY.md)

目的：找出可以讓本基底更通用、更易用的技術，並把「繼承來的程式碼」和「各專案自己的貢獻」分清楚，避免重複統計同一份程式碼。
本文只描述公開倉庫的原始碼與文件；**README 裡的相容性與進度數字是各專案的自述，未經本專案驗證**。

## 1. 方法

- 28 個倉庫（加上第 19 項的子模組 BozoEngine）以 `--depth 1 --filter=blob:none` 複製，只讀目錄樹，必要時才讀個別檔案；未建置、未執行任何下載的程式碼。
- 內容比對用 git blob id：**id 相同即檔案內容完全相同**。基準有四個：PS2Recomp `eb8688b`（本基底的上游起點，SHA-256 鎖定的 zip）、
  PS2Recomp 上游 HEAD、本倉庫 `main`（`5e907ef`）、PCSX2 HEAD。
- 「行數」只計程式碼與腳本（`.cpp/.hpp/.h/.c/.py/.wgsl/.cmake/.sh/.ps1/.cs`），以逐行比對計算保留／修改／新增。

各倉庫調查時的 HEAD 列於文末附錄。

## 2. 程式碼譜系（以內容判定）

| 倉庫 | 檔案數 | 與 PS2Recomp `eb8688b` 相同 | 與本基底 `main` 相同 | 與 PCSX2 相同 | 判定 |
|---|---:|---:|---:|---:|---|
| ran-j/PS2Recomp（上游 HEAD `e5636981a1`） | 298 | 292 | 223 | 0 | `eb8688b` 之後只改 6 個檔（GIF FLG=3，#384；本次已移植） |
| gothamor/gta-vcs-recomp | 594 | 223 | 189 | 0 | 內含 PS2Recomp 副本 + 自己的 D3D11／Vulkan 層 |
| YYOzcan/Tobiichi-Port | 581 | 214 | 187 | 6 | 內含 PS2Recomp 副本（`PS2Recomp/` 394 檔） |
| thiagordsfx/urban-reign-recomp | 862 | 140 | **465** | 367 | 本基底 runtime 與 PCSX2 GS 橋接的來源譜系 |
| z3xox/BT3-Recomp | 665 | 76 | 69 | 61 | PS2Recomp 分支 + IRX 原生碼（見第 4 節） |
| drakolordx7/KillzoneRecomp | 346 | 0 | 9 | 0 | kzvu（PCSX2 microVU 封裝）的來源；已封存 |
| KIexster/god-of-war-recomp | 321 | 1 | 1 | 0 | 179 個補丁疊在 PS2Recomp + Waffle565 runtime 上；與 Tobiichi 共用 30 檔 |
| Waffle565/God-Of-War-Native-Port | 1580 | 1 | 1 | 23 | 與 Tobiichi 共用 42 檔（同一 GoW 譜系） |
| oric212/BT2-Recomp | 572 | 0 | 0 | 1 | 以 PS2Recomp（鎖定 `c5a9d02`）當工具，不內含其原始碼；自己的原生層 |
| jcsix694/def-jam-vendetta-recomp | 155 | 0 | 0 | 0 | PS2Recomp 為子模組；GS 用 paraLLEl-GS |
| moesuito/gt4-staticrecomp | 1496 | 0 | 1 | 15 | 獨立重編譯器與 runtime；與 BT3 共用 15 檔（共同的第三方檔） |
| snuri00/ps2-web-recomp | 116 | 0 | 0 | 0 | 獨立譜系（自己的 `eerecomp`） |
| exoteric33/ps2-native-recomp | 232 | 0 | 0 | 0 | **ps2-web-recomp 的分支**（見第 3 節） |
| danielgomesvieira2000/ps2-port-engine、burnout3-pc | 285 / 107 | 0 | 0 | 0 | 獨立：反編譯＋逐函式驗證路線 |
| TheBozos/ChampionsOfNorrath_Recomp | 32 | 0 | 0 | 0 | 遊戲層；引擎在子模組 BozoEngine（見第 8 節） |

統計原則：一份檔案只算在最早的來源。上表 223 個「與上游相同」的 gta-vcs 檔案是 PS2Recomp 的貢獻，不是 gta-vcs 的；
urban-reign 的 465 個「與本基底相同」的檔案，大部分是本基底從 urban-reign 譜系（含 PCSX2 GS）繼承來的。

## 3. ps2-native-recomp 與 ps2-web-recomp

**關係：** ps2-native-recomp 的 README 寫明「Built on ps2-web-recomp by Samir Nuri (MIT)」。檔案層級的比對支持這點：

| 項目 | 數量 |
|---|---:|
| web 的檔案 | 116 |
| native 的檔案 | 232 |
| 路徑與內容都相同 | 17（`kernel.hpp`、`vu_jit.hpp`、`ipu/*`、`host/profile.*` 等） |
| 路徑相同、內容修改 | 87（runtime 56、recomp 12、scripts 10…） |
| 只在 web（native 捨棄） | 12（GT3 測試、示範影片、效能腳本） |
| 只在 native | 128（tests 45、scripts 29、runtime 26、host 11、app 3…） |

| 程式碼行數 | 行數 |
|---|---:|
| web 的全部程式碼 | 27,671 |
| native 保留的 web 行 | 23,790（86%） |
| native 在共用檔案中新增／修改 | 17,515 |
| native 刪除的 web 行 | 3,195 |
| native 新檔案 | 34,842（runtime 10,724、tests 8,848、scripts 6,651、host 3,484、app 3,308、recomp 1,761） |

**避免重複統計：** web 的 2.77 萬行只算一次（算在 web）；native 的獨立貢獻約 **5.2 萬行**（17,515 + 34,842）。
修改最多的共用檔案是 `runtime/src/gs/gpu/gpu.cpp`、`host/window.cpp`、`runtime/src/iop/sif.cpp`、`runtime/include/ps2/vu_ops.hpp`、
`runtime/src/system.cpp`、`runtime/src/iop/cpu.cpp`、`runtime/src/iop/sio2.cpp`、`gs.wgsl`、`vu_micro.cpp`。

**共同架構（web 首創、native 繼承）：** 自己的 EE 重編譯器 `eerecomp`（沿控制流找函式，以 16 KiB 客體程式碼為輸出單位）、
HLE 的 EE kernel、VU1 含 MAC／狀態／clip 旗標管線延遲、GS 的 4 MB VRAM 以原生 swizzle 佈局放在 GPU 上並以 compute shader 繪製
（WGSL）、SPU2 重寫、IPU、IOP 以解譯器執行光碟上的原版 IRX。

**native 的獨立貢獻：** 原生 Windows／Linux 主機（視窗、鍵位、快速選單、當機處理）、GPU GS 後端大量修改、SIF／SIO2／IOP CPU 修正、
VU 運算與「錄製後預先編譯」的 VU0／VU1 微程式（`docs/vu0-native.md`：遊玩時記錄未知入口，之後編進產生碼；沒見過的程式先解譯、
VU1 在背景編譯後切換）、多遊戲自動測試（`scripts/autotests.py`）、會自己下載建置工具的 App。它的原則「遊戲跑不動就修工具，
不寫遊戲專屬碼」與本基底的 [AGENTS.md](../AGENTS.md) 契約一致。

## 4. IOP：HLE 服務、解譯 IRX、原生重編譯 IRX 是三件不同的事

| 路線 | 代表 | 光碟上的 IRX | IOP kernel／函式庫 | 主要風險 |
|---|---|---|---|---|
| **HLE 服務替代** | BT2-Recomp（`runtime/services/`：`sif_rpc`、`sound_service`、`module_service`、`cri_audio`、`iop_heap`…） | **不執行**；EE 的 SIF RPC 由原生 C++ 服務直接回答 | 不需要 | 每個遊戲的 RPC 協定都要重寫；未知協定只能報錯 |
| **解譯執行原版 IRX** | 本基底 `ps2xIOP`、ps2-native／web-recomp、Play!（HLE BIOS） | 載入 IOP RAM，以 R3000 解譯器執行 | HLE（threadman、intrman、sifcmd、thmsgbx…）；SPU2、SIF DMA 照硬體 | 解譯效能；需要的 kernel 服務要照 ps2sdk 語義補齊 |
| **原生重編譯 IRX** | BT3-Recomp（`ps2xRuntime/src/iop_native/<模組>/`，19 個模組：cdvdman、libsd、mcman、sio2man…） | IRX ELF 解析（段、重定位、匯入／匯出），函式以模組相對位址 `registerIopFunction` 登錄 | HLE | 用 EE（R5900）翻譯器處理 MIPS-I：R3000 載入延遲槽等語義沒有建模；產生碼來自光碟模組 |

補充：

- BT2 另有一個**隔離的 R3000 靜態翻譯原型**（`docs/plans/iop/IOP_NATIVE_EXECUTION_PLAN.md`，2026-10-04 檢查點）：獨立 32 位元 context、
  自有 RAM、延遲分支／延遲載入、明確的 import 派發；跑到 SIO2MAN entry 寫入 SIO2 重設暫存器為止，並明確記錄「不偽造模組成功」。
  它的文件也指出「沿用 EE ABI 不能證明 IOP 執行正確」，這正是 BT3 路線的缺口。
- BT3 把 IRX 的重編譯產物提交在公開倉庫裡。那是光碟上 Sony 函式庫模組的衍生碼，本基底不收、不引用。
- **本基底的選擇：** 預設維持解譯執行（不需要遊戲專屬碼，最通用）；HLE 只用在有公開 ABI 依據並有測試的服務
  （本次新增的 `thmsgbx` 信箱即依 ps2sdk `thmsgbx.c`／`kerr.h`）；原生 IRX 只適合作為之後的**選用加速器**，前提是有正確的
  MIPS-I 語義（延遲載入）與「不能證明就不回報成功」的驗證。

## 5. EE 重編譯與函式發現

| 專案 | 做法 |
|---|---|
| PS2Recomp／本基底 | ELF 符號 + `ps2_analyzer`（內嵌 SCE 符號資料庫）產生 TOML；`entry_points` 補入口；跳轉表；本次新增 `native` 位址白名單，避免同名 SDK 綁定誤傷遊戲自己的函式 |
| ps2-web／native-recomp | 沿控制流找函式（`recomp/analysis.cpp`），以 16 KiB 單位輸出 |
| gt4-staticrecomp | 自己的重編譯器；遊戲程式需要先解包（README 記載的一個建置步驟） |
| BT3 | PS2Recomp 分支 + Ghidra 匯出腳本（`ps2xRecomp/tools/ghidra/`）+ 遊戲專屬 overlay 產生器 |
| BT2 | PS2Recomp（鎖定 `c5a9d02`）；`DBZP.BIN` overlay 的 494 個函式單獨編譯 |
| ChampionsOfNorrath（BozoEngine） | 自己的 `bozo-recomp`，用 ELF 自帶的符號表 |
| ps2-port-engine | 反編譯成 C++，逐函式和原機器碼比對驗證 |

**動態載入的程式碼（overlay）** 各家都是遊戲專屬做法（BT3 `gen_overlay.py`、BT2 單獨編譯 overlay 檔）。本基底下游有一個移植
以「載入位址處的標頭簽章判斷目前常駐哪個映像」派發，是可以通用化的機制；已實作為選用的通用登錄（[IMPROVEMENTS.zh-TW.md](../IMPROVEMENTS.zh-TW.md) 第十四節）。

## 6. VU0／VU1 微程式

| 專案 | 做法 |
|---|---|
| 本基底 | VU1 = PCSX2 microVU（kzvu，JIT）；後備解譯器；本次新增 VU0／VU1 夾制模式分別設定 |
| KillzoneRecomp | kzvu 的來源（PCSX2 microVU 打包成函式庫） |
| gt4-staticrecomp | 所有 VU0／VU1 微程式預先編成原生碼，正常路徑沒有解譯器 |
| ps2-native-recomp | 錄製遊戲實際用到的微程式後預先編譯；未知程式先解譯，VU1 背景編譯後切換 |
| ps2-web-recomp | VU1 含管線延遲的解譯器 + VU JIT |

## 7. GS 後端

| 專案 | 後端 |
|---|---|
| 本基底、urban-reign 譜系 | PCSX2 硬體 GS 打包成 DLL（D3D11），C API |
| gt4-staticrecomp | 自研 CoreGS 2.0（Vulkan），GSdx 放在獨立行程當後備 |
| def-jam-vendetta、BT3（實驗） | paraLLEl-GS（Vulkan compute） |
| BT3 | OpenGL 渲染器（預設） |
| gta-vcs | Windows D3D11、Linux Vulkan |
| burnout3-pc／ps2-port-engine | Vulkan（另有 D3D11） |
| ps2-web／native-recomp | WebGPU compute（WGSL），GS VRAM 以原生 swizzle 佈局放在 GPU |

DMA／VIF／GIF：本次從上游移植 GIF FLG=3（IMAGE2）在所有 GIFtag 解析處等同 IMAGE（上游 #384，PCSX2 `Gif_Unit.h` 相同處理）。

## 8. 第 19 項 ChampionsOfNorrath_Recomp 的驗證

- 倉庫存在且可存取（HEAD `fb55499314`，2026-11-11），README 狀態徽章為 **bring-up（啟動中）**，目標寫的是「完整可玩」，尚未宣稱完成。
- 本身只有 32 個檔案，約 17 萬位元組的遊戲專屬碼（`runtime/con_*`：角色、世界、貼圖、overlay、VU0、燈光）與工具；
  **引擎在子模組 `TheBozos/BozoEngine`**（釘選 `a36a4d2`，2026-11-10，291 個檔案，354 次提交，含 `recompiler/src`、`tools/native_irx`、runtime 與測試）。
- 結論：真實存在、仍在早期；技術主體在不在調查清單中的 BozoEngine。**只列為待觀察，不當作確定的技術資產。**

## 9. 本基底採用的與後續方向

**本次採用（見 [IMPROVEMENTS.zh-TW.md](../IMPROVEMENTS.zh-TW.md) 第十三節）：** `native` 位址白名單、延遲槽恢復修正、巢狀 `entry_*`
派發、`thmsgbx` 信箱與 `QueryIntrContext`、MCMAN 記憶卡描述符、libpad 震動 HLE、VU 夾制設定、GIF FLG=3、選用的 F2／F3 工具
（預設屏蔽）、`tools/new_port.py` 新移植腳手架。

**後續候選（各自需要獨立的設計、測試與驗收）：**

1. ~~通用 overlay 登錄~~：已實作（[IMPROVEMENTS.zh-TW.md](../IMPROVEMENTS.zh-TW.md) 第十四節、[GAME_PORTING.md](GAME_PORTING.md) 第 6 節）。
2. VU 微程式錄製後預先編譯（參考 ps2-native-recomp、gt4-staticrecomp），kzvu JIT 保留為後備。
3. 選用的原生 IRX 加速：正確的 MIPS-I 延遲載入語義，與解譯結果逐模組比對後才啟用。
4. 多遊戲自動化回歸（參考 ps2-native-recomp 的 autotests 與 BT2 對照 PCSX2 擷取的驗證閘）。
5. Vulkan GS 後端評估（paraLLEl-GS、CoreGS 的思路），讓 Linux 不依賴 D3D11。

## 10. D 組：反編譯與重寫路線

rac1-native-game、OpenGOAL（jak-project）、GT3-revisited、ProjectCane（Sly 1）、rac-rs（Rust 重寫）是原生重寫／反編譯路線；
recvx-decomp、kh1、mh1j、Tenkaichi3Decomp 是 matching decomp。它們不共用本基底的程式碼，對本基底的價值在於：
matching decomp 的符號與函式邊界可以餵給重編譯器的函式表與 `native`／`stubs` 設定；渲染與音訊層的設計可作參考。
Play! 是 HLE BIOS 的模擬器，其 IOP 函式庫 HLE 可作為解譯路線的對照。PCSX2 與 ps2sdk 是硬體與 ABI 的參考來源。

## 附錄：調查時的 HEAD

| 倉庫 | HEAD | 日期 |
|---|---|---|
| exoteric33/ps2-native-recomp | `0cc2658bdc` | 2026-11-11 |
| snuri00/ps2-web-recomp | `76554ba441` | 2026-11-08 |
| ran-j/PS2Recomp | `e5636981a1` | 2026-10-22 |
| danielgomesvieira2000/ps2-port-engine | `606a451c26` | 2026-10-31 |
| PCSX2/pcsx2 | `c8081bf5d6` | 2026-11-10 |
| z3xox/BT3-Recomp | `e22a34beb5` | 2026-10-01 |
| z3xox/Tenkaichi3Decomp | `48d66c1f66` | 2026-11-03 |
| oric212/BT2-Recomp | `d42f8afdce` | 2026-11-11 |
| moesuito/gt4-staticrecomp | `b0008e8a93` | 2026-11-11 |
| KIexster/god-of-war-recomp | `d1d11ec545` | 2026-11-06 |
| Waffle565/God-Of-War-Native-Port | `d795e3409b` | 2026-11-08 |
| YYOzcan/Tobiichi-Port | `471c83180a` | 2026-10-16 |
| thiagordsfx/urban-reign-recomp | `80901198b6` | 2026-11-04 |
| jcsix694/def-jam-vendetta-recomp | `f5a7574d28` | 2026-11-06 |
| drakolordx7/KillzoneRecomp | `a81c3309c8` | 2026-11-09 |
| danielgomesvieira2000/burnout3-pc | `69e4cfadb1` | 2026-10-31 |
| gothamor/gta-vcs-recomp | `8665e795a3` | 2026-11-10 |
| TheBozos/ChampionsOfNorrath_Recomp | `fb55499314` | 2026-11-11 |
| TheBozos/BozoEngine（子模組） | `a36a4d23f2` | 2026-11-10 |
| RAC-Native/rac1-native-game | `dd85b16087` | 2026-11-11 |
| open-goal/jak-project | `da5d348676` | 2026-11-10 |
| matt-pulsipher/GT3-revisited | `19c036b243` | 2026-11-10 |
| AshfordFamily/recvx-decomp | `23bc6cb06c` | 2026-11-06 |
| theclub654/ProjectCane | `f77825d6bc` | 2026-10-22 |
| AsobiCodes/rac-rs | `4060c1aa77` | 2026-11-11 |
| ethteck/kh1 | `0df3b6586d` | 2026-08-18 |
| 2Tie/mh1j | `3bc790783e` | 2026-11-08 |
| ps2dev/ps2sdk | `0c36f32d9a` | 2026-11-11 |
| jpd002/Play- | `1f27232772` | 2026-10-24 |
