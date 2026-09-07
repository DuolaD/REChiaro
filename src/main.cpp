#include "reshade.hpp"
#include "version.h"
#include "reshade_bridge.hpp"
#include "mcp_server.hpp"

#include <windows.h>
#include <string>

extern "C" __declspec(dllexport) const char *NAME = "ShadePilot";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Model Context Protocol (MCP) server for ReShade, empowering AI to observe and control shaders, presets, and settings.";
extern "C" __declspec(dllexport) const char *AUTHOR = "DuolaD";
extern "C" __declspec(dllexport) const char *WEBSITE = "https://github.com/DuolaD/RE_MCP";

static void on_init_effect_runtime(reshade::api::effect_runtime *runtime)
{
    shadepilot::ReShadeBridge::instance().on_init_effect_runtime(runtime);
}

static void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
{
    shadepilot::ReShadeBridge::instance().on_destroy_effect_runtime(runtime);
}

static void on_reshade_begin_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *, reshade::api::resource_view, reshade::api::resource_view)
{
    shadepilot::ReShadeBridge::instance().on_begin_effects(runtime);
}

static void on_reshade_finish_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *, reshade::api::resource_view, reshade::api::resource_view)
{
    shadepilot::ReShadeBridge::instance().on_finish_effects(runtime);
}

static void on_reshade_present(reshade::api::effect_runtime *runtime)
{
    shadepilot::ReShadeBridge::instance().on_present(runtime);
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

    // Read configured port from ReShade.ini if present
    uint16_t port = 39800;
    char port_str[32] = {};
    size_t port_size = sizeof(port_str);
    if (reshade::get_config_value(nullptr, "SHADEPILOT", "Port", port_str, &port_size))
    {
        try {
            int p = std::stoi(port_str);
            if (p > 0 && p <= 65535) port = static_cast<uint16_t>(p);
        } catch (...) {}
    }

    // Start background MCP server
    shadepilot::MCPServer::instance().start(port);

    reshade::log::message(reshade::log::level::info,
        "[ShadePilot] Initialized successfully. Version: " SHADEPILOT_VERSION);

    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
    shadepilot::MCPServer::instance().stop();

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


