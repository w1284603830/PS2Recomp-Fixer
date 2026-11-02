# 最後來源審查與修正：2026-11-03

審查基準：公開 `main` 的 `b2f3f6d82e57eb46d22a6fd11d19820ac74571a3`，檔案樹
`e9552f39787c65458bce025a6458d6ed73505857`。本輪檢查前次地址配置改動、正式 runtime 與測試替身的差異、
MPEG／字型記憶體邊界、配置與掛鉤生命週期，以及建置測試是否真的阻止回歸。以下修正在本輪分支中，
需以合併狀態判斷是否已進入主分支；這份來源審查不等於完整遊戲或 Windows 發佈驗收。

## 確認並修正的問題

| 等級 | 問題與觸發條件 | 影響 | 本輪修正與證據 |
|---|---|---|---|
| P1 | HD 安裝以非空 `lookupFunction` 判斷入口存在；正式 runtime 在缺失時回傳非空錯誤處理函式 | 缺少入口仍可能安裝部分／錯誤掛鉤 | 改用 `hasFunction`；測試替身改成正式的非空錯誤函式語義，原缺失入口案例先重現失敗再修復；替換中途失敗會還原已替換入口 |
| P1 | 已安裝掛鉤時，null／其他 runtime 的重設會清掉原始函式記錄；擁有者銷毀亦未還原 | 殘留掛鉤無法回到原生路徑，影響後續載入 | 重設必須由擁有者提出，否則回傳 false 並保留狀態；`loadELF` 在清其他配置前拒絕；擁有者析構時還原掛鉤；加入重設／釋放／重複安裝案例 |
| P1 | MPEG 的動態 object／work／reference 或影像地址接近 RAM 尾端、失效或加法繞回 | 可能越界讀寫、把輸出寫入錯誤 RAM，或在零 work 指標時寫入 kernel 區 | 檢查完整 object／ABI work 範圍、字組／影像長度、RGBA 來源和 reference 算術；輸出目的地不合法時保留待取 frame；零 work 不再作為 RAM 指標 |
| P1 | Legacy MPEG Create 在無 runtime／配置失敗時仍建立假緩衝區或回報成功；字型配置失敗仍發布部分字表 | 遊戲取得不完整物件，可能在稍後崩潰 | MPEG 在配置前失敗且不寫物件；字型先核對完整來源與所有配置，失敗時釋放本輪已取得的區塊，再回報錯誤 |
| P1 | 字型讀取未驗證完整 header／glyph／kerning；關閉時也把雙位元組字型的原始尾端資料當成 kerning 指標 | 越界讀取、錯誤釋放 guest 配置 | 檢查來源、配置和關閉指標的完整範圍；單／雙位元組字型依實際擁有的區塊關閉；測試有效載入→關閉及損壞／不足配置 |
| P1 | `build_all.ps1` 只印出主測試 exit code，沒有以新失敗使建置失敗 | 主測試新增回歸仍可能顯示建置完成 | MiniTest 輸出結構化逐項結果／摘要；依 suite、名稱和狀態核對歷史例外，驗證總數與原生 exit code；新失敗、崩潰、不完整／重複／零結果一律失敗 |
| P2 | IOP／GS cache 測試列表或執行檔缺失，可能執行零項或沿用先前 exit code | 測試未執行卻被當成成功 | 依已配置的 IOP target 檢查所有執行檔，逐個核對 GS cache 執行檔並拒絕零個案例 |

主要實作位於 [MPEG.cpp](../third_party/src/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/MPEG.cpp)、
[Font.cpp](../third_party/src/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/Font.cpp)、
[ps2_runtime.cpp](../third_party/src/PS2Recomp/ps2xRuntime/src/lib/ps2_runtime.cpp)、
[MiniTest.h](../third_party/src/PS2Recomp/ps2xTest/include/MiniTest.h)、
[build_all.ps1](../build_all.ps1) 與 [測試結果檢查器](../tools/check_test_results.py)。

## 已完成的驗證

```sh
ASAN_OPTIONS=detect_leaks=0 python -m unittest discover -s tests -v
```

完整套件 **70／70 通過**：MPEG 18、字型 9、IPU／通用 runner 11、建置結果判讀 10，以及既有的音訊、
cheat、生成碼同步與補丁一致性檢查。MPEG／字型編譯完整的 production translation unit，啟用
AddressSanitizer 和 UndefinedBehaviorSanitizer；建置判讀測試編譯 production MiniTest，再把真實輸出送入
正式 Python 檢查器，涵蓋相同失敗數但名稱不同、錯誤 suite、例外、截斷／舊版／零結果、摘要與 exit 不符。

此執行環境的 LeakSanitizer 無法讀取受追蹤程序的 `/proc` thread 資料，故只關閉 leak scanning；地址與
未定義行為檢查仍啟用。這不是「無記憶體洩漏」的證明。上游累積補丁已同步，反向套用檢查通過；
修改文件的本地連結與空白檢查亦需通過後才提交。

## 尚未取得的驗收與既有界線

- 本輪沒有 Windows／MSVC 全量建置，也沒有實際執行 PowerShell 的完整建置流程。Python 判讀器、正式
  MiniTest 與 production HLE 單元已在主機測試；PowerShell／Windows 整合仍需驗證。
- 沒有新增遊戲通關、實際 GS DLL／GPU 畫面、完整 FFmpeg／HD 影片播放、字型光柵化或人工對聽的驗收；
  也未部署任何使用者的 EXE／DLL、素材或存檔。依 [完整回歸指南](REGRESSION_TESTING.zh-TW.md) 完成移植驗收。
- [歷史例外](../tools/known_test_failures.json) 來自公開建置文件；只限定已知 suite／名稱／狀態，不能證明
  它們的新一次失敗原因相同。Windows 重跑後仍須檢視診斷，並移除已不再失敗的例外。
- 粒子邊緣修正仍有既有 HPO5 與 HUD／深度假設；本輪沒有實作新的獨立 renderer profile gate。
  [Issue #3](https://github.com/w1284603830/PS2Recomp-Fixer/issues/3) 持續追蹤此項。每個遊戲／修訂版，包含
  新鬼與鬼武者 3，都需要自己的適配與畫面證據，不能因本輪測試通過就推廣為通用效果。
- MPEG 的 host 狀態仍以單一 active runtime/session 為契約；本輪增加擁有者防護，不宣稱支援多遊戲同時執行。
