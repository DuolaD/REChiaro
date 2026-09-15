# REChiaro 🎮🎨

[English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md)

[![Release REChiaro Addon](https://github.com/DuolaD/REChiaro/actions/workflows/release.yml/badge.svg)](https://github.com/DuolaD/REChiaro/actions/workflows/release.yml)
[![ReShade API v20](https://img.shields.io/badge/ReShade_API-v20-brightgreen.svg)](https://reshade.me)
[![MCP Protocol](https://img.shields.io/badge/MCP-2024--11--05-blue.svg)](https://modelcontextprotocol.io)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

REChiaro 是一个运行在 ReShade 内部的 Model Context Protocol (MCP) 插件（Add-on）。它在游戏进程中启动本地 HTTP/SSE 服务，允许 AI 工具（Claude Desktop、Cursor、Antigravity 等）直接与 ReShade 图形管线交互：捕获特效前后的游戏画面、调整着色器参数、切换与保存预设、管理插件及全局配置，并读取运行时日志。

---

## 核心功能

- **画面捕获**：在着色器渲染前后（`reshade_begin_effects` 与 `reshade_finish_effects`）抓取画面，返回 Base64 编码的 JPEG 图像。支持原子抓取（在同一渲染帧内同时获取未处理画面与处理后画面），以及包含游戏内 ReShade 菜单的 Overlay 画面。
- **着色器管理**：查询已加载的 `.fx` 文件及其编译状态；枚举所有 Techniques，支持启闭指定特效与动态调整执行次序。
- **参数调优**：读取和修改 Uniform 变量（滑块、颜色、布尔值、向量等），对齐 ReShade 主页（Home）视图，自动过滤只读系统变量；修改后默认自动写入当前预设文件。
- **预处理器宏定义**：查询和修改着色器宏定义（如采样精度、高质量开关等），修改后自动触发 ReShade 下一帧热重载与重新编译。
- **预设存取**：随时将当前着色器参数保存到 `.ini` 预设，或切换加载其他预设文件。
- **插件与全局设置**：扫描已安装的 ReShade 插件并配置启闭状态；读写 `ReShade.ini` 中的各项全局配置。
- **运行状态与日志**：提供实时 FPS、帧耗时、分辨率、图形 API（DirectX 11 / DirectX 12 / Vulkan / OpenGL）和 GPU 信息；提取 `ReShade.log` 最新内容以便排查语法错误。
- **渲染线程安全**：所有 ReShade API 操作均排队提交至渲染主线程（`on_present`）执行，网络服务与图形管线严格解耦，避免因跨线程调用导致驱动崩溃。

---

## 下载与安装

从项目的 [Releases 页面](../../releases) 获取编译好的二进制文件：

| 架构 | 适用环境 | 文件名 |
| :--- | :--- | :--- |
| **64 位** | 现代 PC 游戏（DirectX 11 / DirectX 12 / Vulkan 等） | `REChiaro.addon64` |
| **32 位** | 经典 32 位老游戏（DirectX 9 / 经典模拟器等） | `REChiaro.addon32` |

### 安装步骤

1. 将 `REChiaro.addon64`（或 `REChiaro.addon32`）放置在 ReShade 配置文件中的指定位置。
2. 启动游戏，按快捷键（默认 `Home` 键）打开 ReShade 菜单。
3. 切换到 **“插件 (Add-ons)”** 页面，可以看到 **REChiaro** 已被识别并加载。
4. 插件启动后会自动在后台运行本地 HTTP/SSE 服务，默认监听端口为 `39800`。

### 端口与多实例处理

- **端口自动顺延**：当同时启动多个游戏实例时，REChiaro 会检测端口占用情况。若默认端口 `39800` 已被占用，后续实例将自动尝试 `39801`、`39802` 等端口。
- **自定义端口**：如需指定起始端口，可以在游戏目录下的 `ReShade.ini` 中配置：
  ```ini
  [RECHIARO]
  Port = 39800
  ```
- **健康检查接口**：浏览器或脚本访问 `http://127.0.0.1:39800/health` 可直接获取诊断 JSON，包含当前生效的图形 API、GPU 型号、游戏进程 PID、帧率及实际监听端口。

---

## AI 客户端配置

### 1. Claude Desktop

在 Claude Desktop 配置文件（Windows 路径：`%APPDATA%\Claude\claude_desktop_config.json`）中加入 SSE 服务配置：

```json
{
  "mcpServers": {
    "rechiaro": {
      "url": "http://127.0.0.1:39800/sse"
    }
  }
}
```

### 2. Cursor / Antigravity / 其他 MCP 客户端

- **Server Type**: `sse`
- **Endpoint URL**: `http://127.0.0.1:39800/sse`

---

## MCP 工具列表

REChiaro 提供了 24 个 MCP 控制工具，涵盖画面捕获、着色器调控、预设存取、插件与配置管理：

| 分类 | 工具名称 | 功能说明 |
| :--- | :--- | :--- |
| **画面感知** | `rechiaro_get_screen` | 捕获画面。支持 `before`（特效前画面）、`after`（特效后画面）、`overlay`（包含 ReShade 菜单的最终画面）及 `both`（在同一帧内同时捕获 before/after 图像）。支持指定压缩质量（默认 80%）。 |
| **着色器管理** | `rechiaro_list_effects` | 列出已加载的 `.fx` 文件，返回完整路径、编译状态、Technique 数量和错误日志。 |
| | `rechiaro_list_techniques` | 枚举所有 Technique 名称、所属文件、启用状态和显示标签。 |
| | `rechiaro_set_technique_state` | 启用或禁用指定的 Technique。 |
| | `rechiaro_reorder_techniques` | 调整 Techniques 的渲染执行先后顺序。 |
| | `rechiaro_list_variables` | 枚举当前已启用的着色器参数（滑块、颜色、下拉选项等），过滤系统内部变量，返回数值范围、步长与 UI 分组。 |
| | `rechiaro_set_variable` | 修改着色器参数值（支持浮点、整数、布尔、三维/四维向量），默认自动保存到当前预设。 |
| | `rechiaro_reset_variable` | 将指定着色器参数恢复为默认值。 |
| | `rechiaro_get_preprocessor_definitions` | 查看着色器的预处理器宏定义列表。 |
| | `rechiaro_set_preprocessor_definition` | 修改预处理器宏定义，并通知 ReShade 在下一帧重新编译着色器。 |
| | `rechiaro_reload_effects` | 重新编译指定的 `.fx` 文件或全部着色器。 |
| **预设与状态** | `rechiaro_save_preset` | 将当前所有生效的着色器参数保存到 `.ini` 预设文件。 |
| | `rechiaro_load_preset` | 切换并加载指定的 `.ini` 预设文件。 |
| | `rechiaro_get_current_preset` | 获取当前正在使用的预设文件路径。 |
| | `rechiaro_set_performance_mode` / `get` | 开启或关闭 ReShade 性能模式（性能模式会固化参数以优化编译）。 |
| | `rechiaro_set_effects_state` / `get` | 全局开启或禁用所有特效（等同于 ReShade 全局主开关）。 |
| | `rechiaro_set_overlay_state` | 打开或关闭 ReShade 游戏内菜单界面。 |
| **插件管理** | `rechiaro_list_addons` | 扫描游戏目录及 `addons/` 目录，列出所有 `.addon` / `.addon64` 文件及描述、版本和启用状态。 |
| | `rechiaro_set_addon_state` | 启用或禁用指定的 ReShade 插件（下次启动游戏时生效）。 |
| | `rechiaro_get_addon_config` | 读取 `ReShade.ini` 中特定插件的配置节（如 `[DEPTH]` 等）。 |
| | `rechiaro_set_addon_config` | 写入特定插件在 `ReShade.ini` 中的配置项并保存。 |
| **配置与诊断** | `rechiaro_get_config` | 读取 `ReShade.ini` 中的指定配置项。 |
| | `rechiaro_get_all_config` | 完整读取 `ReShade.ini` 的全部配置节，解析为结构化 JSON。 |
| | `rechiaro_set_config` | 修改 `ReShade.ini` 中的配置项并通知重载生效。 |
| | `rechiaro_get_stats` | 获取实时 FPS、单帧耗时、分辨率、渲染 API、GPU 型号及当前激活的 Technique 数量。 |
| | `rechiaro_get_logs` | 获取最新的 `ReShade.log` 运行日志（支持筛选 Warning/Error）。 |

---

## 本地编译

项目采用 CMake 构建系统，在 Windows 下使用 MSVC 的静态运行时（`/MT`）编译：

```bash
# 编译 64 位版本
cmake -B build64 -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build64 --config Release

# 编译 32 位版本
cmake -B build32 -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build32 --config Release
```

编译产物位于：
- `build64/Release/REChiaro.addon64`
- `build32/Release/REChiaro.addon32`

---

## 自动化构建与发布

项目配置了 GitHub Actions 工作流（`.github/workflows/release.yml`）：
- 当在 GitHub 上创建并推送 Release Tag（如 `v1.0.0`）时，工作流自动触发。
- 工作流解析 Tag 版本号，更新 `src/version.h` 中的宏定义。
- 自动编译 64 位与 32 位二进制文件，并作为 Release Assets 上传。

---

## 许可证

本项目基于 [MIT 许可证](LICENSE) 开源。ReShade API 遵守 BSD-3-Clause 许可证。
