/*
 * Copyright © 2015-2020 Canonical Ltd.
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
 * Authored by: Cemil Azizoglu <cemil.azizoglu@canonical.com>
 */

#include "mir/graphics/platform.h"
#include "mir/graphics/display_report.h"
#include "mir/graphics/display_configuration.h"
#include <mir/graphics/display_configuration_policy.h>
#include "mir/graphics/egl_error.h"
#include "mir/graphics/virtual_output.h"
#include "mir/renderer/gl/context.h"
#include "mir/graphics/gl_config.h"
#include "mir/graphics/atomic_frame.h"
#include "mir/graphics/transformation.h"
#include "display_configuration.h"
#include "display.h"
#include "platform.h"
#include "display_buffer.h"

#include <boost/throw_exception.hpp>

#include <X11/Xatom.h>
#include <algorithm>

#define MIR_LOG_COMPONENT "display"
#include "mir/log.h"

namespace mx=mir::X;
namespace mg=mir::graphics;
namespace mgx=mg::X;
namespace geom=mir::geometry;

namespace
{
auto get_pixel_width(Display *dpy)
{
    auto screen = XDefaultScreenOfDisplay(dpy);

    return float(screen->mwidth) / screen->width;
}
auto get_pixel_height(Display *dpy)
{
    auto screen = XDefaultScreenOfDisplay(dpy);

    return float(screen->mheight) / screen->height;
}

class XGLContext : public mir::renderer::gl::Context
{
public:
    XGLContext(::Display* const x_dpy,
               std::shared_ptr<mg::GLConfig> const& gl_config,
               EGLContext const shared_ctx)
        : egl{*gl_config, x_dpy, shared_ctx}
    {
    }

    ~XGLContext() = default;

    void make_current() const override
    {
        egl.make_current();
    }

    void release_current() const override
    {
        egl.release_current();
    }

private:
    mgx::helpers::EGLHelper const egl;
};
}

mgx::X11Window::X11Window(::Display* x_dpy,
                          EGLDisplay egl_dpy,
                          geom::Size const size,
                          EGLConfig const egl_cfg)
    : x_dpy{x_dpy}
{
    auto root = XDefaultRootWindow(x_dpy);

    EGLint vid;
    if (!eglGetConfigAttrib(egl_dpy, egl_cfg, EGL_NATIVE_VISUAL_ID, &vid))
        BOOST_THROW_EXCEPTION(mg::egl_error("Cannot get config attrib"));

    XVisualInfo visTemplate;
    std::memset(&visTemplate, 0, sizeof visTemplate);
    int num_visuals = 0;
    visTemplate.visualid = vid;
    auto visInfo = XGetVisualInfo(x_dpy, VisualIDMask, &visTemplate, &num_visuals);
    if (!visInfo || !num_visuals)
        BOOST_THROW_EXCEPTION(mg::egl_error("Cannot get visual info, or no matching visuals"));

    mir::log_info("%d visual(s) found", num_visuals);
    mir::log_info("Using the first one :");
    mir::log_info("ID\t\t:\t%lu", visInfo->visualid);
    mir::log_info("screen\t:\t%d", visInfo->screen);
    mir::log_info("depth\t\t:\t%d", visInfo->depth);
    mir::log_info("red_mask\t:\t0x%0lX", visInfo->red_mask);
    mir::log_info("green_mask\t:\t0x%0lX", visInfo->green_mask);
    mir::log_info("blue_mask\t:\t0x%0lX", visInfo->blue_mask);
    mir::log_info("colormap_size\t:\t%d", visInfo->colormap_size);
    mir::log_info("bits_per_rgb\t:\t%d", visInfo->bits_per_rgb);

    r_mask = visInfo->red_mask;

    XSetWindowAttributes attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.background_pixel = 0;
    attr.border_pixel = 0;
    attr.colormap = XCreateColormap(x_dpy, root, visInfo->visual, AllocNone);
    attr.event_mask = StructureNotifyMask |
                      ExposureMask        |
                      KeyPressMask        |
                      KeyReleaseMask      |
                      ButtonPressMask     |
                      ButtonReleaseMask   |
                      FocusChangeMask     |
                      EnterWindowMask     |
                      LeaveWindowMask     |
                      PointerMotionMask;

    auto mask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask;

    win = XCreateWindow(x_dpy, root, 0, 0,
                        size.width.as_int(), size.height.as_int(),
                        0, visInfo->depth, InputOutput,
                        visInfo->visual, mask, &attr);

    XFree(visInfo);

    {
        char const * const title = "Mir On X";
        XSizeHints sizehints;
        sizehints.flags = 0;
        XSetNormalHints(x_dpy, win, &sizehints);
        XSetStandardProperties(x_dpy, win, title, title, None, (char **)NULL, 0, &sizehints);

        XWMHints wm_hints = {
            (InputHint|StateHint), // fields in this structure that are defined
            True,                  // does this application rely on the window manager
                                   // to get keyboard input? Yes, if this is False,
                                   // XGrabKeyboard doesn't work reliably.
            NormalState,           // initial_state
            0,                     // icon_pixmap
            0,                     // icon_window
            0, 0,                  // initial position of icon
            0,                     // pixmap to be used as mask for icon_pixmap
            0                      // id of related window_group
        };

        XSetWMHints(x_dpy, win, &wm_hints);

        Atom wmDeleteMessage = XInternAtom(x_dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(x_dpy, win, &wmDeleteMessage, 1);
    }

    XMapWindow(x_dpy, win);

    XEvent xev;
    do 
    {
        XNextEvent(x_dpy, &xev);
    }
    while (xev.type != Expose);
}

mgx::X11Window::~X11Window()
{
    XDestroyWindow(x_dpy, win);
}

mgx::X11Window::operator Window() const
{
    return win;
}

unsigned long mgx::X11Window::red_mask() const
{
    return r_mask;
}

mgx::Display::Display(::Display* x_dpy,
                      std::vector<X11OutputConfig> const& requested_sizes,
                      std::shared_ptr<DisplayConfigurationPolicy> const& initial_conf_policy,
                      std::shared_ptr<GLConfig> const& gl_config,
                      std::shared_ptr<DisplayReport> const& report)
    : shared_egl{*gl_config, x_dpy},
      x_dpy{x_dpy},
      gl_config{gl_config},
      pixel_width{get_pixel_width(x_dpy)},
      pixel_height{get_pixel_height(x_dpy)},
      report{report},
      last_frame{std::make_shared<AtomicFrame>()}
{
    geom::Point top_left{0, 0};

    for (auto const& requested_size : requested_sizes)
    {
        auto actual_size = requested_size.size;
        auto window = std::make_unique<X11Window>(x_dpy, shared_egl.display(), actual_size, shared_egl.config());
        auto red_mask = window->red_mask();
        auto pf = (red_mask == 0xFF0000 ?
            mir_pixel_format_argb_8888 :
            mir_pixel_format_abgr_8888);
        auto configuration = DisplayConfiguration::build_output(
            pf,
            actual_size,
            top_left,
            geom::Size{actual_size.width * pixel_width, actual_size.height * pixel_height},
            requested_size.scale,
            mir_orientation_normal);
        auto display_buffer = std::make_unique<mgx::DisplayBuffer>(
            x_dpy,
            configuration->id,
            *window,
            configuration->extents(),
            shared_egl.context(),
            last_frame,
            report,
            *gl_config);
        top_left.x += as_delta(configuration->extents().size.width);
        outputs.push_back(std::make_unique<OutputInfo>(this, move(window), move(display_buffer), move(configuration)));
    }

    shared_egl.make_current();

    auto const display_config = configuration();
    initial_conf_policy->apply_to(*display_config);
    configure(*display_config);
    report->report_successful_display_construction();
}

mgx::Display::~Display() noexcept
{
}

void mgx::Display::for_each_display_sync_group(std::function<void(mg::DisplaySyncGroup&)> const& f)
{
    std::lock_guard<std::mutex> lock{mutex};
    for (auto const& output : outputs)
    {
        f(*output->display_buffer);
    }
}

std::unique_ptr<mg::DisplayConfiguration> mgx::Display::configuration() const
{
    std::lock_guard<std::mutex> lock{mutex};
    std::vector<DisplayConfigurationOutput> output_configurations;
    for (auto const& output : outputs)
    {
        output_configurations.push_back(*output->config);
    }
    return std::make_unique<mgx::DisplayConfiguration>(output_configurations);
}

void mgx::Display::configure(mg::DisplayConfiguration const& new_configuration)
{
    std::lock_guard<std::mutex> lock{mutex};

    if (!new_configuration.valid())
    {
        BOOST_THROW_EXCEPTION(
            std::logic_error("Invalid or inconsistent display configuration"));
    }

    new_configuration.for_each_output([&](DisplayConfigurationOutput const& conf_output)
    {
        bool found_info = false;

        for (auto& output : outputs)
        {
            if (output->config->id == conf_output.id)
            {
                *output->config = conf_output;
                output->display_buffer->set_view_area(output->config->extents());
                output->display_buffer->set_transformation(output->config->transformation());
                found_info = true;
                break;
            }
        }

        if (!found_info)
            mir::log_error("Could not find info for output %d", conf_output.id.as_value());
    });
}

void mgx::Display::register_configuration_change_handler(
    EventHandlerRegister& /* event_handler*/,
    DisplayConfigurationChangeHandler const& change_handler)
{
    std::lock_guard<std::mutex> lock{mutex};
    config_change_handlers.push_back(change_handler);
}

void mgx::Display::register_pause_resume_handlers(
    EventHandlerRegister& /*handlers*/,
    DisplayPauseHandler const& /*pause_handler*/,
    DisplayResumeHandler const& /*resume_handler*/)
{
}

void mgx::Display::pause()
{
    BOOST_THROW_EXCEPTION(std::runtime_error("'Display::pause()' not yet supported on x11 platform"));
}

void mgx::Display::resume()
{
    BOOST_THROW_EXCEPTION(std::runtime_error("'Display::resume()' not yet supported on x11 platform"));
}

auto mgx::Display::create_hardware_cursor() -> std::shared_ptr<Cursor>
{
    return nullptr;
}

std::unique_ptr<mg::VirtualOutput> mgx::Display::create_virtual_output(int /*width*/, int /*height*/)
{
    return nullptr;
}

std::unique_ptr<mir::renderer::gl::Context> mgx::Display::create_gl_context() const
{
    return std::make_unique<XGLContext>(x_dpy, gl_config, shared_egl.context());
}

bool mgx::Display::apply_if_configuration_preserves_display_buffers(
    mg::DisplayConfiguration const& /*conf*/)
{
    return false;
}

mg::Frame mgx::Display::last_frame_on(unsigned) const
{
    return last_frame->load();
}

mgx::Display::OutputInfo::OutputInfo(
    Display* owner,
    std::unique_ptr<X11Window> window,
    std::unique_ptr<DisplayBuffer> display_buffer,
    std::shared_ptr<DisplayConfigurationOutput> configuration)
    : owner{owner},
      window{move(window)},
      display_buffer{move(display_buffer)},
      config{move(configuration)}
{
    mx::X11Resources::instance.set_set_output_for_window(*this->window, this);
}

mgx::Display::OutputInfo::~OutputInfo()
{
    mx::X11Resources::instance.clear_output_for_window(*window);
}

void mgx::Display::OutputInfo::set_size(geometry::Size const& size)
{
    std::unique_lock<std::mutex> lock{owner->mutex};
    if (config->modes[0].size == size)
    {
        return;
    }
    config->modes[0].size = size;
    display_buffer->set_view_area({display_buffer->view_area().top_left, size});
    auto const handlers = owner->config_change_handlers;

    lock.unlock();
    for (auto const& handler : handlers)
    {
        handler();
    }
}
