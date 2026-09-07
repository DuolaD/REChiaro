#pragma once

#include <reshade.hpp>
#include <string>
#include <vector>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <memory>
#include "json.hpp"

namespace shadepilot
{
    struct FrameStats
    {
        std::string api_name;
        std::string device_name;
        uint32_t width = 0;
        uint32_t height = 0;
        float fps = 0.0f;
        float frame_time_ms = 0.0f;
        uint64_t frame_count = 0;
    };

    struct TechniqueInfo
    {
        std::string name;
        std::string effect_name;
        bool enabled = false;
        std::string label;
        std::string tooltip;
    };

    struct UniformVariableInfo
    {
        std::string name;
        std::string effect_name;
        std::string type;
        uint32_t rows = 0;
        uint32_t columns = 0;
        uint32_t array_length = 0;
        nlohmann::json current_value;
        std::string label;
        std::string tooltip;
        std::string ui_type;
        nlohmann::json min_value;
        nlohmann::json max_value;
        nlohmann::json step_value;
        std::vector<std::string> ui_items;
    };

    struct AddonInfo
    {
        std::string name;
        std::string description;
        std::string author;
        std::string file;
        std::string version;
        bool enabled = true;
    };

    class ReShadeBridge
    {
    public:
        static ReShadeBridge& instance();

        void on_init_effect_runtime(reshade::api::effect_runtime *runtime);
        void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime);
        void on_begin_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *cmd_list = nullptr, reshade::api::resource_view rtv = { 0 }, reshade::api::resource_view rtv_srgb = { 0 });
        void on_finish_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *cmd_list = nullptr, reshade::api::resource_view rtv = { 0 }, reshade::api::resource_view rtv_srgb = { 0 });
        void on_present(reshade::api::effect_runtime *runtime);

        bool is_runtime_active() const;
        FrameStats get_stats() const;

        // Thread-safe dispatch to render thread
        template <typename F>
        auto execute_on_render_thread(F&& f) -> std::future<typename std::invoke_result<F, reshade::api::effect_runtime*>::type>
        {
            using ReturnType = typename std::invoke_result<F, reshade::api::effect_runtime*>::type;
            auto task = std::make_shared<std::packaged_task<ReturnType(reshade::api::effect_runtime*)>>(std::forward<F>(f));
            std::future<ReturnType> fut = task->get_future();

            {
                std::lock_guard<std::mutex> lock(m_task_mutex);
                m_task_queue.push([task](reshade::api::effect_runtime *rt) {
                    (*task)(rt);
                });
            }
            return fut;
        }

        // High-level API exposed to MCP tools
        std::string capture_screen_base64(const std::string &stage, int quality = 85);
        std::vector<TechniqueInfo> list_techniques(bool enabled_only = false);
        bool set_technique_state(const std::string &technique_name, bool enabled);
        bool reorder_techniques(const std::vector<std::string> &technique_names);

        std::vector<UniformVariableInfo> list_uniform_variables(const std::string &effect_filter = "");
        bool set_uniform_variable(const std::string &effect_name, const std::string &var_name, const nlohmann::json &val);
        bool reset_uniform_variable(const std::string &effect_name, const std::string &var_name);

        nlohmann::json get_preprocessor_definitions(const std::string &effect_name = "");
        bool set_preprocessor_definition(const std::string &effect_name, const std::string &name, const std::string &value);

        bool save_current_preset();

        std::vector<AddonInfo> list_addons();
        bool set_addon_state(const std::string &addon_name, bool enabled);

        std::string get_config(const std::string &section, const std::string &key);
        bool set_config(const std::string &section, const std::string &key, const std::string &value);

        std::vector<std::string> get_recent_logs(size_t max_lines = 100, bool errors_only = false);

    private:
        ReShadeBridge() = default;
        ~ReShadeBridge() = default;

        void process_task_queue(reshade::api::effect_runtime *runtime);

        mutable std::mutex m_runtime_mutex;
        reshade::api::effect_runtime *m_current_runtime = nullptr;

        mutable std::mutex m_stats_mutex;
        FrameStats m_stats;
        std::chrono::high_resolution_clock::time_point m_last_present_time = std::chrono::high_resolution_clock::now();

        std::mutex m_task_mutex;
        std::queue<std::function<void(reshade::api::effect_runtime*)>> m_task_queue;

        // Pending frame captures
        std::mutex m_capture_mutex;
        bool m_capture_before_requested = false;
        bool m_capture_after_requested = false;
        std::vector<uint8_t> m_captured_before_pixels;
        std::vector<uint8_t> m_captured_after_pixels;
        uint32_t m_captured_width = 0;
        uint32_t m_captured_height = 0;
        std::condition_variable m_capture_cv;
    };
}
