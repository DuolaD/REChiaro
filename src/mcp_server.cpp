#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "httplib.h"
#include "mcp_server.hpp"
#include "reshade_bridge.hpp"
#include "version.h"

#include <iostream>
#include <chrono>

namespace shadepilot
{
    MCPServer& MCPServer::instance()
    {
        static MCPServer s_instance;
        return s_instance;
    }

    MCPServer::MCPServer() = default;
    MCPServer::~MCPServer()
    {
        stop();
    }

    bool MCPServer::start(uint16_t port)
    {
        if (m_running.load())
            return true;

        m_port = port;
        m_running = true;
        m_worker_thread = std::thread(&MCPServer::server_worker, this);
        return true;
    }

    void MCPServer::stop()
    {
        if (!m_running.load())
            return;

        m_running = false;
        // Connect locally to trigger unblock if needed
        try
        {
            httplib::Client cli("127.0.0.1", m_port.load());
            cli.set_connection_timeout(0, 100000);
            cli.Get("/shutdown");
        }
        catch (...) {}

        if (m_worker_thread.joinable())
            m_worker_thread.join();
    }

    bool MCPServer::is_running() const
    {
        return m_running.load();
    }

    uint16_t MCPServer::get_port() const
    {
        return m_port.load();
    }

    size_t MCPServer::get_active_client_count() const
    {
        return m_client_counter.load();
    }

    void MCPServer::server_worker()
    {
        httplib::Server svr;

        svr.set_default_headers({
            { "Access-Control-Allow-Origin", "*" },
            { "Access-Control-Allow-Methods", "GET, POST, OPTIONS" },
            { "Access-Control-Allow-Headers", "Content-Type, Authorization" }
        });

        svr.Options(".*", [](const httplib::Request &, httplib::Response &res) {
            res.status = 204;
        });

        svr.Get("/health", [this](const httplib::Request &, httplib::Response &res) {
            nlohmann::json status;
            status["status"] = "ok";
            status["server"] = "ShadePilot";
            status["version"] = SHADEPILOT_VERSION;
            status["port"] = m_port.load();

            const auto &bridge = ReShadeBridge::instance();
            status["process"] = {
                { "name", bridge.get_process_name() },
                { "pid", bridge.get_process_id() },
                { "path", bridge.get_process_path() }
            };

            status["runtime_active"] = bridge.is_runtime_active();
            const auto stats = bridge.get_stats();
            status["render_pipeline"] = stats.pipeline_name.empty() ? "Pending" : stats.pipeline_name;
            status["api"] = stats.api_name.empty() ? "Pending" : stats.api_name;
            status["device"] = stats.device_name;
            status["fps"] = stats.fps;
            status["resolution"] = std::to_string(stats.width) + "x" + std::to_string(stats.height);
            status["active_clients"] = m_client_counter.load();
            status["endpoints"] = {
                { "sse", "http://127.0.0.1:" + std::to_string(m_port.load()) + "/sse" },
                { "message", "http://127.0.0.1:" + std::to_string(m_port.load()) + "/message" }
            };

            res.set_content(status.dump(2), "application/json");
        });

        svr.Get("/shutdown", [&svr](const httplib::Request &, httplib::Response &res) {
            res.set_content("Shutting down", "text/plain");
            svr.stop();
        });

        // SSE Endpoint
        svr.Get("/sse", [this](const httplib::Request &, httplib::Response &res) {
            m_client_counter++;
            const std::string session_id = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

            res.set_chunked_content_provider("text/event-stream",
                [this, session_id](size_t offset, httplib::DataSink &sink) {
                    if (offset == 0)
                    {
                        std::string init_event = "event: endpoint\ndata: /message?sessionId=" + session_id + "\n\n";
                        sink.write(init_event.data(), init_event.size());
                        return true;
                    }

                    // Keep alive heartbeat loop
                    while (m_running.load())
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(15));
                        std::string ping = ": keepalive\n\n";
                        if (!sink.write(ping.data(), ping.size()))
                            break;
                    }
                    return false;
                },
                [this](bool) {
                    m_client_counter--;
                });
        });

        // JSON-RPC Message Endpoint
        auto handle_post_message = [this](const httplib::Request &req, httplib::Response &res) {
            try
            {
                auto j_req = nlohmann::json::parse(req.body);
                auto j_res = handle_json_rpc(j_req);
                res.set_content(j_res.dump(), "application/json");
            }
            catch (const std::exception &e)
            {
                nlohmann::json err;
                err["jsonrpc"] = "2.0";
                err["id"] = nullptr;
                err["error"] = { { "code", -32700 }, { "message", std::string("Parse error: ") + e.what() } };
                res.status = 400;
                res.set_content(err.dump(), "application/json");
            }
        };

        svr.Post("/message", handle_post_message);
        svr.Post("/mcp", handle_post_message);

        uint16_t base_port = m_port.load();
        constexpr int MAX_PORT_ATTEMPTS = 20;
        uint16_t bound_port = 0;

        for (int i = 0; i < MAX_PORT_ATTEMPTS; ++i)
        {
            uint16_t test_port = base_port + i;
            if (svr.bind_to_port("127.0.0.1", test_port))
            {
                bound_port = test_port;
                m_port = bound_port;
                if (i > 0)
                {
                    reshade::log::message(reshade::log::level::warning,
                        ("[ShadePilot] Port " + std::to_string(base_port) +
                         " is in use. Auto-fallback to port " + std::to_string(bound_port)).c_str());
                }
                break;
            }
            svr.stop();
        }

        if (bound_port == 0)
        {
            reshade::log::message(reshade::log::level::error,
                ("[ShadePilot] Failed to bind to any port in range [" +
                 std::to_string(base_port) + " - " + std::to_string(base_port + MAX_PORT_ATTEMPTS - 1) + "]. MCP server aborted.").c_str());
            m_running = false;
            return;
        }

        reshade::log::message(reshade::log::level::info,
            ("[ShadePilot] MCP Server listening on http://127.0.0.1:" + std::to_string(bound_port)).c_str());

        svr.listen_after_bind();
        m_running = false;
    }

    nlohmann::json MCPServer::handle_json_rpc(const nlohmann::json &req)
    {
        nlohmann::json resp;
        resp["jsonrpc"] = "2.0";

        if (req.contains("id"))
            resp["id"] = req["id"];
        else
            resp["id"] = nullptr;

        const std::string method = req.value("method", "");

        if (method == "initialize")
        {
            nlohmann::json result;
            result["protocolVersion"] = "2024-11-05";
            result["capabilities"] = {
                { "tools", { { "listChanged", false } } }
            };
            result["serverInfo"] = {
                { "name", "ShadePilot" },
                { "version", SHADEPILOT_VERSION }
            };
            resp["result"] = result;
        }
        else if (method == "notifications/initialized")
        {
            // Empty response for notification
            return nlohmann::json::object();
        }
        else if (method == "ping")
        {
            resp["result"] = nlohmann::json::object();
        }
        else if (method == "tools/list")
        {
            resp["result"] = { { "tools", get_tools_schema() } };
        }
        else if (method == "tools/call")
        {
            const auto &params = req.value("params", nlohmann::json::object());
            const std::string tool_name = params.value("name", "");
            const auto args = params.value("arguments", nlohmann::json::object());

            try
            {
                if (tool_name == "shadepilot_get_screen")
                    resp["result"] = tool_get_screen(args);
                else if (tool_name == "shadepilot_list_techniques")
                    resp["result"] = tool_list_techniques(args);
                else if (tool_name == "shadepilot_set_technique_state")
                    resp["result"] = tool_set_technique_state(args);
                else if (tool_name == "shadepilot_reorder_techniques")
                    resp["result"] = tool_reorder_techniques(args);
                else if (tool_name == "shadepilot_list_variables")
                    resp["result"] = tool_list_variables(args);
                else if (tool_name == "shadepilot_set_variable")
                    resp["result"] = tool_set_variable(args);
                else if (tool_name == "shadepilot_reset_variable")
                    resp["result"] = tool_reset_variable(args);
                else if (tool_name == "shadepilot_get_preprocessor_definitions")
                    resp["result"] = tool_get_preprocessor_definitions(args);
                else if (tool_name == "shadepilot_set_preprocessor_definition")
                    resp["result"] = tool_set_preprocessor_definition(args);
                else if (tool_name == "shadepilot_save_preset")
                    resp["result"] = tool_save_preset(args);
                else if (tool_name == "shadepilot_list_addons")
                    resp["result"] = tool_list_addons(args);
                else if (tool_name == "shadepilot_set_addon_state")
                    resp["result"] = tool_set_addon_state(args);
                else if (tool_name == "shadepilot_get_config")
                    resp["result"] = tool_get_config(args);
                else if (tool_name == "shadepilot_set_config")
                    resp["result"] = tool_set_config(args);
                else if (tool_name == "shadepilot_get_stats")
                    resp["result"] = tool_get_stats(args);
                else if (tool_name == "shadepilot_get_logs")
                    resp["result"] = tool_get_logs(args);
                else
                {
                    resp["error"] = { { "code", -32601 }, { "message", "Unknown tool: " + tool_name } };
                }
            }
            catch (const std::exception &e)
            {
                resp["result"] = {
                    { "content", {
                        { { "type", "text" }, { "text", std::string("Tool error: ") + e.what() } }
                    } },
                    { "isError", true }
                };
            }
        }
        else
        {
            resp["error"] = { { "code", -32601 }, { "message", "Method not found: " + method } };
        }

        return resp;
    }

    nlohmann::json MCPServer::tool_get_screen(const nlohmann::json &args)
    {
        const std::string stage = args.value("stage", "after");
        const int quality = args.value("quality", 85);

        nlohmann::json content = nlohmann::json::array();

        if (stage == "both")
        {
            std::string before_b64 = ReShadeBridge::instance().capture_screen_base64("before", quality);
            std::string after_b64 = ReShadeBridge::instance().capture_screen_base64("after", quality);

            if (!before_b64.empty())
            {
                content.push_back({
                    { "type", "text" },
                    { "text", "=== ORIGINAL SCREEN (Before ReShade Effects) ===" }
                });
                content.push_back({
                    { "type", "image" },
                    { "data", before_b64 },
                    { "mimeType", "image/jpeg" }
                });
            }

            if (!after_b64.empty())
            {
                content.push_back({
                    { "type", "text" },
                    { "text", "=== PROCESSED SCREEN (After ReShade Effects) ===" }
                });
                content.push_back({
                    { "type", "image" },
                    { "data", after_b64 },
                    { "mimeType", "image/jpeg" }
                });
            }
        }
        else
        {
            std::string img_b64 = ReShadeBridge::instance().capture_screen_base64(stage, quality);
            if (img_b64.empty())
            {
                content.push_back({
                    { "type", "text" },
                    { "text", "Failed to capture screenshot. The game window might be minimized or ReShade runtime is uninitialized." }
                });
                return { { "content", content }, { "isError", true } };
            }

            content.push_back({
                { "type", "image" },
                { "data", img_b64 },
                { "mimeType", "image/jpeg" }
            });
            content.push_back({
                { "type", "text" },
                { "text", "Successfully captured " + stage + " frame." }
            });
        }

        return { { "content", content } };
    }

    nlohmann::json MCPServer::tool_list_techniques(const nlohmann::json &args)
    {
        const bool enabled_only = args.value("enabled_only", false);
        const auto techs = ReShadeBridge::instance().list_techniques(enabled_only);

        nlohmann::json arr = nlohmann::json::array();
        for (const auto &t : techs)
        {
            arr.push_back({
                { "name", t.name },
                { "effect_name", t.effect_name },
                { "enabled", t.enabled },
                { "label", t.label },
                { "tooltip", t.tooltip }
            });
        }

        return {
            { "content", {
                { { "type", "text" }, { "text", arr.dump(2) } }
            } }
        };
    }

    nlohmann::json MCPServer::tool_set_technique_state(const nlohmann::json &args)
    {
        const std::string name = args.value("technique", "");
        const bool enabled = args.value("enabled", true);

        const bool ok = ReShadeBridge::instance().set_technique_state(name, enabled);
        return {
            { "content", {
                { { "type", "text" }, { "text", ok ? ("Set technique '" + name + "' to " + (enabled ? "enabled" : "disabled")) : ("Failed to find technique: " + name) } }
            } },
            { "isError", !ok }
        };
    }

    nlohmann::json MCPServer::tool_reorder_techniques(const nlohmann::json &args)
    {
        std::vector<std::string> names;
        if (args.contains("techniques") && args["techniques"].is_array())
            names = args["techniques"].get<std::vector<std::string>>();

        const bool ok = ReShadeBridge::instance().reorder_techniques(names);
        return {
            { "content", {
                { { "type", "text" }, { "text", ok ? "Techniques reordered successfully." : "Failed to reorder techniques." } }
            } },
            { "isError", !ok }
        };
    }

    nlohmann::json MCPServer::tool_list_variables(const nlohmann::json &args)
    {
        const std::string effect = args.value("effect", "");
        const auto vars = ReShadeBridge::instance().list_uniform_variables(effect);

        nlohmann::json arr = nlohmann::json::array();
        for (const auto &v : vars)
        {
            nlohmann::json item = {
                { "name", v.name },
                { "effect_name", v.effect_name },
                { "type", v.type },
                { "label", v.label },
                { "tooltip", v.tooltip },
                { "current_value", v.current_value }
            };
            if (!v.ui_type.empty()) item["ui_type"] = v.ui_type;
            if (!v.min_value.is_null()) item["ui_min"] = v.min_value;
            if (!v.max_value.is_null()) item["ui_max"] = v.max_value;
            if (!v.step_value.is_null()) item["ui_step"] = v.step_value;
            if (!v.ui_items.empty()) item["ui_items"] = v.ui_items;

            arr.push_back(std::move(item));
        }

        return {
            { "content", {
                { { "type", "text" }, { "text", arr.dump(2) } }
            } }
        };
    }

    nlohmann::json MCPServer::tool_set_variable(const nlohmann::json &args)
    {
        const std::string effect = args.value("effect", "");
        const std::string var = args.value("variable", "");
        const auto val = args.value("value", nlohmann::json());

        const bool ok = ReShadeBridge::instance().set_uniform_variable(effect, var, val);
        return {
            { "content", {
                { { "type", "text" }, { "text", ok ? ("Successfully updated variable '" + var + "'") : ("Failed to set variable: " + var) } }
            } },
            { "isError", !ok }
        };
    }

    nlohmann::json MCPServer::tool_reset_variable(const nlohmann::json &args)
    {
        const std::string effect = args.value("effect", "");
        const std::string var = args.value("variable", "");

        const bool ok = ReShadeBridge::instance().reset_uniform_variable(effect, var);
        return {
            { "content", {
                { { "type", "text" }, { "text", ok ? ("Reset variable '" + var + "' to default.") : ("Failed to reset variable: " + var) } }
            } },
            { "isError", !ok }
        };
    }

    nlohmann::json MCPServer::tool_get_preprocessor_definitions(const nlohmann::json &args)
    {
        const std::string effect = args.value("effect", "");
        const auto defs = ReShadeBridge::instance().get_preprocessor_definitions(effect);

        return {
            { "content", {
                { { "type", "text" }, { "text", defs.dump(2) } }
            } }
        };
    }

    nlohmann::json MCPServer::tool_set_preprocessor_definition(const nlohmann::json &args)
    {
        const std::string effect = args.value("effect", "");
        const std::string name = args.value("name", "");
        const std::string value = args.value("value", "");

        const bool ok = ReShadeBridge::instance().set_preprocessor_definition(effect, name, value);
        return {
            { "content", {
                { { "type", "text" }, { "text", ok ? ("Updated preprocessor definition '" + name + "' = '" + value + "' and queued reload.") : "Failed to set definition." } }
            } },
            { "isError", !ok }
        };
    }

    nlohmann::json MCPServer::tool_save_preset(const nlohmann::json &)
    {
        const bool ok = ReShadeBridge::instance().save_current_preset();
        return {
            { "content", {
                { { "type", "text" }, { "text", ok ? "Current preset saved to disk successfully." : "Failed to save preset." } }
            } },
            { "isError", !ok }
        };
    }

    nlohmann::json MCPServer::tool_list_addons(const nlohmann::json &)
    {
        const auto addons = ReShadeBridge::instance().list_addons();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &a : addons)
        {
            arr.push_back({
                { "name", a.name },
                { "description", a.description },
                { "author", a.author },
                { "file", a.file },
                { "enabled", a.enabled }
            });
        }

        return {
            { "content", {
                { { "type", "text" }, { "text", arr.dump(2) } }
            } }
        };
    }

    nlohmann::json MCPServer::tool_set_addon_state(const nlohmann::json &args)
    {
        const std::string name = args.value("addon_name", "");
        const bool enabled = args.value("enabled", true);

        const bool ok = ReShadeBridge::instance().set_addon_state(name, enabled);
        return {
            { "content", {
                { { "type", "text" }, { "text", ok ? ("Set addon '" + name + "' to " + (enabled ? "enabled" : "disabled") + ". Restart game to take effect.") : "Failed to set addon state." } }
            } },
            { "isError", !ok }
        };
    }

    nlohmann::json MCPServer::tool_get_config(const nlohmann::json &args)
    {
        const std::string section = args.value("section", "");
        const std::string key = args.value("key", "");

        const std::string val = ReShadeBridge::instance().get_config(section, key);
        return {
            { "content", {
                { { "type", "text" }, { "text", val } }
            } }
        };
    }

    nlohmann::json MCPServer::tool_set_config(const nlohmann::json &args)
    {
        const std::string section = args.value("section", "");
        const std::string key = args.value("key", "");
        const std::string value = args.value("value", "");

        const bool ok = ReShadeBridge::instance().set_config(section, key, value);
        return {
            { "content", {
                { { "type", "text" }, { "text", ok ? ("Set [" + section + "] " + key + " = " + value) : "Failed to set config." } }
            } },
            { "isError", !ok }
        };
    }

    nlohmann::json MCPServer::tool_get_stats(const nlohmann::json &)
    {
        const auto &bridge = ReShadeBridge::instance();
        const auto stats = bridge.get_stats();
        nlohmann::json j = {
            { "render_pipeline", stats.pipeline_name.empty() ? "Pending" : stats.pipeline_name },
            { "api", stats.api_name.empty() ? "Pending" : stats.api_name },
            { "device", stats.device_name },
            { "fps", stats.fps },
            { "frame_time_ms", stats.frame_time_ms },
            { "resolution", std::to_string(stats.width) + "x" + std::to_string(stats.height) },
            { "frame_count", stats.frame_count },
            { "port", m_port.load() },
            { "process", {
                { "name", bridge.get_process_name() },
                { "pid", bridge.get_process_id() },
                { "path", bridge.get_process_path() }
            } },
            { "runtime_active", bridge.is_runtime_active() }
        };

        return {
            { "content", {
                { { "type", "text" }, { "text", j.dump(2) } }
            } }
        };
    }

    nlohmann::json MCPServer::tool_get_logs(const nlohmann::json &args)
    {
        const size_t max_lines = args.value("line_count", 100);
        const bool errors_only = args.value("errors_only", false);

        const auto logs = ReShadeBridge::instance().get_recent_logs(max_lines, errors_only);
        std::string text;
        for (const auto &line : logs)
        {
            text += line;
            text += "\n";
        }

        return {
            { "content", {
                { { "type", "text" }, { "text", text } }
            } }
        };
    }

    nlohmann::json MCPServer::get_tools_schema() const
    {
        return nlohmann::json::array({
            {
                { "name", "shadepilot_get_screen" },
                { "description", "Captures the game screen. Can capture 'before' (original unprocessed game frame), 'after' (processed with ReShade shaders), or 'both' for direct visual comparison." },
                { "inputSchema", {
                    { "type", "object" },
                    { "properties", {
                        { "stage", { { "type", "string" }, { "enum", { "before", "after", "both" } }, { "default", "after" }, { "description", "Which frame to capture: before effects, after effects, or both." } } },
                        { "quality", { { "type", "integer" }, { "default", 85 }, { "description", "JPEG quality (1-100)." } } }
                    } }
                } }
            },
            {
                { "name", "shadepilot_list_techniques" },
                { "description", "Lists all shader techniques loaded in ReShade, their file origins, whether they are enabled, and their display labels." },
                { "inputSchema", {
                    { "type", "object" },
                    { "properties", {
                        { "enabled_only", { { "type", "boolean" }, { "default", false }, { "description", "If true, only returns currently enabled techniques." } } }
                    } }
                } }
            },
            {
                { "name", "shadepilot_set_technique_state" },
                { "description", "Enables or disables a specific ReShade shader technique." },
                { "inputSchema", {
                    { "type", "object" },
                    { "required", { "technique", "enabled" } },
                    { "properties", {
                        { "technique", { { "type", "string" }, { "description", "The name of the technique (e.g. 'MartysMods_LAUNCHPAD', 'Vibrance')." } } },
                        { "enabled", { { "type", "boolean" }, { "description", "True to enable, false to disable." } } }
                    } }
                } }
            },
            {
                { "name", "shadepilot_reorder_techniques" },
                { "description", "Changes the rendering order of loaded techniques." },
                { "inputSchema", {
                    { "type", "object" },
                    { "required", { "techniques" } },
                    { "properties", {
                        { "techniques", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "Array of technique names in the desired execution order." } } }
                    } }
                } }
            },
            {
                { "name", "shadepilot_list_variables" },
                { "description", "Enumerates uniform variables (sliders, colors, toggles) of loaded effects with their current values, ranges, types, and descriptions." },
                { "inputSchema", {
                    { "type", "object" },
                    { "properties", {
                        { "effect", { { "type", "string" }, { "default", "" }, { "description", "Optional effect file name (e.g. 'MartysMods_REGRADE+.fx') to filter variables. If empty, lists all." } } }
                    } }
                } }
            },
            {
                { "name", "shadepilot_set_variable" },
                { "description", "Modifies the value of a specific uniform variable in an effect shader." },
                { "inputSchema", {
                    { "type", "object" },
                    { "required", { "variable", "value" } },
                    { "properties", {
                        { "effect", { { "type", "string" }, { "default", "" }, { "description", "Effect filename (e.g. 'MartysMods_REGRADE+.fx')." } } },
                        { "variable", { { "type", "string" }, { "description", "Variable declaration name (e.g. 'Exposure', 'Saturation')." } } },
                        { "value", { { "description", "New value: number for float/int, boolean for bool, or array for vectors." } } }
                    } }
                } }
            },
            {
                { "name", "shadepilot_reset_variable" },
                { "description", "Resets a specific uniform variable to its default preset value." },
                { "inputSchema", {
                    { "type", "object" },
                    { "required", { "variable" } },
                    { "properties", {
                        { "effect", { { "type", "string" }, { "default", "" }, { "description", "Effect filename." } } },
                        { "variable", { { "type", "string" }, { "description", "Variable name to reset." } } }
                    } }
                } }
            },
            {
                { "name", "shadepilot_get_preprocessor_definitions" },
                { "description", "Gets preprocessor definitions (macros) for a specific effect or common global macros." },
                { "inputSchema", {
                    { "type", "object" },
                    { "properties", {
                        { "effect", { { "type", "string" }, { "default", "" }, { "description", "Effect filename (e.g. 'MartysMods_LUTMANAGER.fx')." } } }
                    } }
                } }
            },
            {
                { "name", "shadepilot_set_preprocessor_definition" },
                { "description", "Sets a preprocessor definition (macro) for an effect and triggers automatic recompilation." },
                { "inputSchema", {
                    { "type", "object" },
                    { "required", { "name", "value" } },
                    { "properties", {
                        { "effect", { { "type", "string" }, { "default", "" }, { "description", "Effect filename." } } },
                        { "name", { { "type", "string" }, { "description", "Preprocessor definition name (e.g. 'ENHANCED_LUT_QUALITY')." } } },
                        { "value", { { "type", "string" }, { "description", "Value to define (e.g. '1', '0')." } } }
                    } }
                } }
            },
            {
                { "name", "shadepilot_save_preset" },
                { "description", "Saves all currently active shader technique states and uniform variable modifications to the active preset ini file on disk." },
                { "inputSchema", { { "type", "object" } } }
            },
            {
                { "name", "shadepilot_list_addons" },
                { "description", "Lists all installed ReShade Add-ons, their files, descriptions, authors, and enabled/disabled status." },
                { "inputSchema", { { "type", "object" } } }
            },
            {
                { "name", "shadepilot_set_addon_state" },
                { "description", "Enables or disables an installed ReShade Add-on (takes effect upon next game launch)." },
                { "inputSchema", {
                    { "type", "object" },
                    { "required", { "addon_name", "enabled" } },
                    { "properties", {
                        { "addon_name", { { "type", "string" }, { "description", "Add-on name or filename." } } },
                        { "enabled", { { "type", "boolean" }, { "description", "True to enable, false to disable." } } }
                    } }
                } }
            },
            {
                { "name", "shadepilot_get_config" },
                { "description", "Reads a configuration setting from ReShade.ini." },
                { "inputSchema", {
                    { "type", "object" },
                    { "required", { "section", "key" } },
                    { "properties", {
                        { "section", { { "type", "string" }, { "description", "Section name (e.g. 'OVERLAY', 'INPUT', 'DEPTH')." } } },
                        { "key", { { "type", "string" }, { "description", "Key name (e.g. 'KeyOverlay', 'PerformanceMode')." } } }
                    } }
                } }
            },
            {
                { "name", "shadepilot_set_config" },
                { "description", "Writes a configuration setting to ReShade.ini and reloads configuration." },
                { "inputSchema", {
                    { "type", "object" },
                    { "required", { "section", "key", "value" } },
                    { "properties", {
                        { "section", { { "type", "string" }, { "description", "Section name." } } },
                        { "key", { { "type", "string" }, { "description", "Key name." } } },
                        { "value", { { "type", "string" }, { "description", "String value to set." } } }
                    } }
                } }
            },
            {
                { "name", "shadepilot_get_stats" },
                { "description", "Gets real-time rendering statistics including current FPS, frame duration (ms), graphics API (D3D11/D3D12/Vulkan), and resolution." },
                { "inputSchema", { { "type", "object" } } }
            },
            {
                { "name", "shadepilot_get_logs" },
                { "description", "Retrieves the recent lines of ReShade.log, with option to filter for error and warning messages." },
                { "inputSchema", {
                    { "type", "object" },
                    { "properties", {
                        { "line_count", { { "type", "integer" }, { "default", 100 }, { "description", "Maximum lines of log to fetch." } } },
                        { "errors_only", { { "type", "boolean" }, { "default", false }, { "description", "If true, only returns lines containing ERROR or WARN." } } }
                    } }
                } }
            }
        });
    }
}

