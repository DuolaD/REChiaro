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
        std::string pipeline_name;
        std::string device_name;
        uint32_t vendor_id = 0;
        uint32_t device_id = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        float fps = 0.0f;
        float frame_time_ms = 0.0f;
        uint64_t frame_count = 0;
        bool performance_mode = false;
        bool effects_enabled = true;
        std::string current_preset;
        size_t total_techniques = 0;
        size_t enabled_techniques = 0;
        std::vector<std::string> enabled_technique_names;
    };

    struct EffectInfo
    {
        std::string file_name;
        std::string full_path;
        bool compiled = true;
        std::vector<std::string> errors;
        size_t technique_count = 0;
        size_t enabled_technique_count = 0;
    };

    struct TechniqueInfo
    {
        std::string name;
        std::string effect_name;
        bool enabled = false;
        std::string label;
        std::string tooltip;
        bool hidden = false;
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
        std::string ui_category;
        std::string ui_type;
        std::string ui_units;
        int ui_digits = -1;
        bool is_system = false;
        bool is_hidden = false;
        bool effect_enabled = false;
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
        std::string full_path;
        std::string version;
        std::string website;
        std::string issues;
        bool enabled = true;
    };

    struct DualScreenCapture
    {
        std::string before_base64;
        std::string after_base64;
        bool success = false;
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
        DualScreenCapture capture_both_screens_base64(int quality = 85);

        std::vector<EffectInfo> list_effects();
        std::vector<TechniqueInfo> list_techniques(bool enabled_only = false);
        bool set_technique_state(const std::string &technique_name, bool enabled);
        bool reorder_techniques(const std::vector<std::string> &technique_names);

        std::vector<UniformVariableInfo> list_uniform_variables(const std::string &effect_filter = "", bool enabled_only = true, bool include_system = false);
        bool set_uniform_variable(const std::string &effect_name, const std::string &var_name, const nlohmann::json &val, bool auto_save = true);
        bool reset_uniform_variable(const std::string &effect_name, const std::string &var_name, bool auto_save = true);

        nlohmann::json get_preprocessor_definitions(const std::string &effect_name = "");
        bool set_preprocessor_definition(const std::string &effect_name, const std::string &name, const std::string &value);

        bool save_current_preset();
        bool load_preset(const std::string &preset_path);
        std::string get_current_preset_path();

        bool set_performance_mode(bool enabled);
        bool get_performance_mode();

        bool reload_effects(const std::string &effect_name = "");

        bool set_effects_state(bool enabled);
        bool get_effects_state();

        bool set_overlay_state(bool open);

        std::vector<AddonInfo> list_addons();
        bool set_addon_state(const std::string &addon_name, bool enabled);
        nlohmann::json get_addon_config(const std::string &addon_name);
        bool set_addon_config(const std::string &addon_name, const std::string &key, const std::string &value);

        nlohmann::json get_all_config();
        std::string get_config(const std::string &section, const std::string &key);
        std::vector<std::string> get_config_array(const std::string &section, const std::string &key);
        bool set_config(const std::string &section, const std::string &key, const std::string &value);

        std::vector<std::string> get_recent_logs(size_t max_lines = 100, bool errors_only = false, const std::string &search_query = "");

        std::string get_process_name() const;
        std::string get_process_path() const;
        uint32_t get_process_id() const;

    private:
        ReShadeBridge() = default;
        ~ReShadeBridge();

        void update_device_info(reshade::api::effect_runtime *runtime);
        void process_task_queue(reshade::api::effect_runtime *runtime);

        mutable std::mutex m_runtime_mutex;
        reshade::api::effect_runtime *m_current_runtime = nullptr;

        mutable std::mutex m_stats_mutex;
        FrameStats m_stats;
        std::chrono::high_resolution_clock::time_point m_last_present_time = std::chrono::high_resolution_clock::now();

        std::mutex m_task_mutex;
        std::queue<std::function<void(reshade::api::effect_runtime*)>> m_task_queue;

        // Capture state management
        struct StagingBuffer
        {
            reshade::api::resource resource = { 0 };
            uint32_t width = 0;
            uint32_t height = 0;
            reshade::api::format format = reshade::api::format::unknown;
            bool copy_recorded = false;
        };

        bool ensure_staging_buffer(reshade::api::device *device, reshade::api::effect_runtime *runtime, reshade::api::resource_view rtv, StagingBuffer &staging);
        void record_staging_copy(reshade::api::device *device, reshade::api::effect_runtime *runtime, reshade::api::command_list *cmd_list, reshade::api::resource_view rtv, StagingBuffer &staging, reshade::api::resource_usage current_state);
        void cleanup_staging_buffers(reshade::api::device *device);

        std::mutex m_capture_mutex;
        uint32_t m_requested_stages = 0; // 1=before, 2=after, 4=overlay
        uint64_t m_capture_request_id = 0;
        uint64_t m_capture_completed_id = 0;

        StagingBuffer m_staging_before;
        StagingBuffer m_staging_after;
        StagingBuffer m_staging_overlay;

        std::vector<uint8_t> m_captured_before_pixels;
        std::vector<uint8_t> m_captured_after_pixels;
        std::vector<uint8_t> m_captured_overlay_pixels;
        uint32_t m_captured_width = 0;
        uint32_t m_captured_height = 0;
        std::condition_variable m_capture_cv;
    };
}
