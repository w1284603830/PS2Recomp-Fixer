# 通用基底維護審查 — 2026-11-05

公開基線：main `cc3b5e17f43254db8a06c829172b14b7014e9d52`；源樹
`c6f4eef9dbb19a549ef269daf9775cf4b0a882ae`。本地父提交 `417eeaa` 具有相同源樹。
已核對近期移植修改的實際源碼，提取共用機制；沒有用私有工作樹覆蓋公開 main。
本紀錄不包含私有交接內容、機器路徑、素材、擷取資料或未公開測量數字。

## 修改與隔離

| 項目 | 處理 |
|---|---|
| 輸入更新 | 選單處理與 host pad 發布移到 idle wait 前；非同步 GS 保留 1 ms 阻塞等待。新增最多八個獨立短按確認讀者，保留原 SIO2/HLE 讀取契約。 |
| 原生 pad 邊界 | 不匯入鬼武者 3 的函式地址、輸出偏移、裝置假設、壓感重建或劇情 hook；由各遊戲驗證自己的 ABI。 |
| 提交時點 | 預設 VBlankStart；各移植可明確選 End，逐 field 鎖定決策。電影狀態在 EE 執行緒由 adapter 提供；run 重啟保留配置，重新載入 ELF 清除。 |
| 紋理與身份 | 移除共用固定 DDS 清單及虛構遊戲標題／序列號；改由素材包提供排除清單及標題。修正根目錄參數可能無限迴圈。 |
| 畫面比例、GIF、CG cadence | 移除依特定 framebuffer、深度或固定 bank 自動推斷的邏輯；歷史片段移至遊戲範例，沒有共享呼叫點。 |
| 高清素材 | 靜態頁面需明確選擇格式及目錄；HCG1 保留明確尺寸協定，移除遊戲 bank 白名單並核對整個 RGB 區域；修正 crop 範圍加法溢位。取消自動 7×／NativeScaling 覆寫，改為可選配置。 |
| 反饋與 clamp | 提供預設關閉的 `feedback_copy_hpo_guard`、`clamp_footprint`，各移植需獨立驗證；不加入特定 P4 粒子圖集尺寸、tile、depth 或 billboard 規則。 |
| 診斷 | 修正初始化前 `PS2_` trace 選項優先順序、非法時間窗拒絕，以及 RGB_A PNG 分離 Alpha、pitch 驗證與寫入失敗回報；A/B 改為多幀兩半且強制關閉範圍止於 capture 結束；CG trace 僅觀察。 |
| 測試輸入 | 自動導航需明確按鈕清單，沒有 START/CIRCLE／40 秒預設；frame clock 需提供地址與頻率，嚴格拒絕溢位地址，會話重置清除基準。 |
| VU 與工具 | kzvu 預設正常 clamp、IbitHack 關閉；Killzone 測試自行選參數。11 個固定 ELF 範圍／計數器的腳本移入遊戲範例。 |
| 既有精度 | 保留先前 IPU／MPEG／font profiles、SPU2 FIR 與輸出重採樣；不以本次輸入或渲染維護替換既有混響邏輯。 |

完整使用方式、參數與遷移見 [NEUTRAL_BASE_MIGRATION.md](NEUTRAL_BASE_MIGRATION.md)。UI 預解碼仍保留
「最初針對高清紋理移植的優化」說明。粒子 profile 仍預設關閉，所有遊戲／版本獨立驗證，HPO 相同不能繼承。

## 審查範圍

檢查 repository 目錄、工具及自訂 runtime／GS／host 接點，追蹤身份、固定 RAM／buffer 規則、預設選擇與
session reset 的呼叫路徑，並逐項審查本次 diff。保留 PS2 硬體位址、已識別 SDK ABI 欄位、多遊戲名稱資料庫、
上游經身份選擇的相容性修正及明確隔離的參考 profiles。歷史 `dod` 名稱本身不代表仍在啟用某遊戲規則。
未逐行重新證明所有上游第三方 CPU、GS、編碼器及 JIT 算法；本次審查不宣稱整庫無缺陷或已通過所有遊戲。

固定尺寸的 HD 格式與已支援 SDK ABI 仍屬明確 adapter，未知格式／ABI 需要新增實作與證據，不能改名後宣稱通用。
遊戲範例仍保留自己的地址和規則，沒有放入共用預設。舊 DLL 忽略新參數，發佈必須核對實際 host／DLL 配對。

## 分項驗證

| 類別 | 結果與界線 |
|---|---|
| 靜態審查 | 已核對上述共用／遊戲邊界、預設關閉、失敗回滾、重置、兩份 C API 與上游補丁一致性；靜態檢查不是畫面驗收。 |
| 合成模擬 | 使用記錄式 host／GS 服務，涵蓋短按、獨立確認、選單、來源切換、連續會話、電影切換、搬移 buffer、稀疏採樣碰撞及 Alpha；不包含真人操作或 GPU 畫面。 |
| 雲端實際編譯 | GCC C++20 / UBSan 編譯生產 pad translation unit、policy／HD headers、擷取／身份函式、Bridge／renderer 函式；完整 scheduler 與非 Windows backend 的 Linux 語法檢查通過；Windows 專屬 backend 分支未由此編譯。 |
| 自動化回歸 | PASS：全庫 118 項（新增通用／素材邊界 25 項、粒子專項增至 15 項），35.6 秒；補丁反向／正向套用後逐檔位元組相同。ASan 僅關閉受執行環境限制的 leak scan。 |
| Windows 完整編譯 | 使用者明確要求不在其電腦編譯，因此沒有啟動。GitHub 雲端首次完整編譯抓到移除遊戲標頭後遺漏的共用 field 計數器宣告，已補獨立宣告。更新版的實際完整編譯結果見 [PR #8 Checks](https://github.com/w1284603830/PS2Recomp-Fixer/pull/8/checks)，不能由 headless 測試推定。 |
| 真實遊戲驗證 | NOT_RUN：未執行商業遊戲、真實 GIF replay、畫面／聽感／物理輸入延遲驗收；沒有部署遊戲 EXE／DLL。 |

## 各移植的驗收與修正流程

1. 固定 main／候選 commit、ELF、profile、資源、EXE／DLL 與被消費的輸入；分開原版和增強模式。
2. 先執行無商業資源的測試與完整平台編譯，再建立冷／熱載入、輸入／選單／裝置、CG 全生命週期、粒子／HUD／文字、
   alpha／depth／RT 及持續音畫同步矩陣。每個遊戲和版本有自己的配置與結果。
3. 發現問題時沿 EE → VIF／VU → GIF → GS 找第一個不同狀態；依原生／參考狀態判定硬體、SDK ABI、遊戲 hook 或素材層。
   先做最小修復，補能重現該差異的測試，再重跑直接相關案例與完整發佈矩陣。
4. A/B 每半區檢查有效 draws、profile／套用理由、buffer／ROI，保留 Alpha；空 field 或不同動畫狀態需凍結 replay。
   不把 code check、無崩潰、零 missing、強制房間巡覽或單一顯卡結果當作完整通關與效果驗收。

詳見 [完整回歸指南](REGRESSION_TESTING.zh-TW.md)。
