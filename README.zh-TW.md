# ShadePilot 🎮🎨

[English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md)

[![Release ShadePilot Addon](https://github.com/DuolaD/RE_MCP/actions/workflows/release.yml/badge.svg)](https://github.com/DuolaD/RE_MCP/actions/workflows/release.yml)
[![ReShade API v20](https://img.shields.io/badge/ReShade_API-v20-brightgreen.svg)](https://reshade.me)
[![MCP Protocol](https://img.shields.io/badge/MCP-2024--11--05-blue.svg)](https://modelcontextprotocol.io)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

ShadePilot 是一個執行於 ReShade 內部的 Model Context Protocol (MCP) 附加元件（Add-on）。它在遊戲程序內啟動本機 HTTP/SSE 服務，允許外部 AI 工具（如 Claude Desktop、Cursor、Antigravity 等）直接與 ReShade 渲染管線進行互動：擷取效果處理前後的畫面、調整著色器參數、切換並儲存預設檔、管理外掛及全域設定，並讀取即時執行紀錄。

---

## 核心功能

- **畫面擷取**：在著色器渲染前後（`reshade_begin_effects` 與 `reshade_finish_effects`）擷取畫面，回傳 Base64 編碼的 JPEG 影像。支援不可分割的原子擷取（在同一個渲染幀內同時取得未處理畫面與後製處理後的畫面），以及包含遊戲內 ReShade 選單的 Overlay 畫面。
- **著色器管理**：查詢已載入的 `.fx` 檔案與編譯狀態；列舉所有 Techniques，支援啟閉特定效果與動態調整執行順序。
- **參數調整**：讀取與修改 Uniform 變數（滑桿、顏色、布林值、向量等），對齊 ReShade 主頁（Home）檢視，自動過濾唯讀系統變數；修改後預設自動寫入目前使用的預設檔。
- **前置處理器巨集定義**：查詢與修改著色器巨集定義（如取樣精度、高品質開關等），修改後自動觸發 ReShade 於下一幀熱重載並重新編譯。
- **預設檔存取**：隨時將目前的著色器參數儲存至 `.ini` 預設檔，或切換載入其他預設檔案。
- **附加元件與全域設定**：掃描已安裝的 ReShade 附加元件並設定啟用狀態；讀寫 `ReShade.ini` 內的各項全域設定。
- **執行狀態與紀錄**：提供即時 FPS、單幀耗時、解析度、繪圖 API（DirectX 11 / DirectX 12 / Vulkan / OpenGL）與 GPU 資訊；提取 `ReShade.log` 最新內容以便排除語法錯誤。
- **渲染執行緒安全**：所有 ReShade API 操作均排隊提交至渲染主執行緒（`on_present`）執行，網路服務與繪圖管線嚴格解耦，避免因跨執行緒呼叫導致驅動程式崩潰。

---

## 下載與安裝

請至專案的 [Releases 頁面](../../releases) 下載已編譯的二進位檔案：

| 架構 | 適用環境 | 檔案名稱 |
| :--- | :--- | :--- |
| **64 位元** | 現代 PC 遊戲（DirectX 11 / DirectX 12 / Vulkan 等） | `ShadePilot.addon64` |
| **32 位元** | 經典 32 位元老遊戲（DirectX 9 / 經典模擬器等） | `ShadePilot.addon32` |

### 安裝步驟

1. 將 `ShadePilot.addon64`（或 `ShadePilot.addon32`）複製到遊戲主執行檔（`.exe`）所在目錄，或放置於 ReShade 的 `addons` 資料夾內。
2. 啟動遊戲，按下快捷鍵（預設為 `Home` 鍵）開啟 ReShade 選單。
3. 切換至 **「附加元件 (Add-ons)」** 頁籤，即可看到 **ShadePilot** 已被辨識並載入。
4. 元件啟動後會自動在背景執行本機 HTTP/SSE 服務，預設監聽連接埠為 `39800`。

### 連接埠與多實例處理

- **連接埠自動順延**：當同時啟動多個遊戲實例時，ShadePilot 會自動偵測連接埠占用情況。若預設連接埠 `39800` 已被使用，後續實例將自動依序嘗試 `39801`、`39802` 等可用連接埠。
- **自訂連接埠**：若需指定起始連接埠，可在遊戲目錄下的 `ReShade.ini` 中進行設定：
  ```ini
  [SHADEPILOT]
  Port = 39800
  ```
- **健康檢查介面**：透過瀏覽器或指令碼存取 `http://127.0.0.1:39800/health` 可直接取得診斷 JSON，包含當前生效的繪圖 API、GPU 型號、遊戲處理程序 PID、畫面更新率及實際監聽連接埠。

---

## AI 用戶端設定

### 1. Claude Desktop

在 Claude Desktop 設定檔（Windows 路徑：`%APPDATA%\Claude\claude_desktop_config.json`）中加入 SSE 服務設定：

```json
{
  "mcpServers": {
    "shadepilot": {
      "url": "http://127.0.0.1:39800/sse"
    }
  }
}
```

### 2. Cursor / Antigravity / 其他 MCP 用戶端

- **Server Type**: `sse`
- **Endpoint URL**: `http://127.0.0.1:39800/sse`

---

## MCP 工具清單

ShadePilot 提供 24 個 MCP 控制工具，涵蓋畫面擷取、著色器控制、預設檔存取、外掛與設定管理：

| 分類 | 工具名稱 | 功能說明 |
| :--- | :--- | :--- |
| **畫面感知** | `shadepilot_get_screen` | 擷取畫面。支援 `before`（效果處理前畫面）、`after`（效果處理後畫面）、`overlay`（包含 ReShade 選單的最終畫面）及 `both`（在同一幀內同時擷取 before/after 影像）。支援指定壓縮品質（預設 80%）。 |
| **著色器管理** | `shadepilot_list_effects` | 列出已載入的 `.fx` 檔案，回傳完整路徑、編譯狀態、Technique 數量與錯誤紀錄。 |
| | `shadepilot_list_techniques` | 列舉所有 Technique 名稱、所屬檔案、啟用狀態與顯示標籤。 |
| | `shadepilot_set_technique_state` | 啟用或停用指定的 Technique。 |
| | `shadepilot_reorder_techniques` | 調整 Techniques 的渲染執行先後順序。 |
| | `shadepilot_list_variables` | 列舉當前已啟用的著色器參數（滑桿、顏色、下拉選單等），過濾系統內部變數，回傳數值範圍、步進值與 UI 分組。 |
| | `shadepilot_set_variable` | 修改著色器參數值（支援浮點數、整數、布林值、三維/四維向量），預設自動儲存至目前預設檔。 |
| | `shadepilot_reset_variable` | 將指定著色器參數還原為預設值。 |
| | `shadepilot_get_preprocessor_definitions` | 查看著色器的前置處理器巨集定義清單。 |
| | `shadepilot_set_preprocessor_definition` | 修改前置處理器巨集定義，並通知 ReShade 於下一幀重新編譯著色器。 |
| | `shadepilot_reload_effects` | 重新編譯指定的 `.fx` 檔案或全部著色器。 |
| **預設與狀態** | `shadepilot_save_preset` | 將目前所有生效的著色器參數儲存至 `.ini` 預設檔。 |
| | `shadepilot_load_preset` | 切換並載入指定的 `.ini` 預設檔案。 |
| | `shadepilot_get_current_preset` | 取得目前正在使用的預設檔案路徑。 |
| | `shadepilot_set_performance_mode` / `get` | 開啟或關閉 ReShade 效能模式（效能模式會固化參數以最佳化編譯）。 |
| | `shadepilot_set_effects_state` / `get` | 全域開啟或停用所有效果（等同於 ReShade 全域主開關）。 |
| | `shadepilot_set_overlay_state` | 開啟或關閉 ReShade 遊戲內選單介面。 |
| **外掛管理** | `shadepilot_list_addons` | 掃描遊戲目錄及 `addons/` 目錄，列出所有 `.addon` / `.addon64` 檔案與說明、版本及啟用狀態。 |
| | `shadepilot_set_addon_state` | 啟用或停用指定的 ReShade 附加元件（於下次啟動遊戲時生效）。 |
| | `shadepilot_get_addon_config` | 讀取 `ReShade.ini` 中特定附加元件的設定區段（例如 `[DEPTH]` 等）。 |
| | `shadepilot_set_addon_config` | 寫入特定附加元件在 `ReShade.ini` 中的設定項目並儲存。 |
| **設定與診斷** | `shadepilot_get_config` | 讀取 `ReShade.ini` 中的特定設定項目。 |
| | `shadepilot_get_all_config` | 完整讀取 `ReShade.ini` 的全部設定區段，解析為結構化 JSON。 |
| | `shadepilot_set_config` | 修改 `ReShade.ini` 中的設定項目並通知重新載入生效。 |
| | `shadepilot_get_stats` | 取得即時 FPS、單幀耗時、解析度、繪圖 API、GPU 型號及當前啟用的 Technique 數量。 |
| | `shadepilot_get_logs` | 取得最新的 `ReShade.log` 執行紀錄（支援篩選 Warning/Error）。 |

---

## 本地建置

本專案採用 CMake 建置系統，於 Windows 環境下使用 MSVC 靜態執行階段函式庫（`/MT`）進行編譯：

```bash
# 建置 64 位元版本
cmake -B build64 -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build64 --config Release

# 建置 32 位元版本
cmake -B build32 -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build32 --config Release
```

建置產物位於：
- `build64/Release/ShadePilot.addon64`
- `build32/Release/ShadePilot.addon32`

---

## 自動化建置與發布

專案已配置 GitHub Actions 工作流程（`.github/workflows/release.yml`）：
- 於 GitHub 上建立並推送 Release Tag（如 `v1.0.0`）時自動觸發。
- 工作流程將自動解析 Tag 版本號，更新 `src/version.h` 內的巨集定義。
- 自動編譯產出 64 位元與 32 位元二進位檔案，並附加至 GitHub Release 供使用者下載。

---

## 授權條款

本專案採用 [MIT 授權條款](LICENSE) 開源。ReShade API 遵守 BSD-3-Clause 授權。
