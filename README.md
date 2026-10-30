# MetaSoftware Valorant (1-PC Standalone Edition)

本專案已完成配置與改造，已由原本的 **2 PC (NDI + Makcu)** 架構成功改造成 **1 PC 單機直接運行** 版本。

---

## 改造重點 (1-PC 變更說明)

1. **螢幕擷取 (Screen Capture)**
   - **原本**：依賴 NDI SDK 跨網路從 PC 1 串流至 PC 2，需要第二台電腦與區域網路。
   - **改造後**：直接在單機（1 PC）上利用 Windows GDI 高效擷取主螢幕正中央的 256x256 區域，延遲 <1ms，固定 144 FPS 電競刷新率，免裝 NDI SDK 與額外軟體。

2. **滑鼠輸入控制 (Makcu UDP / 本機控制)**
   - **原本**：必須連線實體 Makcu USB 盒子（經本機 COM 埠通訊），未連接時直接閃退退出。
   - **改造後**：
     - 支援透過 **UDP** 連線樹莓派（Raspberry Pi）或網路橋接器：
       - `PI_IP` / `ip`：預設 `192.168.137.19`（支援 mDNS 如 `pi.local`）
       - `PI_PORT` / `port`：預設 `5005`
     - 若未設定 UDP 或網路連線失敗，自動無縫回退至本機 Windows 滑鼠 API 模擬。
     - 平滑移動演算法支援 Bezier 曲線減速。

3. **按鍵觸發與監聽 (Key Binding)**
   - **原本**：透過 Makcu 硬體監聽滑鼠點擊。
   - **改造後**：直接以 Windows API 監聽本機滑鼠按鍵與鍵盤熱鍵，支援：
     - `LEFT` (滑鼠左鍵)
     - `RIGHT` (滑鼠右鍵)
     - `SIDE1` (滑鼠側鍵 1 - XBUTTON1)
     - `SIDE2` (滑鼠側鍵 2 - XBUTTON2)
     - `ALT` (Alt 鍵)
     - `SHIFT` (Shift 鍵)

4. **認證繞過 (Authentication Bypass)**
   - 移除已失效的 `metasoftware.me` 遠端驗證與 `libcurl` 相依，改為本機離線驗證模式（直接通過）。

5. **相依庫自我包含**
   - 移除對外部 `vcpkg`、`Processing.NDI.Lib`、`makcu-cpp.lib` 的強相依性。
   - 內建 `nlohmann/json` 標頭檔於 `thirdparty/`。
   - OpenCV 已自動配置指向系統 `C:\opencv`。

---

## 執行與建置說明

### 1. 直接執行
已為您編譯好 64 位元可執行檔：
- Release 路徑：`x64\Release\tts.exe`
- 目錄下已附帶 `opencv_world4100.dll` 及 `config.json`。
- 直接以**系統管理員身分**執行 `tts.exe` 即可。

### 2. 使用 MSBuild 重新編譯
在命令列中執行：
```powershell
# 編譯 Release 版本
msbuild tts.sln /p:Configuration=Release /p:Platform=x64

# 或編譯 Debug 版本
msbuild tts.sln /p:Configuration=Debug /p:Platform=x64
```

---

## 操作熱鍵
- **選單開關**：預設為鍵盤 `INSERT` 鍵（可在 UI 或 `config.json` 中自訂）。
- **緊急退出**：預設為鍵盤 `END` 鍵。
- **瞄準觸發**：可於 UI「Aim Assist」選單中設定為右鍵（RIGHT）、左鍵（LEFT）或側鍵（SIDE1/SIDE2）。
