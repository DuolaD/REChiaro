#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include "json.hpp"

namespace rechiaro
{
    class MCPServer
    {
    public:
        static MCPServer& instance();

        bool start(uint16_t port = 39800);
        void stop();
        bool restart(uint16_t port);
        bool is_running() const;
        uint16_t get_port() const;
        size_t get_active_client_count() const;

    private:
        MCPServer();
        ~MCPServer();

        void server_worker();
        nlohmann::json handle_json_rpc(const nlohmann::json &req);

        // Tool handlers
        nlohmann::json tool_get_screen(const nlohmann::json &args);
        nlohmann::json tool_list_effects(const nlohmann::json &args);
        nlohmann::json tool_list_techniques(const nlohmann::json &args);
        nlohmann::json tool_set_technique_state(const nlohmann::json &args);
        nlohmann::json tool_reorder_techniques(const nlohmann::json &args);
        nlohmann::json tool_list_variables(const nlohmann::json &args);
        nlohmann::json tool_set_variable(const nlohmann::json &args);
        nlohmann::json tool_reset_variable(const nlohmann::json &args);
        nlohmann::json tool_get_preprocessor_definitions(const nlohmann::json &args);
        nlohmann::json tool_set_preprocessor_definition(const nlohmann::json &args);
        nlohmann::json tool_save_preset(const nlohmann::json &args);
        nlohmann::json tool_load_preset(const nlohmann::json &args);
        nlohmann::json tool_get_current_preset(const nlohmann::json &args);
        nlohmann::json tool_set_performance_mode(const nlohmann::json &args);
        nlohmann::json tool_get_performance_mode(const nlohmann::json &args);
        nlohmann::json tool_reload_effects(const nlohmann::json &args);
        nlohmann::json tool_set_effects_state(const nlohmann::json &args);
        nlohmann::json tool_get_effects_state(const nlohmann::json &args);
        nlohmann::json tool_set_overlay_state(const nlohmann::json &args);
        nlohmann::json tool_list_addons(const nlohmann::json &args);
        nlohmann::json tool_set_addon_state(const nlohmann::json &args);
        nlohmann::json tool_get_addon_config(const nlohmann::json &args);
        nlohmann::json tool_set_addon_config(const nlohmann::json &args);
        nlohmann::json tool_get_config(const nlohmann::json &args);
        nlohmann::json tool_get_all_config(const nlohmann::json &args);
        nlohmann::json tool_set_config(const nlohmann::json &args);
        nlohmann::json tool_get_stats(const nlohmann::json &args);
        nlohmann::json tool_get_logs(const nlohmann::json &args);

        nlohmann::json get_tools_schema() const;

        std::atomic<bool> m_running{false};
        std::atomic<uint16_t> m_port{39800};
        std::thread m_worker_thread;

        // Active SSE clients & shutdown synchronization
        mutable std::mutex m_clients_mutex;
        std::atomic<size_t> m_client_counter{0};
        std::condition_variable m_cv_shutdown;
        std::mutex m_shutdown_mutex;
    };
}
