#define ImTextureID ImU64
#include "imgui.h"
#include "reshade.hpp"
#include "version.h"
#include "reshade_bridge.hpp"
#include "mcp_server.hpp"

#include <windows.h>
#include <string>
#include <chrono>

extern "C" __declspec(dllexport) const char *NAME = "REChiaro";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Model Context Protocol (MCP) server for ReShade, empowering AI to observe and control shaders, presets, and settings.";
extern "C" __declspec(dllexport) const char *AUTHOR = "DuolaD";
extern "C" __declspec(dllexport) const char *WEBSITE = "https://github.com/DuolaD/REChiaro";

static int s_input_port = 39800;
static bool s_port_initialized = false;
static std::string s_status_message = "";
static std::chrono::steady_clock::time_point s_status_message_time;

static void draw_settings_overlay(reshade::api::effect_runtime *)
{
    auto &server = rechiaro::MCPServer::instance();
    auto &bridge = rechiaro::ReShadeBridge::instance();
    const uint16_t current_port = server.get_port();
    const bool is_running = server.is_running();

    if (!s_port_initialized)
    {
        s_input_port = current_port;
        s_port_initialized = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 1. Server Status
    ImGui::Text("MCP Server Status:");
    ImGui::SameLine();
    if (is_running)
    {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Running (Active)");
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Stopped");
    }

    // 2. Current Listening Address & Quick Copy
    if (is_running)
    {
        const std::string sse_url = "http://127.0.0.1:" + std::to_string(current_port) + "/sse";
        ImGui::Text("Listening URL:  %s", sse_url.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy URL"))
        {
            ImGui::SetClipboardText(sse_url.c_str());
            s_status_message = "Copied SSE URL to clipboard!";
            s_status_message_time = std::chrono::steady_clock::now();
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Copy Claude Config"))
        {
            const std::string cfg = "{\n  \"mcpServers\": {\n    \"rechiaro\": {\n      \"url\": \"" + sse_url + "\"\n    }\n  }\n}";
            ImGui::SetClipboardText(cfg.c_str());
            s_status_message = "Copied Claude Desktop JSON config to clipboard!";
            s_status_message_time = std::chrono::steady_clock::now();
        }
    }

    // 3. Diagnostics Info
    const auto stats = bridge.get_stats();
    ImGui::Text("Render Pipeline: %s (%s) | %s",
        stats.pipeline_name.empty() ? "Detecting..." : stats.pipeline_name.c_str(),
        stats.api_name.empty() ? "Direct3D" : stats.api_name.c_str(),
        stats.device_name.empty() ? "GPU" : stats.device_name.c_str());

    ImGui::Text("Host Process:    %s (PID: %u) | Connected Clients: %zu",
        bridge.get_process_name().c_str(),
        bridge.get_process_id(),
        server.get_active_client_count());

    ImGui::Text("ReShade State:   Effects: %s | Perf Mode: %s",
        stats.effects_enabled ? "Enabled" : "Disabled",
        stats.performance_mode ? "ON" : "OFF");

    if (!stats.current_preset.empty())
    {
        ImGui::Text("Active Preset:   %s", stats.current_preset.c_str());
    }

    ImGui::Spacing();

    // 4. Port Configuration & Control
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("Server Port", &s_input_port, 1, 100))
    {
        if (s_input_port < 1024) s_input_port = 1024;
        if (s_input_port > 65535) s_input_port = 65535;
    }

    ImGui::SameLine();
    if (ImGui::Button("Apply & Restart"))
    {
        if (s_input_port >= 1024 && s_input_port <= 65535)
        {
            const uint16_t target_port = static_cast<uint16_t>(s_input_port);
            reshade::set_config_value(nullptr, "RECHIARO", "Port", std::to_string(target_port).c_str());
            server.restart(target_port);
            s_status_message = "Server restarted on port " + std::to_string(server.get_port());
            s_status_message_time = std::chrono::steady_clock::now();
        }
    }

    ImGui::SameLine();
    if (is_running)
    {
        if (ImGui::Button("Stop Server"))
        {
            server.stop();
            s_status_message = "Server stopped.";
            s_status_message_time = std::chrono::steady_clock::now();
        }
    }
    else
    {
        if (ImGui::Button("Start Server"))
        {
            server.start(static_cast<uint16_t>(s_input_port));
            s_status_message = "Server started on port " + std::to_string(server.get_port());
            s_status_message_time = std::chrono::steady_clock::now();
        }
    }

    // Status message feedback
    if (!s_status_message.empty())
    {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - s_status_message_time).count();
        if (elapsed < 5)
        {
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "%s", s_status_message.c_str());
        }
        else
        {
            s_status_message.clear();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
}

static void on_init_effect_runtime(reshade::api::effect_runtime *runtime)
{
    rechiaro::ReShadeBridge::instance().on_init_effect_runtime(runtime);
}

static void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
{
    rechiaro::ReShadeBridge::instance().on_destroy_effect_runtime(runtime);
}

static void on_reshade_begin_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb)
{
    rechiaro::ReShadeBridge::instance().on_begin_effects(runtime, cmd_list, rtv, rtv_srgb);
}

static void on_reshade_finish_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb)
{
    rechiaro::ReShadeBridge::instance().on_finish_effects(runtime, cmd_list, rtv, rtv_srgb);
}

static void on_reshade_present(reshade::api::effect_runtime *runtime)
{
    rechiaro::ReShadeBridge::instance().on_present(runtime);
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
    if (!reshade::register_addon(addon_module, reshade_module))
        return false;

    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_reshade_begin_effects);
    reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_reshade_finish_effects);
    reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);

    // Register Addons Settings Overlay UI under REChiaro
    reshade::register_overlay(nullptr, draw_settings_overlay);

    // Read configured port from ReShade.ini if present
    uint16_t port = 39800;
    char port_str[32] = {};
    size_t port_size = sizeof(port_str);
    if (!reshade::get_config_value(nullptr, "RECHIARO", "Port", port_str, &port_size))
    {
        port_size = sizeof(port_str);
        reshade::get_config_value(nullptr, "SHADEPILOT", "Port", port_str, &port_size);
    }
    try {
        int p = std::stoi(port_str);
        if (p > 0 && p <= 65535) port = static_cast<uint16_t>(p);
    } catch (...) {}

    // Start background MCP server
    rechiaro::MCPServer::instance().start(port);

    reshade::log::message(reshade::log::level::info,
        "[REChiaro] Initialized successfully. Version: " RECHIARO_VERSION);

    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
    rechiaro::MCPServer::instance().stop();

    reshade::unregister_overlay(nullptr, draw_settings_overlay);

    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
    reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_reshade_begin_effects);
    reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(on_reshade_finish_effects);
    reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);

    reshade::unregister_addon(addon_module, reshade_module);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

