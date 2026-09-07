#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "reshade_bridge.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "base64.hpp"

namespace shadepilot
{
    ReShadeBridge& ReShadeBridge::instance()
    {
        static ReShadeBridge s_instance;
        return s_instance;
    }

    void ReShadeBridge::on_init_effect_runtime(reshade::api::effect_runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(m_runtime_mutex);
        m_current_runtime = runtime;
        reshade::log::message(reshade::log::level::info, "[ShadePilot] Effect runtime initialized.");
    }

    void ReShadeBridge::on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(m_runtime_mutex);
        if (m_current_runtime == runtime)
            m_current_runtime = nullptr;
        reshade::log::message(reshade::log::level::info, "[ShadePilot] Effect runtime destroyed.");
    }

    static bool capture_frame_advanced(
        reshade::api::effect_runtime *runtime,
        reshade::api::command_list *cmd_list,
        reshade::api::resource_view rtv,
        reshade::api::resource_usage current_state,
        std::vector<uint8_t> &out_pixels,
        uint32_t &out_width,
        uint32_t &out_height)
    {
        if (runtime == nullptr) return false;
        reshade::api::device *device = runtime->get_device();
        if (device == nullptr) return false;

        reshade::api::resource target_resource = { 0 };
        if (rtv.handle != 0)
            target_resource = device->get_resource_from_view(rtv);
        if (target_resource.handle == 0)
            target_resource = runtime->get_current_back_buffer();
        if (target_resource.handle == 0)
            return false;

        const reshade::api::resource_desc desc = device->get_resource_desc(target_resource);
        if (desc.texture.width == 0 || desc.texture.height == 0)
            return false;

        out_width = desc.texture.width;
        out_height = desc.texture.height;

        // Convert sRGB / typeless to default typed (e.g. r8g8b8a8_unorm_srgb = 29 -> r8g8b8a8_unorm = 25)
        reshade::api::format intermediate_format = reshade::api::format_to_default_typed(desc.texture.format, 0);
        if (intermediate_format == reshade::api::format::unknown)
            intermediate_format = reshade::api::format::r8g8b8a8_unorm;

        reshade::api::resource staging_resource = { 0 };
        reshade::api::resource_desc staging_desc(
            desc.texture.width, desc.texture.height, 1, 1,
            intermediate_format,
            1,
            reshade::api::memory_heap::readback,
            reshade::api::resource_usage::copy_dest
        );

        if (!device->create_resource(staging_desc, nullptr, reshade::api::resource_usage::copy_dest, &staging_resource))
        {
            // Fallback: try default capture_screenshot
            out_pixels.resize(static_cast<size_t>(out_width) * out_height * 4);
            if (runtime->capture_screenshot(out_pixels.data()))
                return true;
            out_pixels.clear();
            return false;
        }

        reshade::api::command_queue *queue = runtime->get_command_queue();
        reshade::api::command_list *copy_cmd = cmd_list;
        if (copy_cmd == nullptr && queue != nullptr)
            copy_cmd = queue->get_immediate_command_list();

        if (copy_cmd == nullptr)
        {
            device->destroy_resource(staging_resource);
            return false;
        }

        copy_cmd->barrier(target_resource, current_state, reshade::api::resource_usage::copy_source);
        copy_cmd->copy_texture_region(target_resource, 0, nullptr, staging_resource, 0, nullptr);
        copy_cmd->barrier(target_resource, reshade::api::resource_usage::copy_source, current_state);

        if (queue != nullptr)
        {
            reshade::api::fence sync_fence = {};
            if (device->create_fence(0, reshade::api::fence_flags::none, &sync_fence))
            {
                queue->signal(sync_fence, 1);
                device->wait(sync_fence, 1);
                device->destroy_fence(sync_fence);
            }
            else
            {
                queue->wait_idle();
            }
        }

        bool success = false;
        reshade::api::subresource_data mapped_data = {};
        if (device->map_texture_region(staging_resource, 0, nullptr, reshade::api::map_access::read_only, &mapped_data))
        {
            out_pixels.resize(static_cast<size_t>(out_width) * out_height * 4);
            const auto *mapped_bytes = static_cast<const uint8_t *>(mapped_data.data);
            uint8_t *dst = out_pixels.data();

            for (size_t y = 0; y < out_height; ++y)
            {
                const uint8_t *src_row = mapped_bytes + y * mapped_data.row_pitch;
                uint8_t *dst_row = dst + y * out_width * 4;

                if (intermediate_format == reshade::api::format::b8g8r8a8_unorm ||
                    intermediate_format == reshade::api::format::b8g8r8a8_unorm_srgb)
                {
                    // BGRA -> RGBA
                    for (size_t x = 0; x < out_width; ++x)
                    {
                        dst_row[x * 4 + 0] = src_row[x * 4 + 2];
                        dst_row[x * 4 + 1] = src_row[x * 4 + 1];
                        dst_row[x * 4 + 2] = src_row[x * 4 + 0];
                        dst_row[x * 4 + 3] = src_row[x * 4 + 3];
                    }
                }
                else if (intermediate_format == reshade::api::format::b8g8r8x8_unorm ||
                         intermediate_format == reshade::api::format::b8g8r8x8_unorm_srgb)
                {
                    // BGRX -> RGBA
                    for (size_t x = 0; x < out_width; ++x)
                    {
                        dst_row[x * 4 + 0] = src_row[x * 4 + 2];
                        dst_row[x * 4 + 1] = src_row[x * 4 + 1];
                        dst_row[x * 4 + 2] = src_row[x * 4 + 0];
                        dst_row[x * 4 + 3] = 0xFF;
                    }
                }
                else if (intermediate_format == reshade::api::format::r8g8b8x8_unorm ||
                         intermediate_format == reshade::api::format::r8g8b8x8_unorm_srgb)
                {
                    for (size_t x = 0; x < out_width; ++x)
                    {
                        dst_row[x * 4 + 0] = src_row[x * 4 + 0];
                        dst_row[x * 4 + 1] = src_row[x * 4 + 1];
                        dst_row[x * 4 + 2] = src_row[x * 4 + 2];
                        dst_row[x * 4 + 3] = 0xFF;
                    }
                }
                else if (intermediate_format == reshade::api::format::r10g10b10a2_unorm ||
                         intermediate_format == reshade::api::format::b10g10r10a2_unorm)
                {
                    const auto offset_r = intermediate_format == reshade::api::format::b10g10r10a2_unorm ? 2 : 0;
                    const auto offset_b = intermediate_format == reshade::api::format::b10g10r10a2_unorm ? 0 : 2;
                    for (size_t x = 0; x < out_width; ++x)
                    {
                        const uint32_t rgba = *reinterpret_cast<const uint32_t *>(src_row + x * 4);
                        dst_row[x * 4 + offset_r] = ((rgba & 0x000003FFu) / 4) & 0xFF;
                        dst_row[x * 4 + 1]        = (((rgba & 0x000FFC00u) >> 10) / 4) & 0xFF;
                        dst_row[x * 4 + offset_b] = (((rgba & 0x3FF00000u) >> 20) / 4) & 0xFF;
                        dst_row[x * 4 + 3]        = 0xFF;
                    }
                }
                else
                {
                    // Handles r8g8b8a8_unorm and r8g8b8a8_unorm_srgb
                    std::memcpy(dst_row, src_row, out_width * 4);
                }
            }
            device->unmap_texture_region(staging_resource, 0);
            success = true;
        }

        device->destroy_resource(staging_resource);

        if (!success)
        {
            out_pixels.resize(static_cast<size_t>(out_width) * out_height * 4);
            if (runtime->capture_screenshot(out_pixels.data()))
                return true;
            out_pixels.clear();
            return false;
        }

        return true;
    }

    void ReShadeBridge::on_begin_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view)
    {
        std::lock_guard<std::mutex> lock(m_capture_mutex);
        if (m_capture_before_requested && runtime != nullptr)
        {
            uint32_t width = 0, height = 0;
            if (capture_frame_advanced(runtime, cmd_list, rtv, reshade::api::resource_usage::render_target, m_captured_before_pixels, width, height))
            {
                m_captured_width = width;
                m_captured_height = height;
            }
            else
            {
                m_captured_before_pixels.clear();
            }
            m_capture_before_requested = false;
            m_capture_cv.notify_all();
        }
    }

    void ReShadeBridge::on_finish_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view)
    {
        std::lock_guard<std::mutex> lock(m_capture_mutex);
        if (m_capture_after_requested && runtime != nullptr)
        {
            uint32_t width = 0, height = 0;
            if (capture_frame_advanced(runtime, cmd_list, rtv, reshade::api::resource_usage::render_target, m_captured_after_pixels, width, height))
            {
                m_captured_width = width;
                m_captured_height = height;
            }
            else
            {
                m_captured_after_pixels.clear();
            }
            m_capture_after_requested = false;
            m_capture_cv.notify_all();
        }
    }

    void ReShadeBridge::on_present(reshade::api::effect_runtime *runtime)
    {
        // Update stats
        {
            const auto now = std::chrono::high_resolution_clock::now();
            const std::chrono::duration<float, std::milli> delta = now - m_last_present_time;
            m_last_present_time = now;

            std::lock_guard<std::mutex> lock(m_stats_mutex);
            m_stats.frame_count++;
            m_stats.frame_time_ms = delta.count();
            if (m_stats.frame_time_ms > 0.0001f)
                m_stats.fps = 1000.0f / m_stats.frame_time_ms;

            if (runtime != nullptr)
            {
                uint32_t w = 0, h = 0;
                runtime->get_screenshot_width_and_height(&w, &h);
                m_stats.width = w;
                m_stats.height = h;

                reshade::api::device *dev = runtime->get_device();
                if (dev != nullptr)
                {
                    switch (dev->get_api())
                    {
                    case reshade::api::device_api::d3d9: m_stats.api_name = "Direct3D 9"; break;
                    case reshade::api::device_api::d3d10: m_stats.api_name = "Direct3D 10"; break;
                    case reshade::api::device_api::d3d11: m_stats.api_name = "Direct3D 11"; break;
                    case reshade::api::device_api::d3d12: m_stats.api_name = "Direct3D 12"; break;
                    case reshade::api::device_api::opengl: m_stats.api_name = "OpenGL"; break;
                    case reshade::api::device_api::vulkan: m_stats.api_name = "Vulkan"; break;
                    default: m_stats.api_name = "Unknown"; break;
                    }
                }
            }
        }

        // Process scheduled tasks on render thread
        process_task_queue(runtime);
    }

    void ReShadeBridge::process_task_queue(reshade::api::effect_runtime *runtime)
    {
        std::queue<std::function<void(reshade::api::effect_runtime*)>> tasks;
        {
            std::lock_guard<std::mutex> lock(m_task_mutex);
            tasks.swap(m_task_queue);
        }

        while (!tasks.empty())
        {
            tasks.front()(runtime);
            tasks.pop();
        }
    }

    bool ReShadeBridge::is_runtime_active() const
    {
        std::lock_guard<std::mutex> lock(m_runtime_mutex);
        return m_current_runtime != nullptr;
    }

    FrameStats ReShadeBridge::get_stats() const
    {
        std::lock_guard<std::mutex> lock(m_stats_mutex);
        return m_stats;
    }

    std::string ReShadeBridge::capture_screen_base64(const std::string &stage, int quality)
    {
        std::vector<uint8_t> pixels;
        uint32_t width = 0, height = 0;

        {
            std::unique_lock<std::mutex> lock(m_capture_mutex);
            if (stage == "before")
                m_capture_before_requested = true;
            else
                m_capture_after_requested = true;

            const bool finished = m_capture_cv.wait_for(lock, std::chrono::milliseconds(2500), [&]() {
                if (stage == "before")
                    return !m_capture_before_requested;
                else
                    return !m_capture_after_requested;
            });

            if (!finished)
            {
                m_capture_before_requested = false;
                m_capture_after_requested = false;
                return "";
            }

            if (stage == "before")
                pixels = std::move(m_captured_before_pixels);
            else
                pixels = std::move(m_captured_after_pixels);

            width = m_captured_width;
            height = m_captured_height;
        }

        if (pixels.empty() || width == 0 || height == 0)
            return "";

        // Compress RGBA to JPEG in memory
        std::vector<uint8_t> jpeg_buffer;
        auto write_callback = [](void *context, void *data, int size) {
            auto *buf = static_cast<std::vector<uint8_t>*>(context);
            const auto *bytes = static_cast<const uint8_t*>(data);
            buf->insert(buf->end(), bytes, bytes + size);
        };

        if (!stbi_write_jpg_to_func(write_callback, &jpeg_buffer, width, height, 4, pixels.data(), quality))
            return "";

        return base64::encode(jpeg_buffer);
    }

    std::vector<TechniqueInfo> ReShadeBridge::list_techniques(bool enabled_only)
    {
        auto fut = execute_on_render_thread([enabled_only](reshade::api::effect_runtime *runtime) -> std::vector<TechniqueInfo> {
            std::vector<TechniqueInfo> list;
            if (runtime == nullptr) return list;

            runtime->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime *rt, reshade::api::effect_technique tech) {
                const bool enabled = rt->get_technique_state(tech);
                if (enabled_only && !enabled)
                    return;

                TechniqueInfo info;
                info.enabled = enabled;

                char name[256] = {};
                rt->get_technique_name(tech, name);
                info.name = name;

                char effect[256] = {};
                rt->get_technique_effect_name(tech, effect);
                info.effect_name = effect;

                char label[256] = {};
                if (rt->get_annotation_string_from_technique(tech, "ui_label", label))
                    info.label = label;
                else
                    info.label = info.name;

                char tooltip[512] = {};
                if (rt->get_annotation_string_from_technique(tech, "ui_tooltip", tooltip))
                    info.tooltip = tooltip;

                list.push_back(std::move(info));
            });

            return list;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return {};
    }

    bool ReShadeBridge::set_technique_state(const std::string &technique_name, bool enabled)
    {
        auto fut = execute_on_render_thread([technique_name, enabled](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime == nullptr) return false;

            reshade::api::effect_technique tech = runtime->find_technique(nullptr, technique_name.c_str());
            if (tech.handle == 0) return false;

            runtime->set_technique_state(tech, enabled);
            return true;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return false;
    }

    bool ReShadeBridge::reorder_techniques(const std::vector<std::string> &technique_names)
    {
        auto fut = execute_on_render_thread([technique_names](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime == nullptr) return false;

            std::vector<reshade::api::effect_technique> handles;
            handles.reserve(technique_names.size());
            for (const auto &name : technique_names)
            {
                reshade::api::effect_technique tech = runtime->find_technique(nullptr, name.c_str());
                if (tech.handle != 0)
                    handles.push_back(tech);
            }

            if (!handles.empty())
            {
                runtime->reorder_techniques(handles.size(), handles.data());
                return true;
            }
            return false;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return false;
    }

    std::vector<UniformVariableInfo> ReShadeBridge::list_uniform_variables(const std::string &effect_filter)
    {
        auto fut = execute_on_render_thread([effect_filter](reshade::api::effect_runtime *runtime) -> std::vector<UniformVariableInfo> {
            std::vector<UniformVariableInfo> list;
            if (runtime == nullptr) return list;

            const char *filter = effect_filter.empty() ? nullptr : effect_filter.c_str();

            runtime->enumerate_uniform_variables(filter, [&](reshade::api::effect_runtime *rt, reshade::api::effect_uniform_variable var) {
                UniformVariableInfo info;

                char name[256] = {};
                rt->get_uniform_variable_name(var, name);
                info.name = name;

                char effect[256] = {};
                rt->get_uniform_variable_effect_name(var, effect);
                info.effect_name = effect;

                reshade::api::format base_type = reshade::api::format::unknown;
                rt->get_uniform_variable_type(var, &base_type, &info.rows, &info.columns, &info.array_length);

                const size_t total_elements = (info.rows ? info.rows : 1) * (info.columns ? info.columns : 1);

                switch (base_type)
                {
                case reshade::api::format::r32_float:
                    info.type = "float";
                    if (total_elements == 1) {
                        float v = 0; rt->get_uniform_value_float(var, &v, 1);
                        info.current_value = v;
                    } else {
                        std::vector<float> vals(total_elements);
                        rt->get_uniform_value_float(var, vals.data(), total_elements);
                        info.current_value = vals;
                    }
                    break;
                case reshade::api::format::r32_sint:
                    info.type = "int";
                    if (total_elements == 1) {
                        int32_t v = 0; rt->get_uniform_value_int(var, &v, 1);
                        info.current_value = v;
                    } else {
                        std::vector<int32_t> vals(total_elements);
                        rt->get_uniform_value_int(var, vals.data(), total_elements);
                        info.current_value = vals;
                    }
                    break;
                case reshade::api::format::r32_uint:
                    info.type = "uint";
                    if (total_elements == 1) {
                        uint32_t v = 0; rt->get_uniform_value_uint(var, &v, 1);
                        info.current_value = v;
                    } else {
                        std::vector<uint32_t> vals(total_elements);
                        rt->get_uniform_value_uint(var, vals.data(), total_elements);
                        info.current_value = vals;
                    }
                    break;
                case reshade::api::format::r32_typeless:
                    info.type = "bool";
                    if (total_elements == 1) {
                        bool v = false; rt->get_uniform_value_bool(var, &v, 1);
                        info.current_value = v;
                    } else {
                        std::vector<bool> vals(total_elements);
                        std::vector<int> temp(total_elements);
                        rt->get_uniform_value_bool(var, reinterpret_cast<bool*>(temp.data()), total_elements);
                        for (size_t i = 0; i < total_elements; ++i) vals[i] = (temp[i] != 0);
                        info.current_value = vals;
                    }
                    break;
                default:
                    info.type = "other";
                    break;
                }

                char label[256] = {};
                if (rt->get_annotation_string_from_uniform_variable(var, "ui_label", label))
                    info.label = label;
                else
                    info.label = info.name;

                char tooltip[512] = {};
                if (rt->get_annotation_string_from_uniform_variable(var, "ui_tooltip", tooltip))
                    info.tooltip = tooltip;

                char ui_type[64] = {};
                if (rt->get_annotation_string_from_uniform_variable(var, "ui_type", ui_type))
                    info.ui_type = ui_type;

                float min_f = 0, max_f = 0, step_f = 0;
                if (rt->get_annotation_float_from_uniform_variable(var, "ui_min", &min_f, 1))
                    info.min_value = min_f;
                if (rt->get_annotation_float_from_uniform_variable(var, "ui_max", &max_f, 1))
                    info.max_value = max_f;
                if (rt->get_annotation_float_from_uniform_variable(var, "ui_step", &step_f, 1))
                    info.step_value = step_f;

                char ui_items[1024] = {};
                if (rt->get_annotation_string_from_uniform_variable(var, "ui_items", ui_items))
                {
                    std::stringstream ss(ui_items);
                    std::string item;
                    while (std::getline(ss, item, '\0'))
                        if (!item.empty()) info.ui_items.push_back(item);
                }

                list.push_back(std::move(info));
            });

            return list;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return {};
    }

    bool ReShadeBridge::set_uniform_variable(const std::string &effect_name, const std::string &var_name, const nlohmann::json &val)
    {
        auto fut = execute_on_render_thread([effect_name, var_name, val](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime == nullptr) return false;

            const char *eff = effect_name.empty() ? nullptr : effect_name.c_str();
            reshade::api::effect_uniform_variable var = runtime->find_uniform_variable(eff, var_name.c_str());
            if (var.handle == 0) return false;

            reshade::api::format base_type = reshade::api::format::unknown;
            uint32_t rows = 0, cols = 0, len = 0;
            runtime->get_uniform_variable_type(var, &base_type, &rows, &cols, &len);

            const size_t total = (rows ? rows : 1) * (cols ? cols : 1);

            if (base_type == reshade::api::format::r32_float)
            {
                if (val.is_array())
                {
                    std::vector<float> data(total, 0.0f);
                    for (size_t i = 0; i < (std::min)(total, val.size()); ++i)
                        data[i] = val[i].get<float>();
                    runtime->set_uniform_value_float(var, data.data(), total);
                }
                else if (val.is_number())
                {
                    float f = val.get<float>();
                    runtime->set_uniform_value_float(var, &f, 1);
                }
                return true;
            }
            else if (base_type == reshade::api::format::r32_sint)
            {
                if (val.is_array())
                {
                    std::vector<int32_t> data(total, 0);
                    for (size_t i = 0; i < (std::min)(total, val.size()); ++i)
                        data[i] = val[i].get<int32_t>();
                    runtime->set_uniform_value_int(var, data.data(), total);
                }
                else if (val.is_number_integer())
                {
                    int32_t v = val.get<int32_t>();
                    runtime->set_uniform_value_int(var, &v, 1);
                }
                return true;
            }
            else if (base_type == reshade::api::format::r32_typeless || base_type == reshade::api::format::r32_uint)
            {
                if (val.is_boolean())
                {
                    bool b = val.get<bool>();
                    runtime->set_uniform_value_bool(var, &b, 1);
                    return true;
                }
                else if (val.is_number())
                {
                    uint32_t u = val.get<uint32_t>();
                    runtime->set_uniform_value_uint(var, &u, 1);
                    return true;
                }
            }

            return false;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return false;
    }

    bool ReShadeBridge::reset_uniform_variable(const std::string &effect_name, const std::string &var_name)
    {
        auto fut = execute_on_render_thread([effect_name, var_name](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime == nullptr) return false;

            const char *eff = effect_name.empty() ? nullptr : effect_name.c_str();
            reshade::api::effect_uniform_variable var = runtime->find_uniform_variable(eff, var_name.c_str());
            if (var.handle == 0) return false;

            runtime->reset_uniform_value(var);
            return true;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return false;
    }

    nlohmann::json ReShadeBridge::get_preprocessor_definitions(const std::string &effect_name)
    {
        auto fut = execute_on_render_thread([effect_name](reshade::api::effect_runtime *runtime) -> nlohmann::json {
            nlohmann::json res = nlohmann::json::object();
            if (runtime == nullptr) return res;

            // Common preprocessor macros in popular shaders
            static const char *kKnownDefs[] = {
                "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN",
                "RESHADE_DEPTH_INPUT_IS_REVERSED",
                "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC",
                "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE",
                "LUT_SAMPLING_MODE",
                "ENHANCED_LUT_QUALITY",
                "ENABLE_TRANSPARENCY_FIX"
            };

            for (const char *def : kKnownDefs)
            {
                char val[256] = {};
                size_t sz = sizeof(val);
                if (runtime->get_preprocessor_definition_for_effect(effect_name.c_str(), def, val, &sz))
                    res[def] = std::string(val);
            }

            return res;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return nlohmann::json::object();
    }

    bool ReShadeBridge::set_preprocessor_definition(const std::string &effect_name, const std::string &name, const std::string &value)
    {
        auto fut = execute_on_render_thread([effect_name, name, value](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime == nullptr) return false;

            runtime->set_preprocessor_definition_for_effect(effect_name.c_str(), name.c_str(), value.c_str());
            runtime->reload_effect_next_frame(effect_name.empty() ? nullptr : effect_name.c_str());
            return true;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return false;
    }

    bool ReShadeBridge::save_current_preset()
    {
        auto fut = execute_on_render_thread([](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime == nullptr) return false;
            runtime->save_current_preset();
            return true;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return false;
    }

    std::vector<AddonInfo> ReShadeBridge::list_addons()
    {
        std::vector<AddonInfo> list;

        char base_path_buf[MAX_PATH] = {};
        size_t base_path_size = sizeof(base_path_buf);
        reshade::get_reshade_base_path(base_path_buf, &base_path_size);
        std::filesystem::path base_path(base_path_buf);

        // Read disabled addons
        char disabled_buf[4096] = {};
        size_t disabled_size = sizeof(disabled_buf);
        std::vector<std::string> disabled_list;
        if (reshade::get_config_value(nullptr, "ADDON", "DisabledAddons", disabled_buf, &disabled_size))
        {
            size_t offset = 0;
            while (offset < disabled_size && disabled_buf[offset] != '\0')
            {
                std::string item(disabled_buf + offset);
                disabled_list.push_back(item);
                offset += item.size() + 1;
            }
        }

        // Iterate addon directory
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(base_path, ec))
        {
            if (!entry.is_regular_file()) continue;
            const auto ext = entry.path().extension();
            if (ext == ".addon" || ext == ".addon64" || ext == ".addon32")
            {
                AddonInfo info;
                info.file = entry.path().filename().u8string();
                info.name = entry.path().stem().u8string();
                info.enabled = true;

                for (const auto &d : disabled_list)
                {
                    if (d.find(info.file) != std::string::npos || d.find(info.name) != std::string::npos)
                    {
                        info.enabled = false;
                        break;
                    }
                }

                HMODULE mod = LoadLibraryExW(entry.path().c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
                if (mod)
                {
                    if (const char *const *pName = reinterpret_cast<const char *const *>(GetProcAddress(mod, "NAME")))
                        info.name = *pName;
                    if (const char *const *pDesc = reinterpret_cast<const char *const *>(GetProcAddress(mod, "DESCRIPTION")))
                        info.description = *pDesc;
                    if (const char *const *pAuth = reinterpret_cast<const char *const *>(GetProcAddress(mod, "AUTHOR")))
                        info.author = *pAuth;
                    FreeLibrary(mod);
                }

                list.push_back(std::move(info));
            }
        }

        return list;
    }

    bool ReShadeBridge::set_addon_state(const std::string &addon_name, bool enabled)
    {
        char disabled_buf[4096] = {};
        size_t disabled_size = sizeof(disabled_buf);
        std::vector<std::string> disabled_list;
        if (reshade::get_config_value(nullptr, "ADDON", "DisabledAddons", disabled_buf, &disabled_size))
        {
            size_t offset = 0;
            while (offset < disabled_size && disabled_buf[offset] != '\0')
            {
                std::string item(disabled_buf + offset);
                disabled_list.push_back(item);
                offset += item.size() + 1;
            }
        }

        if (enabled)
        {
            disabled_list.erase(std::remove_if(disabled_list.begin(), disabled_list.end(), [&](const std::string &item) {
                return item.find(addon_name) != std::string::npos;
            }), disabled_list.end());
        }
        else
        {
            bool exists = false;
            for (const auto &item : disabled_list)
                if (item.find(addon_name) != std::string::npos) { exists = true; break; }
            if (!exists)
                disabled_list.push_back(addon_name);
        }

        std::string packed;
        for (const auto &item : disabled_list)
        {
            packed += item;
            packed += '\0';
        }

        reshade::set_config_value(nullptr, "ADDON", "DisabledAddons", packed.data(), packed.size());
        return true;
    }

    std::string ReShadeBridge::get_config(const std::string &section, const std::string &key)
    {
        char val[1024] = {};
        size_t sz = sizeof(val);
        if (reshade::get_config_value(nullptr, section.c_str(), key.c_str(), val, &sz))
            return std::string(val);
        return "";
    }

    bool ReShadeBridge::set_config(const std::string &section, const std::string &key, const std::string &value)
    {
        reshade::set_config_value(nullptr, section.c_str(), key.c_str(), value.c_str());
        return true;
    }

    std::vector<std::string> ReShadeBridge::get_recent_logs(size_t max_lines, bool errors_only)
    {
        std::vector<std::string> lines;

        char base_path_buf[MAX_PATH] = {};
        size_t base_path_size = sizeof(base_path_buf);
        reshade::get_reshade_base_path(base_path_buf, &base_path_size);

        std::filesystem::path log_path = std::filesystem::path(base_path_buf) / "ReShade.log";
        std::ifstream file(log_path);
        if (!file.is_open())
            return lines;

        std::string line;
        while (std::getline(file, line))
        {
            if (errors_only)
            {
                if (line.find("ERROR |") != std::string::npos || line.find("WARN  |") != std::string::npos)
                    lines.push_back(line);
            }
            else
            {
                lines.push_back(line);
            }
        }

        if (lines.size() > max_lines)
            lines.erase(lines.begin(), lines.end() - max_lines);

        return lines;
    }
}

