# ShadePilot 🎮🎨

[![Release ShadePilot Addon](https://github.com/DuolaD/RE_MCP/actions/workflows/release.yml/badge.svg)](https://github.com/DuolaD/RE_MCP/actions/workflows/release.yml)
[![ReShade API v20](https://img.shields.io/badge/ReShade_API-v20-brightgreen.svg)](https://reshade.me)
[![MCP Protocol](https://img.shields.io/badge/MCP-2024--11--05-blue.svg)](https://modelcontextprotocol.io)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**ShadePilot** 是一个专为 **ReShade** 打造的高性能 **Model Context Protocol (MCP)** 插件（Add-on）。

它赋予了人工智能（Claude / Cursor / Antigravity / 智能体）**“看懂游戏画面”的眼睛**与**“操控光影与着色器”的双手**。通过安全可靠的渲染主线程调度机制，AI 可以实时观察未处理（Before）与处理后（After）的游戏画面，微调任意着色器参数与宏定义，保存预设，甚至诊断着色器编译日志。

---

## 🌟 核心特性

- 👁️ **全知双视界（Before & After）**：在渲染流水线的前置点（`reshade_begin_effects`）与后置点（`reshade_finish_effects`）捕获纯净无 UI 遮挡的原始画面与特效画面，经由内存高速压缩向 AI 提供直观视觉反馈。
- 🎛️ **着色器全面操控**：枚举所有加载的 Techniques，支持一键启闭指定滤镜，动态调整渲染执行顺序。
- 🎨 **参数精细调控（Uniform Variables）**：读取与更改任意着色器的滑块、浮点数、整数、布尔值及多维颜色向量，支持重置默认值。
- ⚙️ **宏定义与自动重载（Preprocessor Definitions）**：支持查看与修改预处理器定义（如 LUT 采样模式、高质量开关等），修改后自动通知 ReShade 在下一帧重新编译生效。
- 💾 **预设实时落地**：支持将调整完成的所有状态持久化写入当前活动的预设文件（`.ini`）。
- 🧩 **插件与全局设置管理**：支持扫描和列出已安装的 ReShade Add-ons 并切换启闭状态；读写 `ReShade.ini` 的任意设置项。
- 📊 **性能与日志诊断**：提供实时 FPS、单帧耗时、分辨率、渲染 API 信息；实时提取 `ReShade.log`，方便 AI 诊断语法报错。
- 🛡️ **绝对线程安全**：后台 HTTP/SSE 网络服务与图形渲染管线解耦，所有 ReShade 核心操作均排队在 Present 主线程安全执行，杜绝显卡崩溃。

---

## 📥 下载与安装

进入项目的 [Releases 页面](../../releases) 下载最新发行版：

| 架构 | 适用场景 | 目标文件 |
| :--- | :--- | :--- |
| **64-bit** | 绝大部分现代 PC 游戏（DX11 / DX12 / Vulkan 等） | `ShadePilot.addon64` |
| **32-bit** | 经典 32 位老游戏（DirectX 9 / 经典模拟器等） | `ShadePilot.addon32` |

### 安装步骤
1. 将下载的 `ShadePilot.addon64`（或 `ShadePilot.addon32`）复制到游戏的根目录（即游戏主程序 `.exe` 所在目录，或 ReShade 的 `addons` 目录）。
2. 启动游戏，按下快捷键（默认 `Home` 键）呼出 ReShade 界面。
3. 切换到顶部的 **“插件 (Add-ons)”** 一栏，即可看到 **ShadePilot**。
4. 默认情况下插件会自动在后台启动本地 MCP 服务，监听端口：`39800`。

> 💡 **多实例与端口自动顺延（Auto-increment Fallback）**：
> 当您同时多开游戏客户端时，ShadePilot 会自动检测端口占用情况。若默认端口 `39800` 已被首个实例使用，后续实例将自动顺延绑定至 `39801`、`39802` 等可用端口。
>
> 💡 **自定义端口**：如需自定义起始端口，可在游戏目录下的 `ReShade.ini` 中添加：
> ```ini
> [SHADEPILOT]
> Port = 39800
> ```
>
> 💡 **健康检查与渲染管线诊断**：
> 访问 `http://127.0.0.1:39800/health` 可直接获取诊断 JSON，包含当前生效的渲染管线（`DX11`/`DX12`/`Vulkan`/`OpenGL` 等）、GPU 显卡型号、游戏进程名（PID）、帧率及当前分配的端口。

---

## 🤖 接入 AI 客户端 (MCP Configuration)

### 1. Claude Desktop
在 Claude Desktop 的配置文件中（路径：%APPDATA%\Claude\claude_desktop_config.json）添加 SSE 服务配置：

```json
{
  "mcpServers": {
    "shadepilot": {
      "url": "http://127.0.0.1:39800/sse"
    }
  }
}
```

### 2. Cursor / Antigravity / 其他 MCP 客户端
- **Server Type**: `sse`
- **Endpoint URL**: `http://127.0.0.1:39800/sse`

---

## 🛠️ MCP Tools 接口矩阵

ShadePilot 为 AI 智能体提供了完整且完备的控制接口矩阵，全面对齐 ReShade 各功能选项卡（主页 Home、插件 Add-ons、设置 Settings、统计与日志）：

| 分类 | 工具名称 | 描述与特性 |
| :--- | :--- | :--- |
| **画面感知** | `shadepilot_get_screen` | 捕获游戏画面。支持 `before`（特效前纯净画面）、`after`（特效后画面）、`overlay`（包含 ReShade 游戏内全菜单与调试界面的最终画面）及 `both`（在同一渲染帧内**原子捕获** Before/After，零撕裂与时差）。 |
| **着色器 (图二)** | `shadepilot_list_effects` | 列出所有已安装与加载的着色器文件（`.fx`），包含文件全路径、编译成功状态、Technique 计数及错误日志。 |
| | `shadepilot_list_techniques` | 枚举所有着色器技术（Technique）名称、所属文件、启用状态与 UI 标签。 |
| | `shadepilot_set_technique_state` | 启用或禁用指定的着色器技术。 |
| | `shadepilot_reorder_techniques` | 动态调整着色器的渲染执行先后顺序。 |
| | `shadepilot_list_variables` | **对齐 ReShade 主页（Home tab）**：默认（`enabled_only=true`, `include_system=false`）仅枚举当前已启用着色器的调节参数（滑块、颜色、下拉项等），自动剔除系统只读时间/帧数变量，返回分类（`ui_category`）、数值范围、单位与步长。 |
| | `shadepilot_set_variable` | 任意更改着色器参数值（支持浮点、整数、布尔、三维/四维向量），默认自动将修改落地持久化至当前激活的预设文件（`auto_save=true`）。 |
| | `shadepilot_reset_variable` | 将指定着色器变量重置为其默认初始预设值。 |
| | `shadepilot_get_preprocessor_definitions` | 查看着色器的预处理器宏定义（Macros）。 |
| | `shadepilot_set_preprocessor_definition` | 修改预处理器宏定义并自动触发着色器下一帧热重载与编译。 |
| | `shadepilot_reload_effects` | 立即将指定着色器或全部着色器放入重载队列，下一帧无缝重新编译。 |
| **预设与状态** | `shadepilot_save_preset` | 将当前所有生效的参数和着色器状态保存到预设文件（`.ini`）。 |
| | `shadepilot_load_preset` | 切换加载指定的 ReShade 预设文件并即时应用。 |
| | `shadepilot_get_current_preset` | 获取当前正在生效的预设文件路径。 |
| | `shadepilot_set_performance_mode` / `get` | 开关 ReShade 性能模式（Performance Mode），触发着色器优化编译。 |
| | `shadepilot_set_effects_state` / `get` | 全局启用或禁用所有特效（等同于 ReShade 全局主开关快捷键）。 |
| | `shadepilot_set_overlay_state` | 远控打开或关闭 ReShade 游戏内原生浮层菜单。 |
| **插件 (图三)** | `shadepilot_list_addons` | **对齐 ReShade 插件页（Add-ons tab）**：遍历游戏根目录与 `addons/` 目录，扫描所有 `.addon` / `.addon64` 插件，返回插件名称、描述、作者、版本、网址及启用/禁用状态。 |
| | `shadepilot_set_addon_state` | 启用或禁用指定的插件（在 `ReShade.ini` 的 `[ADDONS]` 中配置，下次游戏启动生效）。 |
| | `shadepilot_get_addon_config` | **读取插件独立变量与配置**：读取 `ReShade.ini` 中属于该插件配置节（例如 `[DEPTH]`、`[OBS_CAPTURE]` 等）的全部键值对。 |
| | `shadepilot_set_addon_config` | **任意更改插件独立配置**：直接修改指定插件在 `ReShade.ini` 中的配置项并立即持久化。 |
| **设置与诊断** | `shadepilot_get_config` | 读取 `ReShade.ini` 中的单个配置项（如 `[OVERLAY] KeyOverlay`）。 |
| | `shadepilot_get_all_config` | 完整读取 `ReShade.ini` 的所有 Section 与键值对，结构化解析为 JSON 对象。 |
| | `shadepilot_set_config` | 任意修改 `ReShade.ini` 中的配置项并通知重载生效。 |
| | `shadepilot_get_stats` | 获取实时 FPS、单帧毫秒耗时、分辨率、图形 API（DX11/DX12/Vulkan）、GPU 型号/设备ID、当前已激活 Techniques 数量与名称。 |
| | `shadepilot_get_logs` | 获取最新 ReShade 运行日志（支持筛选 Warning/Error 或全文关键字检索）。 |

---

## 🔨 本地构建

项目采用标准 CMake 构建系统，使用 MSVC 静态运行时（`/MT`）编译，生成自包含的插件二进制：

```bash
# 构建 64 位版本
cmake -B build64 -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build64 --config Release

# 构建 32 位版本
cmake -B build32 -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build32 --config Release
```

产物将输出在：
- `build64/Release/ShadePilot.addon64`
- `build32/Release/ShadePilot.addon32`

---

## 🚀 GitHub Actions 自动化发行

本项目配置了完整的持续交付工作流（`.github/workflows/release.yml`）：
- 当在 GitHub 创建并发布新 Release（例如创建 Tag `v1.0.0`）时，工作流会自动触发。
- 工作流将自动提取 Release Tag，替换 `src/version.h` 中的版本号宏。
- 分别自动编译产出 `ShadePilot.addon64` 和 `ShadePilot.addon32`。
- 将两个架构产物自动上传附加至当前 GitHub Release 中。

---

## 📄 开源许可证

本项目基于 [MIT License](LICENSE) 开源。ReShade API 遵守 BSD-3-Clause 许可证。
