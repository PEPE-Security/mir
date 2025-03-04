/*
 * Copyright © 2018-2021 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Christopher James Halse Rogers <christopher.halse.rogers@canonical.com>
 */

#include "wl_surface.h"

#include "wayland_utils.h"
#include "wl_surface_role.h"
#include "wl_subcompositor.h"
#include "wl_region.h"
#include "deleted_for_resource.h"

#include "wayland_wrapper.h"

#include "wayland_frontend.tp.h"

#include "mir/graphics/buffer_properties.h"
#include "mir/scene/session.h"
#include "mir/frontend/wayland.h"
#include "mir/compositor/buffer_stream.h"
#include "mir/executor.h"
#include "mir/graphics/graphic_buffer_allocator.h"
#include "mir/scene/surface.h"
#include "mir/shell/surface_specification.h"
#include "mir/log.h"

#include <algorithm>
#include <chrono>
#include <boost/throw_exception.hpp>
#include <wayland-server-protocol.h>

namespace mf = mir::frontend;
namespace geom = mir::geometry;
namespace mw = mir::wayland;
namespace msh = mir::shell;

mf::WlSurfaceState::Callback::Callback(wl_resource* new_resource)
    : mw::Callback{new_resource, Version<1>()},
      destroyed{deleted_flag_for_resource(resource)}
{
}

void mf::WlSurfaceState::update_from(WlSurfaceState const& source)
{
    if (source.buffer)
        buffer = source.buffer;

    if (source.scale)
        scale = source.scale;

    if (source.offset)
        offset = source.offset;

    if (source.input_shape)
        input_shape = source.input_shape;

    frame_callbacks.insert(end(frame_callbacks),
                           begin(source.frame_callbacks),
                           end(source.frame_callbacks));

    if (source.surface_data_invalidated)
        surface_data_invalidated = true;
}

bool mf::WlSurfaceState::surface_data_needs_refresh() const
{
    return offset ||
           input_shape ||
           surface_data_invalidated;
}

mf::WlSurface::WlSurface(
    wl_resource* new_resource,
    std::shared_ptr<Executor> const& executor,
    std::shared_ptr<graphics::GraphicBufferAllocator> const& allocator)
    : Surface(new_resource, Version<4>()),
        session{get_session(client)},
        stream{session->create_buffer_stream({{}, mir_pixel_format_invalid, graphics::BufferUsage::undefined})},
        allocator{allocator},
        executor{executor},
        null_role{this},
        role{&null_role}
{
    // wl_surface is specified to act in mailbox mode
    stream->allow_framedropping(true);
}

mf::WlSurface::~WlSurface()
{
    role->destroy();
    session->destroy_buffer_stream(stream);
}

bool mf::WlSurface::synchronized() const
{
    return role->synchronized();
}

auto mf::WlSurface::subsurface_at(geom::Point point) -> std::experimental::optional<WlSurface*>
{
    if (!buffer_size_)
    {
        // surface not mapped
        return std::experimental::nullopt;
    }
    point = point - offset_;
    // loop backwards so the first subsurface we find that accepts the input is the topmost one
    for (auto child_it = children.rbegin(); child_it != children.rend(); ++child_it)
    {
        if (auto result = (*child_it)->subsurface_at(point))
            return result;
    }
    geom::Rectangle surface_rect = {geom::Point{}, buffer_size_.value_or(geom::Size{})};
    for (auto& rect : input_shape.value_or(std::vector<geom::Rectangle>{surface_rect}))
    {
        if (rect.intersection_with(surface_rect).contains(point))
            return this;
    }
    return std::experimental::nullopt;
}

auto mf::WlSurface::scene_surface() const -> std::experimental::optional<std::shared_ptr<scene::Surface>>
{
    return role->scene_surface();
}

void mf::WlSurface::set_role(WlSurfaceRole* role_)
{
    if (role != &null_role)
        BOOST_THROW_EXCEPTION(std::runtime_error("Surface already has a role"));
    role = role_;
}

void mf::WlSurface::clear_role()
{
    role = &null_role;
}

void mf::WlSurface::set_pending_offset(std::experimental::optional<geom::Displacement> const& offset)
{
    pending.offset = offset;
}

void mf::WlSurface::add_subsurface(WlSubsurface* child)
{
    if (std::find(children.begin(), children.end(), child) != children.end())
    {
        log_warning("Subsurface %p added to surface %p multiple times", static_cast<void*>(child), static_cast<void*>(this));
        return;
    }

    children.push_back(child);
}

void mf::WlSurface::remove_subsurface(WlSubsurface* child)
{
    children.erase(
        std::remove(
            children.begin(),
            children.end(),
            child),
        children.end());
}

void mf::WlSurface::refresh_surface_data_now()
{
    role->refresh_surface_data_now();
}

void mf::WlSurface::populate_surface_data(std::vector<shell::StreamSpecification>& buffer_streams,
                                          std::vector<geom::Rectangle>& input_shape_accumulator,
                                          geometry::Displacement const& parent_offset) const
{
    geometry::Displacement offset = parent_offset + offset_;

    buffer_streams.push_back(msh::StreamSpecification{stream, offset, {}});
    geom::Rectangle surface_rect = {geom::Point{} + offset, buffer_size_.value_or(geom::Size{})};
    if (input_shape)
    {
        for (auto rect : input_shape.value())
        {
            rect.top_left = rect.top_left + offset;
            rect = rect.intersection_with(surface_rect); // clip to surface
            input_shape_accumulator.push_back(rect);
        }

        // If we have an explicity specified empty input shape all inport should be ignored
        // however, if we give Mir an empty vector it will use a default input shape
        // therefore we add a zero size rect to the vector
        // TODO: sort this whole mess out
        if (input_shape.value().empty())
        {
            input_shape_accumulator.push_back({{}, {}});
        }
    }
    else
    {
        input_shape_accumulator.push_back(surface_rect);
    }

    for (WlSubsurface* subsurface : children)
    {
        subsurface->populate_surface_data(buffer_streams, input_shape_accumulator, offset);
    }
}

mf::WlSurface* mf::WlSurface::from(wl_resource* resource)
{
    void* raw_surface = wl_resource_get_user_data(resource);
    return static_cast<WlSurface*>(static_cast<wayland::Surface*>(raw_surface));
}

void mf::WlSurface::send_frame_callbacks()
{
    for (auto const& frame : frame_callbacks)
    {
        if (!*frame->destroyed)
        {
            auto const timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch());
            frame->send_done_event(timestamp_ms.count());
            frame->destroy_wayland_object();
        }
    }
    frame_callbacks.clear();
}

void mf::WlSurface::destroy()
{
    destroy_wayland_object();
}

void mf::WlSurface::attach(std::experimental::optional<wl_resource*> const& buffer, int32_t x, int32_t y)
{
    if (x != 0 || y != 0)
    {
        mir::log_warning("Client requested unimplemented non-zero attach offset. Rendering will be incorrect.");
    }

    pending.buffer = buffer.value_or(nullptr);
}

void mf::WlSurface::damage(int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    // This isn't essential, but could enable optimizations
}

void mf::WlSurface::damage_buffer(int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    // This isn't essential, but could enable optimizations
}

void mf::WlSurface::frame(wl_resource* new_callback)
{
    pending.frame_callbacks.push_back(std::make_shared<WlSurfaceState::Callback>(new_callback));
}

void mf::WlSurface::set_opaque_region(std::experimental::optional<wl_resource*> const& region)
{
    (void)region;
    // This isn't essential, but could enable optimizations
}

void mf::WlSurface::set_input_region(std::experimental::optional<wl_resource*> const& region)
{
    if (region)
    {
        // since pending.input_shape is an optional optional, this is needed
        auto shape = WlRegion::from(region.value())->rectangle_vector();
        pending.input_shape = decltype(pending.input_shape)::value_type{move(shape)};
    }
    else
    {
        // set the inner optional to nullopt to indicate the input region should be updated, but with a null region
        pending.input_shape = decltype(pending.input_shape)::value_type{};
    }
}

namespace
{
MirPixelFormat wl_format_to_mir_format(uint32_t format)
{
    switch (format)
    {
        case WL_SHM_FORMAT_ARGB8888:
            return mir_pixel_format_argb_8888;
        case WL_SHM_FORMAT_XRGB8888:
            return mir_pixel_format_xrgb_8888;
        case WL_SHM_FORMAT_RGBA4444:
            return mir_pixel_format_rgba_4444;
        case WL_SHM_FORMAT_RGBA5551:
            return mir_pixel_format_rgba_5551;
        case WL_SHM_FORMAT_RGB565:
            return mir_pixel_format_rgb_565;
        case WL_SHM_FORMAT_RGB888:
            return mir_pixel_format_rgb_888;
        case WL_SHM_FORMAT_BGR888:
            return mir_pixel_format_bgr_888;
        case WL_SHM_FORMAT_XBGR8888:
            return mir_pixel_format_xbgr_8888;
        case WL_SHM_FORMAT_ABGR8888:
            return mir_pixel_format_abgr_8888;
        default:
            return mir_pixel_format_invalid;
    }
}
}

void mf::WlSurface::commit(WlSurfaceState const& state)
{
    // We're going to lose the value of state, so copy the frame_callbacks first. We have to maintain a list of
    // callbacks in wl_surface because if a client commits multiple times before the first buffer is handled, all the
    // callbacks should be sent at once.
    frame_callbacks.insert(end(frame_callbacks), begin(state.frame_callbacks), end(state.frame_callbacks));

    if (state.offset)
        offset_ = state.offset.value();

    if (state.input_shape)
        input_shape = state.input_shape.value();

    if (state.scale)
        stream->set_scale(state.scale.value());

    if (state.buffer)
    {
        wl_resource * buffer = *state.buffer;

        if (buffer == nullptr)
        {
            // TODO: unmap surface, and unmap all subsurfaces
            buffer_size_ = std::experimental::nullopt;
            send_frame_callbacks();
        }
        else
        {
            auto const executor_send_frame_callbacks = [executor = executor, weak_self = mw::make_weak(this)]()
                {
                    executor->spawn([weak_self]()
                        {
                            if (weak_self)
                            {
                                weak_self.value().send_frame_callbacks();
                            }
                        });
                };

            std::shared_ptr<graphics::Buffer> mir_buffer;

            if (auto const shm_buffer = wl_shm_buffer_get(buffer))
            {
                auto const stride = wl_shm_buffer_get_stride(shm_buffer);
                auto const width = wl_shm_buffer_get_width(shm_buffer);
                auto const format = wl_format_to_mir_format(wl_shm_buffer_get_format(shm_buffer));
                if (stride < width * MIR_BYTES_PER_PIXEL(format)) {
                    wl_resource_post_error(
                        buffer,
                        WL_SHM_ERROR_INVALID_STRIDE,
                        "Stride (%u) is less than width × bytes per pixel (%u×%u). "
                        "Did you accidentally specify stride in pixels?",
                        stride, width, MIR_BYTES_PER_PIXEL(format));

                    BOOST_THROW_EXCEPTION((
                                              std::runtime_error{"Buffer has invalid stride"}));
                }
                mir_buffer = allocator->buffer_from_shm(
                    buffer,
                    executor,
                    std::move(executor_send_frame_callbacks));
                tracepoint(
                    mir_server_wayland,
                    sw_buffer_committed,
                    wl_resource_get_client(resource),
                    mir_buffer->id().as_value());
            }
            else
            {
                std::shared_ptr<bool> buffer_destroyed = deleted_flag_for_resource(buffer);

                auto release_buffer = [executor = executor, buffer = buffer, destroyed = buffer_destroyed]()
                    {
                        executor->spawn(run_unless(
                            destroyed,
                            [buffer](){ wl_resource_post_event(buffer, wayland::Buffer::Opcode::release); }));
                    };

                mir_buffer = allocator->buffer_from_resource(
                    buffer,
                    std::move(executor_send_frame_callbacks),
                    std::move(release_buffer));
                tracepoint(
                    mir_server_wayland,
                    hw_buffer_committed,
                    wl_resource_get_client(resource),
                    mir_buffer->id().as_value());
            }

            stream->submit_buffer(mir_buffer);
            auto const new_buffer_size = stream->stream_size();

            if (!input_shape && std::experimental::make_optional(new_buffer_size) != buffer_size_)
            {
                state.invalidate_surface_data(); // input shape needs to be recalculated for the new size
            }

            buffer_size_ = new_buffer_size;
        }
    }
    else
    {
        send_frame_callbacks();
    }

    for (WlSubsurface* child: children)
    {
        child->parent_has_committed();
    }
}

void mf::WlSurface::commit()
{
    if (pending.offset && *pending.offset == offset_)
        pending.offset = std::experimental::nullopt;

    // The same input shape could be represented by the same rectangles in a different order, or even
    // different rectangles. We don't check for that, however, because it would only cause an unnecessary
    // update and not do any real harm. Checking for identical vectors should cover most cases.
    if (pending.input_shape && *pending.input_shape == input_shape)
        pending.input_shape = std::experimental::nullopt;

    // order is important
    auto const state = std::move(pending);
    pending = WlSurfaceState();
    role->commit(state);
}

void mf::WlSurface::set_buffer_transform(int32_t transform)
{
    (void)transform;
    // TODO
}

void mf::WlSurface::set_buffer_scale(int32_t scale)
{
    pending.scale = scale;
}

auto mf::WlSurface::confine_pointer_state() const -> MirPointerConfinementState
{
    if (auto const maybe_scene_surface = scene_surface())
    {
        if (auto const scene_surface = *maybe_scene_surface)
        {
            return scene_surface->confine_pointer_state();
        }
    }

    return mir_pointer_unconfined;
}

mf::NullWlSurfaceRole::NullWlSurfaceRole(WlSurface* surface) :
    surface{surface}
{
}

auto mf::NullWlSurfaceRole::scene_surface() const -> std::experimental::optional<std::shared_ptr<scene::Surface>>
{
    return std::experimental::nullopt;
}
void mf::NullWlSurfaceRole::refresh_surface_data_now() {}
void mf::NullWlSurfaceRole::commit(WlSurfaceState const& state) { surface->commit(state); }
void mf::NullWlSurfaceRole::destroy() {}
