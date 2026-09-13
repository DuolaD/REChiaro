# ShadePilot 🎮🎨

[English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md)

[![Release ShadePilot Addon](https://github.com/DuolaD/RE_MCP/actions/workflows/release.yml/badge.svg)](https://github.com/DuolaD/RE_MCP/actions/workflows/release.yml)
[![ReShade API v20](https://img.shields.io/badge/ReShade_API-v20-brightgreen.svg)](https://reshade.me)
[![MCP Protocol](https://img.shields.io/badge/MCP-2024--11--05-blue.svg)](https://modelcontextprotocol.io)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

ShadePilot is an in-process Model Context Protocol (MCP) add-on for ReShade. Running an embedded HTTP/SSE server inside the target game process, it enables AI assistants (such as Claude Desktop, Cursor, and Antigravity) to inspect the graphics pipeline, capture raw and post-processed game frames, adjust shader variables, switch presets, manage add-ons, and diagnose compilation errors.

---

## Features

- **Screen Capture**: Captures game frames before shader execution (`reshade_begin_effects`) and after post-processing (`reshade_finish_effects`) as Base64-encoded JPEG images. Supports atomic capture (`both`) to retrieve both stages within the exact same rendering frame with zero timing drift, as well as capturing the full in-game menu overlay (`overlay`).
- **Effect & Technique Control**: Enumerates loaded `.fx` files, compilation states, and error logs. Lists techniques, toggles individual effects on or off, and reorders technique execution order.
- **Parameter Tuning**: Reads and writes uniform variables (sliders, colors, booleans, and vectors). Matches ReShade's Home tab by filtering out internal read-only system variables, returning UI categories, step sizes, and valid ranges. Automatically persists adjustments to the active `.ini` preset.
- **Preprocessor Definitions**: Inspects and updates preprocessor macros, automatically scheduling ReShade to recompile affected shaders on the following frame.
- **Preset Management**: Saves active parameter configurations to `.ini` presets, loads alternate preset files on the fly, and inspects the currently active preset path.
- **Add-on & Configuration Management**: Discovers installed `.addon` and `.addon64` binaries, toggles their enabled status in `ReShade.ini`, and reads or writes arbitrary configuration sections.
- **Performance & Diagnostics**: Reports real-time frame rates, frame times, resolutions, graphics APIs (DirectX 11, DirectX 12, Vulkan, OpenGL), and GPU hardware details. Retrieves recent lines from `ReShade.log` with optional error and warning filters.
- **Thread-Safe Architecture**: Network operations run on background threads while all ReShade API calls are dispatched to execute on the graphics presentation thread (`on_present`), preventing GPU device hung states and driver crashes.

---

## Download & Installation

Pre-built binaries are available on the [Releases page](../../releases):

| Architecture | Target Environment | Binary |
| :--- | :--- | :--- |
| **64-bit** | Modern PC titles (DirectX 11, DirectX 12, Vulkan, OpenGL) | `ShadePilot.addon64` |
| **32-bit** | Legacy 32-bit titles (DirectX 9, emulators) | `ShadePilot.addon32` |

### Setup Steps

1. Copy `ShadePilot.addon64` (or `ShadePilot.addon32`) into the game directory containing the main `.exe`, or into ReShade's `addons` folder.
2. Launch the game and press the ReShade overlay key (default is `Home`).
3. Switch to the **Add-ons** tab to verify that **ShadePilot** is loaded.
4. ShadePilot automatically starts its local HTTP/SSE server on port `39800`.

### Multi-Instance & Port Handling

- **Auto-Increment Fallback**: When running multiple game clients simultaneously, ShadePilot verifies port availability using `SO_EXCLUSIVEADDRUSE`. If port `39800` is in use, subsequent instances automatically bind to `39801`, `39802`, and so forth.
- **Custom Port**: To configure a custom starting port, add the following section to `ReShade.ini` in the game directory:
  ```ini
  [SHADEPILOT]
  Port = 39800
  ```
- **Health Check Endpoint**: Navigating to `http://127.0.0.1:39800/health` returns diagnostic JSON including the active rendering API, GPU device details, process PID, FPS, and the assigned port.

---

## AI Client Configuration

### 1. Claude Desktop

Add the SSE server configuration to Claude Desktop's configuration file (`%APPDATA%\Claude\claude_desktop_config.json` on Windows):

```json
{
  "mcpServers": {
    "shadepilot": {
      "url": "http://127.0.0.1:39800/sse"
    }
  }
}
```

### 2. Cursor / Antigravity / Other MCP Clients

- **Server Type**: `sse`
- **Endpoint URL**: `http://127.0.0.1:39800/sse`

---

## MCP Tools Reference

ShadePilot registers 24 MCP tools covering frame capture, shader control, presets, and diagnostics:

| Category | Tool Name | Description |
| :--- | :--- | :--- |
| **Capture** | `shadepilot_get_screen` | Captures game frames. Options include `before` (pre-effects), `after` (post-effects), `overlay` (with ReShade menu), and `both` (atomic same-frame dual capture). Supports configurable JPEG quality (default 80%). |
| **Shaders** | `shadepilot_list_effects` | Lists loaded `.fx` files with file paths, compilation success status, technique counts, and compiler error logs. |
| | `shadepilot_list_techniques` | Lists all technique names, source `.fx` files, enabled states, and UI labels. |
| | `shadepilot_set_technique_state` | Enables or disables a specific technique. |
| | `shadepilot_reorder_techniques` | Reorders technique execution sequence in the render pipeline. |
| | `shadepilot_list_variables` | Enumerates uniform variables for enabled effects, filtering out internal read-only system uniforms. Returns UI categories, value ranges, and steps. |
| | `shadepilot_set_variable` | Updates a uniform variable (float, integer, boolean, or vector). Automatically saves to the active preset by default. |
| | `shadepilot_reset_variable` | Resets a uniform variable to its default preset value. |
| | `shadepilot_get_preprocessor_definitions` | Returns preprocessor macro definitions for loaded shaders. |
| | `shadepilot_set_preprocessor_definition` | Sets a preprocessor macro and triggers shader recompilation on the next frame. |
| | `shadepilot_reload_effects` | Reloads and recompiles a specific `.fx` file or all active shaders. |
| **Presets & State** | `shadepilot_save_preset` | Persists all current shader parameters to an `.ini` preset file. |
| | `shadepilot_load_preset` | Switches to and applies a different `.ini` preset file. |
| | `shadepilot_get_current_preset` | Retrieves the file path of the currently active preset. |
| | `shadepilot_set_performance_mode` / `get` | Toggles ReShade Performance Mode (bakes parameters into shader source code for runtime optimization). |
| | `shadepilot_set_effects_state` / `get` | Globally enables or disables all post-processing effects. |
| | `shadepilot_set_overlay_state` | Opens or closes ReShade's native in-game menu overlay. |
| **Add-ons** | `shadepilot_list_addons` | Scans the game folder and `addons/` directory for installed `.addon` and `.addon64` files, returning metadata, version, and enabled status. |
| | `shadepilot_set_addon_state` | Enables or disables an installed add-on (takes effect on the next game launch). |
| | `shadepilot_get_addon_config` | Reads configuration settings for a specific add-on section in `ReShade.ini` (e.g. `[DEPTH]`). |
| | `shadepilot_set_addon_config` | Writes settings for a specific add-on section in `ReShade.ini` and saves immediately. |
| **Settings & Logs** | `shadepilot_get_config` | Reads an individual configuration setting from `ReShade.ini`. |
| | `shadepilot_get_all_config` | Reads all sections and key-value pairs from `ReShade.ini` as a structured JSON object. |
| | `shadepilot_set_config` | Modifies a setting in `ReShade.ini` and triggers a configuration reload. |
| | `shadepilot_get_stats` | Returns real-time FPS, frame duration (ms), resolution, graphics API, GPU vendor and device ID, and active technique count. |
| | `shadepilot_get_logs` | Fetches recent lines from `ReShade.log`, with an optional filter for warnings and errors. |

---

## Building from Source

The project uses CMake with MSVC static runtime (`/MT`):

```bash
# Build 64-bit release
cmake -B build64 -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build64 --config Release

# Build 32-bit release
cmake -B build32 -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build32 --config Release
```

Build outputs:
- `build64/Release/ShadePilot.addon64`
- `build32/Release/ShadePilot.addon32`

---

## Automated Releases

A GitHub Actions workflow (`.github/workflows/release.yml`) handles release packaging:
- Triggered whenever a release tag (e.g. `v1.0.0`) is published.
- Updates the version macro in `src/version.h` to match the tag.
- Compiles both 64-bit and 32-bit binaries.
- Uploads the resulting artifacts to the GitHub Release.

---

## License

This project is licensed under the [MIT License](LICENSE). The ReShade API is distributed under the BSD-3-Clause license.
