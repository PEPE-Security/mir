/*
 * Copyright © 2012 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2 or 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by:
 *   Christopher James Halse Rogers <christopher.halse.rogers@canonical.com>
 */

#include "buffer_allocator.h"
#include "gbm_buffer.h"
#include "mir/graphics/linux_dmabuf.h"
#include "buffer_texture_binder.h"
#include "mir/anonymous_shm_file.h"
#include "shm_buffer.h"
#include "display_helpers.h"
#include "gbm_format_conversions.h"
#include "egl_context_executor.h"
#include "mir/graphics/egl_extensions.h"
#include "mir/graphics/egl_error.h"
#include "mir/graphics/buffer_properties.h"
#include "mir/raii.h"
#include "mir/graphics/display.h"
#include "mir/renderer/gl/context.h"
#include "mir/renderer/gl/context_source.h"
#include "mir/graphics/egl_wayland_allocator.h"
#include "buffer_from_wl_shm.h"
#include "mir/executor.h"

#include <boost/throw_exception.hpp>
#include <boost/exception/errinfo_errno.hpp>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <gbm.h>
#include <cassert>
#include <fcntl.h>

#include <wayland-server.h>

#define MIR_LOG_COMPONENT "gbm-kms-buffer-allocator"
#include <mir/log.h>
#include <mutex>

namespace mg  = mir::graphics;
namespace mgg = mg::gbm;
namespace mgc = mg::common;
namespace geom = mir::geometry;

namespace
{

class EGLImageBufferTextureBinder : public mgc::BufferTextureBinder
{
public:
    EGLImageBufferTextureBinder(std::shared_ptr<gbm_bo> const& gbm_bo,
                                std::shared_ptr<mg::EGLExtensions> const& egl_extensions)
        : bo{gbm_bo},
          egl_extensions{egl_extensions},
          egl_image{EGL_NO_IMAGE_KHR}
    {
    }

    ~EGLImageBufferTextureBinder()
    {
        if (egl_image != EGL_NO_IMAGE_KHR)
            egl_extensions->base(egl_display).eglDestroyImageKHR(egl_display, egl_image);
    }


    void gl_bind_to_texture() override
    {
        ensure_egl_image();

        egl_extensions->base(egl_display).glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, egl_image);
    }

protected:
    virtual void ensure_egl_image() = 0;

    std::shared_ptr<gbm_bo> const bo;
    std::shared_ptr<mg::EGLExtensions> const egl_extensions;
    EGLDisplay egl_display;
    EGLImageKHR egl_image;
};

class NativePixmapTextureBinder : public EGLImageBufferTextureBinder
{
public:
    NativePixmapTextureBinder(std::shared_ptr<gbm_bo> const& gbm_bo,
                              std::shared_ptr<mg::EGLExtensions> const& egl_extensions)
        : EGLImageBufferTextureBinder(gbm_bo, egl_extensions)
    {
    }

private:
    void ensure_egl_image()
    {
        if (egl_image == EGL_NO_IMAGE_KHR)
        {
            eglBindAPI(EGL_OPENGL_ES_API);
            egl_display = eglGetCurrentDisplay();
            gbm_bo* bo_raw{bo.get()};

            const EGLint image_attrs[] =
            {
                EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                EGL_NONE
            };

            egl_image = egl_extensions->base(egl_display).eglCreateImageKHR(
                egl_display,
                EGL_NO_CONTEXT,
                EGL_NATIVE_PIXMAP_KHR,
                reinterpret_cast<void*>(bo_raw),
                image_attrs);
            if (egl_image == EGL_NO_IMAGE_KHR)
                BOOST_THROW_EXCEPTION(mg::egl_error("Failed to create EGLImage"));
        }
    }
};

class DMABufTextureBinder : public EGLImageBufferTextureBinder
{
public:
    DMABufTextureBinder(std::shared_ptr<gbm_bo> const& gbm_bo,
                        std::shared_ptr<mg::EGLExtensions> const& egl_extensions)
        : EGLImageBufferTextureBinder(gbm_bo, egl_extensions)
    {
    }

private:
    void ensure_egl_image()
    {
        if (egl_image == EGL_NO_IMAGE_KHR)
        {
            eglBindAPI(EGL_OPENGL_ES_API);
            egl_display = eglGetCurrentDisplay();
            gbm_bo* bo_raw{bo.get()};

            auto device = gbm_bo_get_device(bo_raw);
            auto gem_handle = gbm_bo_get_handle(bo_raw).u32;
            auto drm_fd = gbm_device_get_fd(device);
            int raw_fd = -1;

            auto ret = drmPrimeHandleToFD(drm_fd, gem_handle, DRM_CLOEXEC, &raw_fd);
            prime_fd = mir::Fd{raw_fd};
            if (ret)
            {
                std::string const msg("Failed to get PRIME fd from gbm-kms bo");
                BOOST_THROW_EXCEPTION(
                    std::system_error(errno, std::system_category(), "Failed to get PRIME fd from gbm-kms bo"));
            }

            const EGLint image_attrs_X[] =
            {
                EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                EGL_WIDTH, static_cast<EGLint>(gbm_bo_get_width(bo_raw)),
                EGL_HEIGHT, static_cast<EGLint>(gbm_bo_get_height(bo_raw)),
                EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(gbm_bo_get_format(bo_raw)),
                EGL_DMA_BUF_PLANE0_FD_EXT, prime_fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(gbm_bo_get_stride(bo_raw)),
                EGL_NONE
            };

            egl_image = egl_extensions->base(egl_display).eglCreateImageKHR(
                egl_display,
                EGL_NO_CONTEXT,
                EGL_LINUX_DMA_BUF_EXT,
                static_cast<EGLClientBuffer>(nullptr),
                image_attrs_X);
            if (egl_image == EGL_NO_IMAGE_KHR)
                BOOST_THROW_EXCEPTION(mg::egl_error("Failed to create EGLImage"));
        }
    }

    mir::Fd prime_fd;
};

struct GBMBODeleter
{
    void operator()(gbm_bo* handle) const
    {
        if (handle)
            gbm_bo_destroy(handle);
    }
};

std::unique_ptr<mir::renderer::gl::Context> context_for_output(mg::Display const& output)
{
    try
    {
        auto& context_source = dynamic_cast<mir::renderer::gl::ContextSource const&>(output);

        /*
         * We care about no part of this context's config; we will do no rendering with it.
         * All we care is that we can allocate texture IDs and bind a texture, which is
         * config independent.
         *
         * That's not *entirely* true; we also need it to be on the same device as we want
         * to do the rendering on, and that GL must support all the extensions we care about,
         * but since we don't yet support heterogeneous hybrid and implementing that will require
         * broader interface changes it's a safe enough requirement for now.
         */
        return context_source.create_gl_context();
    }
    catch (std::bad_cast const& err)
    {
        std::throw_with_nested(
            boost::enable_error_info(
                std::runtime_error{"Output platform cannot provide a GL context"})
                << boost::throw_function(__PRETTY_FUNCTION__)
                << boost::throw_line(__LINE__)
                << boost::throw_file(__FILE__));
    }
}
}

mgg::BufferAllocator::BufferAllocator(
    mg::Display const& output,
    gbm_device* device,
    BypassOption bypass_option,
    mgg::BufferImportMethod const buffer_import_method)
    : ctx{context_for_output(output)},
      egl_delegate{
          std::make_shared<mgc::EGLContextExecutor>(context_for_output(output))},
      device(device),
      egl_extensions(std::make_shared<mg::EGLExtensions>()),
      bypass_option(buffer_import_method == mgg::BufferImportMethod::dma_buf ?
                        mgg::BypassOption::prohibited :
                        bypass_option),
      buffer_import_method(buffer_import_method)
{
}

std::shared_ptr<mg::Buffer> mgg::BufferAllocator::alloc_software_buffer(
    geom::Size size, MirPixelFormat format)
{
    if (!mgc::MemoryBackedShmBuffer::supports(format))
    {
        BOOST_THROW_EXCEPTION(
            std::runtime_error(
                "Trying to create SHM buffer with unsupported pixel format"));
    }

    return std::make_shared<mgc::MemoryBackedShmBuffer>(size, format, egl_delegate);
}

std::vector<MirPixelFormat> mgg::BufferAllocator::supported_pixel_formats()
{
    /*
     * supported_pixel_formats() is kind of a kludge. The right answer depends
     * on whether you're using hardware or software, and it depends on
     * the usage type (e.g. scanout). In the future it's also expected to
     * depend on the GPU model in use at runtime.
     *   To be precise, ShmBuffer now supports OpenGL compositing of all
     * but one MirPixelFormat (bgr_888). But GBM only supports [AX]RGB.
     * So since we don't yet have an adequate API in place to query what the
     * intended usage will be, we need to be conservative and report the
     * intersection of ShmBuffer and GBM's pixel format support. That is
     * just these two. Be aware however you can create a software surface
     * with almost any pixel format and it will also work...
     *   TODO: Convert this to a loop that just queries the intersection of
     * gbm_device_is_format_supported and ShmBuffer::supports(), however not
     * yet while the former is buggy. (FIXME: LP: #1473901)
     */
    static std::vector<MirPixelFormat> const pixel_formats{
        mir_pixel_format_argb_8888,
        mir_pixel_format_xrgb_8888
    };

    return pixel_formats;
}

void mgg::BufferAllocator::bind_display(wl_display* display, std::shared_ptr<Executor> wayland_executor)
{
    auto context_guard = mir::raii::paired_calls(
        [this]() { ctx->make_current(); },
        [this]() { ctx->release_current(); });
    auto dpy = eglGetCurrentDisplay();

    try
    {
        mg::wayland::bind_display(dpy, display, *egl_extensions);
        egl_display_bound = true;
    }
    catch (...)
    {
        log(
            logging::Severity::warning,
            MIR_LOG_COMPONENT,
            std::current_exception(),
            "Failed to bind EGL Display to Wayland display, falling back to software buffers");
    }

    try
    {
        mg::EGLExtensions::EXTImageDmaBufImportModifiers modifier_ext{dpy};
        dmabuf_extension =
            std::unique_ptr<LinuxDmaBufUnstable, std::function<void(LinuxDmaBufUnstable*)>>(
                new LinuxDmaBufUnstable{
                    display,
                    dpy,
                    egl_extensions,
                    modifier_ext,
                },
                [wayland_executor](LinuxDmaBufUnstable* global)
                {
                    // The global must be destroyed on the Wayland thread
                    wayland_executor->spawn(
                        [global]()
                        {
                            /* This is safe against double-frees, as the WaylandExecutor
                             * guarantees that work scheduled will only run while the Wayland
                             * event loop is running, and the main loop is stopped before
                             * wl_display_destroy() frees any globals
                             *
                             * This will, however, leak the global if the main loop is destroyed
                             * before the buffer allocator. Fixing that requires work in the
                             * wrapper generator.
                             */
                            delete global;
                        });
                });
        mir::log_info("Enabled linux-dmabuf import support");
    }
    catch (std::runtime_error const&)
    {
        mir::log_info(
            "No EGL_EXT_image_dma_buf_import_modifiers support, disabling linux-dmabuf import");
    }

    this->wayland_executor = std::move(wayland_executor);
}

void mgg::BufferAllocator::unbind_display(wl_display* display)
{
    if (egl_display_bound)
    {
        auto context_guard = mir::raii::paired_calls(
            [this]() { ctx->make_current(); },
            [this]() { ctx->release_current(); });
        auto dpy = eglGetCurrentDisplay();

        mg::wayland::unbind_display(dpy, display, *egl_extensions);
    }
}

std::shared_ptr<mg::Buffer> mgg::BufferAllocator::buffer_from_resource(
    wl_resource* buffer,
    std::function<void()>&& on_consumed,
    std::function<void()>&& on_release)
{
    auto context_guard = mir::raii::paired_calls(
        [this]() { ctx->make_current(); },
        [this]() { ctx->release_current(); });

    if (auto dmabuf = dmabuf_extension->buffer_from_resource(
        buffer,
        ctx,
        std::function<void()>{on_consumed},
        std::function<void()>{on_release},
        wayland_executor))
    {
        return dmabuf;
    }
    return mg::wayland::buffer_from_resource(
        buffer,
        std::move(on_consumed),
        std::move(on_release),
        ctx,
        *egl_extensions,
        wayland_executor);
}

auto mgg::BufferAllocator::buffer_from_shm(
    wl_resource* buffer,
    std::shared_ptr<Executor> wayland_executor,
    std::function<void()>&& on_consumed) -> std::shared_ptr<Buffer>
{
    return mg::wayland::buffer_from_wl_shm(
        buffer,
        std::move(wayland_executor),
        egl_delegate,
        std::move(on_consumed));
}
