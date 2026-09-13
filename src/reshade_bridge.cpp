#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winver.h>
#include "reshade_bridge.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <unordered_set>
#include <cctype>

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

    ReShadeBridge::~ReShadeBridge()
    {
        std::lock_guard<std::mutex> lock(m_runtime_mutex);
        if (m_current_runtime != nullptr)
        {
            cleanup_staging_buffers(m_current_runtime->get_device());
        }
    }

    void ReShadeBridge::on_init_effect_runtime(reshade::api::effect_runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(m_runtime_mutex);
        m_current_runtime = runtime;
        update_device_info(runtime);
        reshade::log::message(reshade::log::level::info, "[ShadePilot] Effect runtime initialized.");
    }

    void ReShadeBridge::on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(m_runtime_mutex);
        if (m_current_runtime == runtime)
        {
            if (runtime != nullptr)
            {
                cleanup_staging_buffers(runtime->get_device());
            }
            m_current_runtime = nullptr;
        }
        reshade::log::message(reshade::log::level::info, "[ShadePilot] Effect runtime destroyed.");
    }

    void ReShadeBridge::cleanup_staging_buffers(reshade::api::device *device)
    {
        std::lock_guard<std::mutex> lock(m_capture_mutex);
        if (device != nullptr)
        {
            if (m_staging_before.resource.handle != 0)
            {
                device->destroy_resource(m_staging_before.resource);
                m_staging_before = {};
            }
            if (m_staging_after.resource.handle != 0)
            {
                device->destroy_resource(m_staging_after.resource);
                m_staging_after = {};
            }
            if (m_staging_overlay.resource.handle != 0)
            {
                device->destroy_resource(m_staging_overlay.resource);
                m_staging_overlay = {};
            }
        }
    }

    bool ReShadeBridge::ensure_staging_buffer(
        reshade::api::device *device,
        reshade::api::effect_runtime *runtime,
        reshade::api::resource_view rtv,
        StagingBuffer &staging)
    {
        if (device == nullptr || runtime == nullptr)
            return false;

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

        reshade::api::format intermediate_format = reshade::api::format_to_default_typed(desc.texture.format, 0);
        if (intermediate_format == reshade::api::format::unknown)
            intermediate_format = reshade::api::format::r8g8b8a8_unorm;

        if (staging.resource.handle != 0 &&
            (staging.width != desc.texture.width || staging.height != desc.texture.height || staging.format != intermediate_format))
        {
            device->destroy_resource(staging.resource);
            staging = {};
        }

        if (staging.resource.handle == 0)
        {
            reshade::api::resource_desc staging_desc(
                desc.texture.width, desc.texture.height, 1, 1,
                intermediate_format, 1,
                reshade::api::memory_heap::readback,
                reshade::api::resource_usage::copy_dest
            );

            if (!device->create_resource(staging_desc, nullptr, reshade::api::resource_usage::copy_dest, &staging.resource))
                return false;

            staging.width = desc.texture.width;
            staging.height = desc.texture.height;
            staging.format = intermediate_format;
        }

        return true;
    }

    void ReShadeBridge::record_staging_copy(
        reshade::api::device *device,
        reshade::api::effect_runtime *runtime,
        reshade::api::command_list *cmd_list,
        reshade::api::resource_view rtv,
        StagingBuffer &staging,
        reshade::api::resource_usage current_state)
    {
        if (device == nullptr || runtime == nullptr || cmd_list == nullptr || staging.resource.handle == 0)
            return;

        reshade::api::resource target_resource = { 0 };
        if (rtv.handle != 0)
            target_resource = device->get_resource_from_view(rtv);
        if (target_resource.handle == 0)
            target_resource = runtime->get_current_back_buffer();
        if (target_resource.handle == 0)
            return;

        cmd_list->barrier(target_resource, current_state, reshade::api::resource_usage::copy_source);
        cmd_list->copy_texture_region(target_resource, 0, nullptr, staging.resource, 0, nullptr);
        cmd_list->barrier(target_resource, reshade::api::resource_usage::copy_source, current_state);

        staging.copy_recorded = true;
    }

    static bool convert_mapped_to_rgba(
        const uint8_t *mapped_bytes,
        uint32_t row_pitch,
        uint32_t width,
        uint32_t height,
        reshade::api::format intermediate_format,
        std::vector<uint8_t> &out_pixels)
    {
        out_pixels.resize(static_cast<size_t>(width) * height * 4);
        uint8_t *dst = out_pixels.data();

        for (size_t y = 0; y < height; ++y)
        {
            const uint8_t *src_row = mapped_bytes + y * row_pitch;
            uint8_t *dst_row = dst + y * width * 4;

            if (intermediate_format == reshade::api::format::b8g8r8a8_unorm ||
                intermediate_format == reshade::api::format::b8g8r8a8_unorm_srgb)
            {
                for (size_t x = 0; x < width; ++x)
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
                for (size_t x = 0; x < width; ++x)
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
                for (size_t x = 0; x < width; ++x)
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
                for (size_t x = 0; x < width; ++x)
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
                std::memcpy(dst_row, src_row, width * 4);
            }
        }
        return true;
    }

    void ReShadeBridge::on_begin_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view)
    {
        std::lock_guard<std::mutex> lock(m_capture_mutex);
        if ((m_requested_stages & 1) && runtime != nullptr && cmd_list != nullptr)
        {
            auto *device = runtime->get_device();
            if (ensure_staging_buffer(device, runtime, rtv, m_staging_before))
            {
                record_staging_copy(device, runtime, cmd_list, rtv, m_staging_before, reshade::api::resource_usage::render_target);
            }
        }
    }

    void ReShadeBridge::on_finish_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view)
    {
        std::lock_guard<std::mutex> lock(m_capture_mutex);
        if ((m_requested_stages & 2) && runtime != nullptr && cmd_list != nullptr)
        {
            auto *device = runtime->get_device();
            if (ensure_staging_buffer(device, runtime, rtv, m_staging_after))
            {
                record_staging_copy(device, runtime, cmd_list, rtv, m_staging_after, reshade::api::resource_usage::render_target);
            }
        }
    }

    void ReShadeBridge::update_device_info(reshade::api::effect_runtime *runtime)
    {
        if (runtime == nullptr)
            return;

        reshade::api::device *dev = runtime->get_device();
        if (dev == nullptr)
            return;

        std::lock_guard<std::mutex> lock(m_stats_mutex);
        switch (dev->get_api())
        {
        case reshade::api::device_api::d3d9:
            m_stats.api_name = "Direct3D 9";
            m_stats.pipeline_name = "DX9";
            break;
        case reshade::api::device_api::d3d10:
            m_stats.api_name = "Direct3D 10";
            m_stats.pipeline_name = "DX10";
            break;
        case reshade::api::device_api::d3d11:
            m_stats.api_name = "Direct3D 11";
            m_stats.pipeline_name = "DX11";
            break;
        case reshade::api::device_api::d3d12:
            m_stats.api_name = "Direct3D 12";
            m_stats.pipeline_name = "DX12";
            break;
        case reshade::api::device_api::opengl:
            m_stats.api_name = "OpenGL";
            m_stats.pipeline_name = "OpenGL";
            break;
        case reshade::api::device_api::vulkan:
            m_stats.api_name = "Vulkan";
            m_stats.pipeline_name = "Vulkan";
            break;
        default:
            m_stats.api_name = "Unknown";
            m_stats.pipeline_name = "Unknown";
            break;
        }

        char desc_buf[256] = {};
        if (dev->get_property(reshade::api::device_properties::description, desc_buf))
        {
            m_stats.device_name = desc_buf;
        }

        uint32_t vid = 0, did = 0;
        if (dev->get_property(reshade::api::device_properties::vendor_id, &vid))
            m_stats.vendor_id = vid;
        if (dev->get_property(reshade::api::device_properties::device_id, &did))
            m_stats.device_id = did;
    }

    void ReShadeBridge::on_present(reshade::api::effect_runtime *runtime)
    {
        // 1. Update stats
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

                m_stats.effects_enabled = runtime->get_effects_state();

                char preset_buf[1024] = {};
                size_t preset_sz = sizeof(preset_buf);
                runtime->get_current_preset_path(preset_buf, &preset_sz);
                m_stats.current_preset = preset_buf;

                bool perf = false;
                if (reshade::get_config_value(runtime, "GENERAL", "PerformanceMode", perf))
                    m_stats.performance_mode = perf;

                size_t total_techs = 0;
                size_t enabled_techs = 0;
                std::vector<std::string> enabled_names;
                runtime->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime *rt, reshade::api::effect_technique tech) {
                    total_techs++;
                    if (rt->get_technique_state(tech))
                    {
                        enabled_techs++;
                        char tname[256] = {};
                        rt->get_technique_name(tech, tname);
                        enabled_names.push_back(tname);
                    }
                });
                m_stats.total_techniques = total_techs;
                m_stats.enabled_techniques = enabled_techs;
                m_stats.enabled_technique_names = std::move(enabled_names);
            }
        }

        update_device_info(runtime);

        // 2. Overlay capture & safe frame readback synchronization at end of frame
        {
            std::lock_guard<std::mutex> lock(m_capture_mutex);

            if (runtime != nullptr)
            {
                auto *device = runtime->get_device();
                auto *queue = runtime->get_command_queue();

                // If overlay frame requested, copy current presented back buffer
                if ((m_requested_stages & 4) && device != nullptr && queue != nullptr)
                {
                    if (ensure_staging_buffer(device, runtime, { 0 }, m_staging_overlay))
                    {
                        record_staging_copy(device, runtime, queue->get_immediate_command_list(), { 0 }, m_staging_overlay, reshade::api::resource_usage::present);
                    }
                }

                // Check if any staging copy was recorded this frame
                const bool has_recorded = m_staging_before.copy_recorded || m_staging_after.copy_recorded || m_staging_overlay.copy_recorded;

                if (has_recorded && device != nullptr && queue != nullptr)
                {
                    // Safe synchronization at end of frame: no render pass or command recording is active!
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

                    // Readback Before frame
                    if (m_staging_before.copy_recorded)
                    {
                        reshade::api::subresource_data mapped = {};
                        if (device->map_texture_region(m_staging_before.resource, 0, nullptr, reshade::api::map_access::read_only, &mapped))
                        {
                            convert_mapped_to_rgba(static_cast<const uint8_t*>(mapped.data), mapped.row_pitch, m_staging_before.width, m_staging_before.height, m_staging_before.format, m_captured_before_pixels);
                            device->unmap_texture_region(m_staging_before.resource, 0);
                            m_captured_width = m_staging_before.width;
                            m_captured_height = m_staging_before.height;
                        }
                        m_staging_before.copy_recorded = false;
                    }

                    // Readback After frame
                    if (m_staging_after.copy_recorded)
                    {
                        reshade::api::subresource_data mapped = {};
                        if (device->map_texture_region(m_staging_after.resource, 0, nullptr, reshade::api::map_access::read_only, &mapped))
                        {
                            convert_mapped_to_rgba(static_cast<const uint8_t*>(mapped.data), mapped.row_pitch, m_staging_after.width, m_staging_after.height, m_staging_after.format, m_captured_after_pixels);
                            device->unmap_texture_region(m_staging_after.resource, 0);
                            m_captured_width = m_staging_after.width;
                            m_captured_height = m_staging_after.height;
                        }
                        m_staging_after.copy_recorded = false;
                    }

                    // Readback Overlay frame
                    if (m_staging_overlay.copy_recorded)
                    {
                        reshade::api::subresource_data mapped = {};
                        if (device->map_texture_region(m_staging_overlay.resource, 0, nullptr, reshade::api::map_access::read_only, &mapped))
                        {
                            convert_mapped_to_rgba(static_cast<const uint8_t*>(mapped.data), mapped.row_pitch, m_staging_overlay.width, m_staging_overlay.height, m_staging_overlay.format, m_captured_overlay_pixels);
                            device->unmap_texture_region(m_staging_overlay.resource, 0);
                            m_captured_width = m_staging_overlay.width;
                            m_captured_height = m_staging_overlay.height;
                        }
                        m_staging_overlay.copy_recorded = false;
                    }

                    m_requested_stages = 0;
                    m_capture_completed_id++;
                    m_capture_cv.notify_all();
                }
            }
        }

        // 3. Process scheduled tasks on render thread
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
            uint32_t mask = 2; // default after
            if (stage == "before")
                mask = 1;
            else if (stage == "overlay" || stage == "ui" || stage == "present")
                mask = 4;

            m_requested_stages |= mask;
            const uint64_t start_completed_id = m_capture_completed_id;

            const bool finished = m_capture_cv.wait_for(lock, std::chrono::milliseconds(2500), [&]() {
                return m_capture_completed_id > start_completed_id;
            });

            if (!finished)
            {
                m_requested_stages &= ~mask;
                return "";
            }

            if (stage == "before")
                pixels = m_captured_before_pixels;
            else if (stage == "overlay" || stage == "ui" || stage == "present")
                pixels = m_captured_overlay_pixels;
            else
                pixels = m_captured_after_pixels;

            width = m_captured_width;
            height = m_captured_height;
        }

        if (pixels.empty() || width == 0 || height == 0)
            return "";

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

    DualScreenCapture ReShadeBridge::capture_both_screens_base64(int quality)
    {
        DualScreenCapture res;
        std::vector<uint8_t> before_pixels;
        std::vector<uint8_t> after_pixels;
        uint32_t width = 0, height = 0;

        {
            std::unique_lock<std::mutex> lock(m_capture_mutex);
            // Request both before (1) and after (2) in the SAME frame!
            m_requested_stages |= (1 | 2);
            const uint64_t start_completed_id = m_capture_completed_id;

            const bool finished = m_capture_cv.wait_for(lock, std::chrono::milliseconds(2500), [&]() {
                return m_capture_completed_id > start_completed_id;
            });

            if (!finished)
            {
                m_requested_stages &= ~(1 | 2);
                return res;
            }

            before_pixels = m_captured_before_pixels;
            after_pixels = m_captured_after_pixels;
            width = m_captured_width;
            height = m_captured_height;
        }

        if (before_pixels.empty() || after_pixels.empty() || width == 0 || height == 0)
            return res;

        auto write_callback = [](void *context, void *data, int size) {
            auto *buf = static_cast<std::vector<uint8_t>*>(context);
            const auto *bytes = static_cast<const uint8_t*>(data);
            buf->insert(buf->end(), bytes, bytes + size);
        };

        std::vector<uint8_t> before_jpg;
        if (stbi_write_jpg_to_func(write_callback, &before_jpg, width, height, 4, before_pixels.data(), quality))
            res.before_base64 = base64::encode(before_jpg);

        std::vector<uint8_t> after_jpg;
        if (stbi_write_jpg_to_func(write_callback, &after_jpg, width, height, 4, after_pixels.data(), quality))
            res.after_base64 = base64::encode(after_jpg);

        res.success = (!res.before_base64.empty() && !res.after_base64.empty());
        return res;
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

                bool hidden = false;
                rt->get_annotation_bool_from_technique(tech, "hidden", &hidden, 1);
                info.hidden = hidden;

                list.push_back(std::move(info));
            });

            return list;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return {};
    }

    std::vector<EffectInfo> ReShadeBridge::list_effects()
    {
        auto fut = execute_on_render_thread([this](reshade::api::effect_runtime *runtime) -> std::vector<EffectInfo> {
            std::vector<EffectInfo> effects;
            if (runtime == nullptr) return effects;

            // 1. Gather all effect search paths
            std::vector<std::filesystem::path> search_paths;
            char base_buf[MAX_PATH] = {};
            size_t base_sz = sizeof(base_buf);
            reshade::get_reshade_base_path(base_buf, &base_sz);
            const std::filesystem::path base_path(base_buf);

            char paths_buf[4096] = {};
            size_t paths_sz = sizeof(paths_buf);
            if (reshade::get_config_value(runtime, "GENERAL", "EffectSearchPaths", paths_buf, &paths_sz))
            {
                size_t offset = 0;
                while (offset < paths_sz && paths_buf[offset] != '\0')
                {
                    std::string p(paths_buf + offset);
                    if (!p.empty())
                    {
                        std::filesystem::path sp(p);
                        if (sp.is_relative()) sp = base_path / sp;
                        search_paths.push_back(sp);
                    }
                    offset += p.size() + 1;
                }
            }
            if (search_paths.empty())
            {
                search_paths.push_back(base_path);
                search_paths.push_back(base_path / "reshade-shaders" / "Shaders");
            }

            // 2. Enumerate techniques to count techniques per effect
            std::unordered_map<std::string, size_t> tech_counts;
            std::unordered_map<std::string, size_t> enabled_counts;
            runtime->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime *rt, reshade::api::effect_technique tech) {
                char eff[256] = {};
                rt->get_technique_effect_name(tech, eff);
                tech_counts[eff]++;
                if (rt->get_technique_state(tech))
                    enabled_counts[eff]++;
            });

            // 3. Read compile errors from logs if any
            const auto recent_logs = get_recent_logs(200, true);

            // 4. Scan files
            std::unordered_set<std::string> seen_files;
            std::error_code ec;

            for (const auto &sp : search_paths)
            {
                if (!std::filesystem::exists(sp, ec)) continue;

                for (const auto &entry : std::filesystem::recursive_directory_iterator(sp, std::filesystem::directory_options::skip_permission_denied, ec))
                {
                    if (!entry.is_regular_file(ec)) continue;
                    if (entry.path().extension() == ".fx")
                    {
                        const std::string filename = entry.path().filename().u8string();
                        if (seen_files.count(filename)) continue;
                        seen_files.insert(filename);

                        EffectInfo info;
                        info.file_name = filename;
                        info.full_path = entry.path().u8string();
                        info.technique_count = tech_counts[filename];
                        info.enabled_technique_count = enabled_counts[filename];
                        info.compiled = (info.technique_count > 0);

                        if (!info.compiled)
                        {
                            for (const auto &line : recent_logs)
                            {
                                if (line.find(filename) != std::string::npos)
                                    info.errors.push_back(line);
                            }
                        }

                        effects.push_back(std::move(info));
                    }
                }
            }

            // Also add any loaded effects that were outside search paths
            for (const auto &[eff_name, cnt] : tech_counts)
            {
                if (!seen_files.count(eff_name))
                {
                    EffectInfo info;
                    info.file_name = eff_name;
                    info.technique_count = cnt;
                    info.enabled_technique_count = enabled_counts[eff_name];
                    info.compiled = true;
                    effects.push_back(std::move(info));
                }
            }

            return effects;
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

    std::vector<UniformVariableInfo> ReShadeBridge::list_uniform_variables(const std::string &effect_filter, bool enabled_only, bool include_system)
    {
        auto fut = execute_on_render_thread([effect_filter, enabled_only, include_system](reshade::api::effect_runtime *runtime) -> std::vector<UniformVariableInfo> {
            std::vector<UniformVariableInfo> list;
            if (runtime == nullptr) return list;

            // Find all effect names that have at least one enabled technique (matching Home tab / 图二 logic)
            std::unordered_set<std::string> enabled_effects;
            runtime->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime *rt, reshade::api::effect_technique tech) {
                if (rt->get_technique_state(tech))
                {
                    char eff[256] = {};
                    rt->get_technique_effect_name(tech, eff);
                    enabled_effects.insert(eff);
                }
            });

            const char *filter = effect_filter.empty() ? nullptr : effect_filter.c_str();

            runtime->enumerate_uniform_variables(filter, [&](reshade::api::effect_runtime *rt, reshade::api::effect_uniform_variable var) {
                char effect[256] = {};
                rt->get_uniform_variable_effect_name(var, effect);

                const bool is_effect_enabled = (enabled_effects.count(effect) > 0);
                if (enabled_only && !is_effect_enabled)
                    return; // Matches ReShade Home tab: hide variables for inactive effects!

                // Check for hidden and source (special system) annotations
                char source_buf[64] = {};
                size_t source_sz = sizeof(source_buf);
                const bool has_source = rt->get_annotation_string_from_uniform_variable(var, "source", source_buf, &source_sz);

                bool is_hidden = false;
                rt->get_annotation_bool_from_uniform_variable(var, "hidden", &is_hidden, 1);

                if (!include_system && (has_source || is_hidden))
                    return; // Skip read-only internal system variables unless requested

                UniformVariableInfo info;
                info.effect_name = effect;
                info.effect_enabled = is_effect_enabled;
                info.is_system = has_source;
                info.is_hidden = is_hidden;

                char name[256] = {};
                rt->get_uniform_variable_name(var, name);
                info.name = name;

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

                char category[256] = {};
                if (rt->get_annotation_string_from_uniform_variable(var, "ui_category", category))
                    info.ui_category = category;

                char ui_type[64] = {};
                if (rt->get_annotation_string_from_uniform_variable(var, "ui_type", ui_type))
                    info.ui_type = ui_type;

                char units[64] = {};
                if (rt->get_annotation_string_from_uniform_variable(var, "ui_units", units))
                    info.ui_units = units;

                int digits = -1;
                if (rt->get_annotation_int_from_uniform_variable(var, "ui_digits", &digits, 1))
                    info.ui_digits = digits;

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

    bool ReShadeBridge::set_uniform_variable(const std::string &effect_name, const std::string &var_name, const nlohmann::json &val, bool auto_save)
    {
        auto fut = execute_on_render_thread([effect_name, var_name, val, auto_save](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime == nullptr) return false;

            const char *eff = effect_name.empty() ? nullptr : effect_name.c_str();
            reshade::api::effect_uniform_variable var = runtime->find_uniform_variable(eff, var_name.c_str());
            if (var.handle == 0) return false;

            reshade::api::format base_type = reshade::api::format::unknown;
            uint32_t rows = 0, cols = 0, len = 0;
            runtime->get_uniform_variable_type(var, &base_type, &rows, &cols, &len);

            const size_t total = (rows ? rows : 1) * (cols ? cols : 1);
            bool success = false;

            if (base_type == reshade::api::format::r32_float)
            {
                if (val.is_array())
                {
                    std::vector<float> data(total, 0.0f);
                    for (size_t i = 0; i < (std::min)(total, val.size()); ++i)
                        data[i] = val[i].get<float>();
                    runtime->set_uniform_value_float(var, data.data(), total);
                    success = true;
                }
                else if (val.is_number())
                {
                    float f = val.get<float>();
                    runtime->set_uniform_value_float(var, &f, 1);
                    success = true;
                }
            }
            else if (base_type == reshade::api::format::r32_sint)
            {
                if (val.is_array())
                {
                    std::vector<int32_t> data(total, 0);
                    for (size_t i = 0; i < (std::min)(total, val.size()); ++i)
                        data[i] = val[i].get<int32_t>();
                    runtime->set_uniform_value_int(var, data.data(), total);
                    success = true;
                }
                else if (val.is_number_integer())
                {
                    int32_t v = val.get<int32_t>();
                    runtime->set_uniform_value_int(var, &v, 1);
                    success = true;
                }
            }
            else if (base_type == reshade::api::format::r32_typeless || base_type == reshade::api::format::r32_uint)
            {
                if (val.is_boolean())
                {
                    bool b = val.get<bool>();
                    runtime->set_uniform_value_bool(var, &b, 1);
                    success = true;
                }
                else if (val.is_number())
                {
                    uint32_t u = val.get<uint32_t>();
                    runtime->set_uniform_value_uint(var, &u, 1);
                    success = true;
                }
            }

            if (success && auto_save)
            {
                runtime->save_current_preset();
            }

            return success;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return false;
    }

    bool ReShadeBridge::reset_uniform_variable(const std::string &effect_name, const std::string &var_name, bool auto_save)
    {
        auto fut = execute_on_render_thread([effect_name, var_name, auto_save](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime == nullptr) return false;

            const char *eff = effect_name.empty() ? nullptr : effect_name.c_str();
            reshade::api::effect_uniform_variable var = runtime->find_uniform_variable(eff, var_name.c_str());
            if (var.handle == 0) return false;

            runtime->reset_uniform_value(var);
            if (auto_save)
            {
                runtime->save_current_preset();
            }
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

    bool ReShadeBridge::load_preset(const std::string &preset_path)
    {
        auto fut = execute_on_render_thread([this, preset_path](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime == nullptr) return false;
            runtime->set_current_preset_path(preset_path.c_str());
            {
                std::lock_guard<std::mutex> lock(m_stats_mutex);
                m_stats.current_preset = preset_path;
            }
            return true;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return false;
    }

    std::string ReShadeBridge::get_current_preset_path()
    {
        auto fut = execute_on_render_thread([](reshade::api::effect_runtime *runtime) -> std::string {
            if (runtime == nullptr) return "";
            char buf[1024] = {};
            size_t sz = sizeof(buf);
            runtime->get_current_preset_path(buf, &sz);
            return std::string(buf);
        });

        if (fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready)
            return fut.get();

        std::lock_guard<std::mutex> lock(m_stats_mutex);
        return m_stats.current_preset;
    }

    bool ReShadeBridge::set_performance_mode(bool enabled)
    {
        auto fut = execute_on_render_thread([this, enabled](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime != nullptr)
            {
                reshade::set_config_value(runtime, "GENERAL", "PerformanceMode", enabled ? "1" : "0");
                runtime->reload_effect_next_frame(nullptr);
            }
            else
            {
                reshade::set_config_value(nullptr, "GENERAL", "PerformanceMode", enabled ? "1" : "0");
            }
            {
                std::lock_guard<std::mutex> lock(m_stats_mutex);
                m_stats.performance_mode = enabled;
            }
            return true;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();

        reshade::set_config_value(nullptr, "GENERAL", "PerformanceMode", enabled ? "1" : "0");
        {
            std::lock_guard<std::mutex> lock(m_stats_mutex);
            m_stats.performance_mode = enabled;
        }
        return true;
    }

    bool ReShadeBridge::get_performance_mode()
    {
        reshade::api::effect_runtime *rt = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_runtime_mutex);
            rt = m_current_runtime;
        }
        bool enabled = false;
        if (reshade::get_config_value(rt, "GENERAL", "PerformanceMode", enabled))
            return enabled;
        if (rt != nullptr && reshade::get_config_value(nullptr, "GENERAL", "PerformanceMode", enabled))
            return enabled;
        std::lock_guard<std::mutex> lock(m_stats_mutex);
        return m_stats.performance_mode;
    }

    bool ReShadeBridge::reload_effects(const std::string &effect_name)
    {
        auto fut = execute_on_render_thread([effect_name](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime == nullptr) return false;
            runtime->reload_effect_next_frame(effect_name.empty() ? nullptr : effect_name.c_str());
            return true;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return false;
    }

    bool ReShadeBridge::set_effects_state(bool enabled)
    {
        auto fut = execute_on_render_thread([this, enabled](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime == nullptr) return false;
            runtime->set_effects_state(enabled);
            {
                std::lock_guard<std::mutex> lock(m_stats_mutex);
                m_stats.effects_enabled = enabled;
            }
            return true;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();
        return false;
    }

    bool ReShadeBridge::get_effects_state()
    {
        auto fut = execute_on_render_thread([](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime == nullptr) return false;
            return runtime->get_effects_state();
        });

        if (fut.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready)
            return fut.get();

        std::lock_guard<std::mutex> lock(m_stats_mutex);
        return m_stats.effects_enabled;
    }

    bool ReShadeBridge::set_overlay_state(bool open)
    {
        auto fut = execute_on_render_thread([open](reshade::api::effect_runtime *runtime) -> bool {
            if (runtime == nullptr) return false;
            return runtime->open_overlay(open, reshade::api::input_source::keyboard);
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
        const std::filesystem::path base_path(base_path_buf);

        // Read disabled addons list from ReShade.ini
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

        // Directories to scan: base path, addons/ directory, and configured AddonPath
        std::vector<std::filesystem::path> scan_dirs;
        scan_dirs.push_back(base_path);
        scan_dirs.push_back(base_path / "addons");

        char custom_addon_path[MAX_PATH] = {};
        size_t custom_sz = sizeof(custom_addon_path);
        if (reshade::get_config_value(nullptr, "ADDON", "AddonPath", custom_addon_path, &custom_sz))
        {
            std::filesystem::path ap(custom_addon_path);
            if (ap.is_relative()) ap = base_path / ap;
            scan_dirs.push_back(ap);
        }

        std::unordered_set<std::string> seen_files;
        std::error_code ec;

        for (const auto &dir : scan_dirs)
        {
            if (!std::filesystem::exists(dir, ec)) continue;

            for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            {
                if (!entry.is_regular_file()) continue;
                const auto ext = entry.path().extension();
                if (ext == ".addon" || ext == ".addon64" || ext == ".addon32")
                {
                    const std::string filename = entry.path().filename().u8string();
                    if (seen_files.count(filename)) continue;
                    seen_files.insert(filename);

                    AddonInfo info;
                    info.file = filename;
                    info.full_path = entry.path().u8string();
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

                    // Extract PE exports
                    HMODULE mod = LoadLibraryExW(entry.path().c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
                    if (mod)
                    {
                        if (const char *const *pName = reinterpret_cast<const char *const *>(GetProcAddress(mod, "NAME")))
                            info.name = *pName;
                        if (const char *const *pDesc = reinterpret_cast<const char *const *>(GetProcAddress(mod, "DESCRIPTION")))
                            info.description = *pDesc;
                        if (const char *const *pAuth = reinterpret_cast<const char *const *>(GetProcAddress(mod, "AUTHOR")))
                            info.author = *pAuth;
                        if (const char *const *pWeb = reinterpret_cast<const char *const *>(GetProcAddress(mod, "WEBSITE")))
                            info.website = *pWeb;
                        if (const char *const *pIss = reinterpret_cast<const char *const *>(GetProcAddress(mod, "ISSUES")))
                            info.issues = *pIss;

                        FreeLibrary(mod);
                    }

                    // Extract PE version resource
                    DWORD ver_handle = 0;
                    DWORD ver_size = GetFileVersionInfoSizeW(entry.path().c_str(), &ver_handle);
                    if (ver_size > 0)
                    {
                        std::vector<BYTE> ver_data(ver_size);
                        if (GetFileVersionInfoW(entry.path().c_str(), ver_handle, ver_size, ver_data.data()))
                        {
                            VS_FIXEDFILEINFO *pFileInfo = nullptr;
                            UINT len = 0;
                            if (VerQueryValueW(ver_data.data(), L"\\", reinterpret_cast<void**>(&pFileInfo), &len) && pFileInfo != nullptr)
                            {
                                info.version = std::to_string(HIWORD(pFileInfo->dwFileVersionMS)) + "." +
                                               std::to_string(LOWORD(pFileInfo->dwFileVersionMS)) + "." +
                                               std::to_string(HIWORD(pFileInfo->dwFileVersionLS)) + "." +
                                               std::to_string(LOWORD(pFileInfo->dwFileVersionLS));
                            }
                        }
                    }

                    list.push_back(std::move(info));
                }
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

    nlohmann::json ReShadeBridge::get_all_config()
    {
        nlohmann::json root = nlohmann::json::object();

        char base_path_buf[MAX_PATH] = {};
        size_t base_path_size = sizeof(base_path_buf);
        reshade::get_reshade_base_path(base_path_buf, &base_path_size);

        std::filesystem::path ini_path = std::filesystem::path(base_path_buf) / "ReShade.ini";
        if (!std::filesystem::exists(ini_path))
        {
            ini_path = std::filesystem::path(get_process_path()).parent_path() / "ReShade.ini";
        }

        FILE *f = _wfsopen(ini_path.c_str(), L"r", SH_DENYNO);
        if (!f)
            return root;

        std::string current_section = "GENERAL";
        char line_buf[2048];

        while (fgets(line_buf, sizeof(line_buf), f))
        {
            std::string line(line_buf);
            // Trim
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            line = line.substr(start);

            if (line.empty() || line[0] == ';' || line[0] == '#')
                continue;

            if (line.front() == '[' && line.back() == ']')
            {
                current_section = line.substr(1, line.size() - 2);
                if (!root.contains(current_section))
                    root[current_section] = nlohmann::json::object();
                continue;
            }

            const size_t eq = line.find('=');
            if (eq != std::string::npos)
            {
                std::string key = line.substr(0, eq);
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
                std::string val = line.substr(eq + 1);
                size_t vstart = val.find_first_not_of(" \t");
                if (vstart != std::string::npos) val = val.substr(vstart); else val.clear();

                if (!root.contains(current_section))
                    root[current_section] = nlohmann::json::object();

                // Check if comma-separated list
                if (val.find(',') != std::string::npos && (key.find("Key") != std::string::npos || key.find("Path") != std::string::npos))
                {
                    std::vector<std::string> parts;
                    std::stringstream ss(val);
                    std::string part;
                    while (std::getline(ss, part, ','))
                    {
                        while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.erase(0, 1);
                        while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.pop_back();
                        parts.push_back(part);
                    }
                    root[current_section][key] = parts;
                }
                else
                {
                    root[current_section][key] = val;
                }
            }
        }

        fclose(f);
        return root;
    }

    nlohmann::json ReShadeBridge::get_addon_config(const std::string &addon_name)
    {
        nlohmann::json all_cfg = get_all_config();
        nlohmann::json res = nlohmann::json::object();

        std::string upper_name = addon_name;
        std::transform(upper_name.begin(), upper_name.end(), upper_name.begin(), [](unsigned char c) { return (char)std::toupper(c); });

        for (auto it = all_cfg.begin(); it != all_cfg.end(); ++it)
        {
            std::string sec = it.key();
            std::string upper_sec = sec;
            std::transform(upper_sec.begin(), upper_sec.end(), upper_sec.begin(), [](unsigned char c) { return (char)std::toupper(c); });

            // Matches section names e.g. "DEPTH" for "Generic Depth", "OBS_CAPTURE", "SHADEPILOT", etc.
            if (upper_sec == upper_name || upper_name.find(upper_sec) != std::string::npos || upper_sec.find(upper_name) != std::string::npos)
            {
                res[sec] = it.value();
            }
        }

        return res;
    }

    bool ReShadeBridge::set_addon_config(const std::string &addon_name, const std::string &key, const std::string &value)
    {
        std::string section = addon_name;
        // If user passed addon name "Generic Depth", map to section "DEPTH"
        if (section == "Generic Depth" || section == "generic_depth")
            section = "DEPTH";
        else if (section == "ShadePilot" || section == "shadepilot")
            section = "SHADEPILOT";

        return set_config(section, key, value);
    }

    std::string ReShadeBridge::get_config(const std::string &section, const std::string &key)
    {
        char val[1024] = {};
        size_t sz = sizeof(val);
        reshade::api::effect_runtime *rt = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_runtime_mutex);
            rt = m_current_runtime;
        }
        if (reshade::get_config_value(rt, section.c_str(), key.c_str(), val, &sz))
            return std::string(val);
        if (rt != nullptr && reshade::get_config_value(nullptr, section.c_str(), key.c_str(), val, &sz))
            return std::string(val);
        return "";
    }

    std::vector<std::string> ReShadeBridge::get_config_array(const std::string &section, const std::string &key)
    {
        std::vector<std::string> res;
        char val[4096] = {};
        size_t sz = sizeof(val);
        reshade::api::effect_runtime *rt = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_runtime_mutex);
            rt = m_current_runtime;
        }
        if (reshade::get_config_value(rt, section.c_str(), key.c_str(), val, &sz) ||
            (rt != nullptr && reshade::get_config_value(nullptr, section.c_str(), key.c_str(), val, &sz)))
        {
            size_t offset = 0;
            while (offset < sz && val[offset] != '\0')
            {
                std::string item(val + offset);
                res.push_back(item);
                offset += item.size() + 1;
            }
        }
        return res;
    }

    bool ReShadeBridge::set_config(const std::string &section, const std::string &key, const std::string &value)
    {
        auto fut = execute_on_render_thread([this, section, key, value](reshade::api::effect_runtime *runtime) -> bool {
            reshade::set_config_value(runtime, section.c_str(), key.c_str(), value.c_str());
            if (runtime == nullptr)
            {
                reshade::set_config_value(nullptr, section.c_str(), key.c_str(), value.c_str());
                return true;
            }

            if (section == "GENERAL")
            {
                if (key == "PerformanceMode")
                {
                    const bool enabled = (value == "1" || value == "true" || value == "TRUE");
                    runtime->reload_effect_next_frame(nullptr);
                    std::lock_guard<std::mutex> lock(m_stats_mutex);
                    m_stats.performance_mode = enabled;
                }
                else if (key == "PresetPath")
                {
                    runtime->set_current_preset_path(value.c_str());
                    std::lock_guard<std::mutex> lock(m_stats_mutex);
                    m_stats.current_preset = value;
                }
                else if (key == "EffectSearchPaths" || key == "TextureSearchPaths" || key == "PreprocessorDefinitions" || key == "SkipLoadingDisabledEffects")
                {
                    runtime->reload_effect_next_frame(nullptr);
                }
            }
            return true;
        });

        if (fut.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready)
            return fut.get();

        reshade::set_config_value(nullptr, section.c_str(), key.c_str(), value.c_str());
        return true;
    }

    std::vector<std::string> ReShadeBridge::get_recent_logs(size_t max_lines, bool errors_only, const std::string &search_query)
    {
        std::vector<std::string> lines;

        char base_path_buf[MAX_PATH] = {};
        size_t base_path_size = sizeof(base_path_buf);
        reshade::get_reshade_base_path(base_path_buf, &base_path_size);
        const std::filesystem::path base_path(base_path_buf);

        // Candidate log paths
        std::vector<std::filesystem::path> candidate_paths = {
            base_path / "ReShade.log",
            std::filesystem::path(get_process_path()).parent_path() / "ReShade.log",
            base_path / "dxgi.log",
            base_path / "d3d11.log",
            base_path / "d3d12.log",
            base_path / "d3d9.log",
            base_path / "opengl32.log"
        };

        std::filesystem::path target_log;
        for (const auto &p : candidate_paths)
        {
            if (std::filesystem::exists(p))
            {
                target_log = p;
                break;
            }
        }

        if (target_log.empty())
            target_log = candidate_paths[0];

        FILE *f = _wfsopen(target_log.c_str(), L"r", SH_DENYNO);
        if (!f)
            return lines;

        char line_buf[4096];
        while (fgets(line_buf, sizeof(line_buf), f))
        {
            std::string line(line_buf);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();

            if (errors_only)
            {
                if (line.find("ERROR |") == std::string::npos &&
                    line.find("WARN  |") == std::string::npos &&
                    line.find("error:") == std::string::npos &&
                    line.find("warning:") == std::string::npos)
                {
                    continue;
                }
            }

            if (!search_query.empty())
            {
                if (line.find(search_query) == std::string::npos)
                    continue;
            }

            lines.push_back(std::move(line));
        }

        fclose(f);

        if (lines.size() > max_lines)
            lines.erase(lines.begin(), lines.end() - max_lines);

        return lines;
    }

    std::string ReShadeBridge::get_process_name() const
    {
        char path[MAX_PATH] = {};
        if (::GetModuleFileNameA(NULL, path, MAX_PATH))
        {
            const char *slash = std::strrchr(path, '\\');
            if (!slash) slash = std::strrchr(path, '/');
            return slash ? (slash + 1) : path;
        }
        return "Unknown";
    }

    std::string ReShadeBridge::get_process_path() const
    {
        char path[MAX_PATH] = {};
        if (::GetModuleFileNameA(NULL, path, MAX_PATH))
        {
            return path;
        }
        return "";
    }

    uint32_t ReShadeBridge::get_process_id() const
    {
        return static_cast<uint32_t>(::GetCurrentProcessId());
    }
}
