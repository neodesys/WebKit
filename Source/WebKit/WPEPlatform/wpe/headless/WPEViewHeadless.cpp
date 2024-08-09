/*
 * Copyright (C) 2023 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WPEViewHeadless.h"

#include "WPEToplevelHeadless.h"
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/RunLoopSourcePriority.h>
#include <wtf/glib/WTFGType.h>
#include <math.h>

/**
 * WPEViewHeadless:
 *
 */
struct _WPEViewHeadlessPrivate {
    GRefPtr<WPEBuffer> pendingBuffer;
    GRefPtr<WPEBuffer> committedBuffer;
    GRefPtr<GSource> frameSource;
    gint64 lastFrameTime;
    gfloat fps;
};
WEBKIT_DEFINE_FINAL_TYPE(WPEViewHeadless, wpe_view_headless, WPE_TYPE_VIEW, WPEView)

enum {
    PROP_0,
    PROP_FPS,
    N_PROPERTIES
};

#define PROP_FPS_DEFAULT 60.f
#define PROP_FPS_MIN 0.00001f

static GParamSpec* sObjProperties[N_PROPERTIES] = { nullptr, };

enum {
    BUFFER_AVAILABLE,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

static GSourceFuncs frameSourceFuncs = {
    nullptr, // prepare
    nullptr, // check
    // dispatch
    [](GSource* source, GSourceFunc callback, gpointer userData) -> gboolean
    {
        if (g_source_get_ready_time(source) == -1)
            return G_SOURCE_CONTINUE;
        g_source_set_ready_time(source, -1);
        return callback(userData);
    },
    nullptr, // finalize
    nullptr, // closure_callback
    nullptr, // closure_marshall
};

static void wpeViewHeadlessConstructed(GObject* object)
{
    G_OBJECT_CLASS(wpe_view_headless_parent_class)->constructed(object);

    auto* view = WPE_VIEW(object);
    g_signal_connect(view, "notify::toplevel", G_CALLBACK(+[](WPEView* view, GParamSpec*, gpointer) {
        auto* toplevel = wpe_view_get_toplevel(view);
        if (!toplevel) {
            wpe_view_unmap(view);
            return;
        }

        int width;
        int height;
        wpe_toplevel_get_size(toplevel, &width, &height);
        if (width && height)
            wpe_view_resized(view, width, height);

        wpe_view_map(view);
    }), nullptr);

    auto* priv = WPE_VIEW_HEADLESS(view)->priv;
    priv->fps = PROP_FPS_DEFAULT;
    priv->frameSource = adoptGRef(g_source_new(&frameSourceFuncs, sizeof(GSource)));
    g_source_set_priority(priv->frameSource.get(), RunLoopSourcePriority::RunLoopTimer);
    g_source_set_name(priv->frameSource.get(), "WPE headless frame timer");
    g_source_set_callback(priv->frameSource.get(), [](gpointer userData) -> gboolean {
        auto* view = WPE_VIEW(userData);
        auto* priv = WPE_VIEW_HEADLESS(view)->priv;
        if (priv->committedBuffer)
            wpe_view_buffer_released(view, priv->committedBuffer.get());
        priv->committedBuffer = WTFMove(priv->pendingBuffer);
        wpe_view_buffer_rendered(view, priv->committedBuffer.get());
        priv->lastFrameTime = g_get_monotonic_time();

        if (g_source_is_destroyed(priv->frameSource.get()))
            return G_SOURCE_REMOVE;
        return G_SOURCE_CONTINUE;
    }, object, nullptr);
    g_source_attach(priv->frameSource.get(), g_main_context_get_thread_default());
    g_source_set_ready_time(priv->frameSource.get(), -1);
}

static void wpeViewHeadlessSetProperty(GObject* object, guint propId, const GValue* value, GParamSpec* paramSpec)
{
    auto* priv = WPE_VIEW_HEADLESS(object)->priv;

    switch (propId) {
    case PROP_FPS:
        priv->fps = g_value_get_float(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, propId, paramSpec);
        break;
    }
}

static void wpeViewHeadlessGetProperty(GObject* object, guint propId, GValue* value, GParamSpec* paramSpec)
{
    auto* priv = WPE_VIEW_HEADLESS(object)->priv;

    switch (propId) {
    case PROP_FPS:
        g_value_set_float(value, priv->fps);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, propId, paramSpec);
        break;
    }
}

static void wpeViewHeadlessDispose(GObject* object)
{
    auto* priv = WPE_VIEW_HEADLESS(object)->priv;

    if (priv->frameSource) {
        g_source_destroy(priv->frameSource.get());
        priv->frameSource = nullptr;
    }

    G_OBJECT_CLASS(wpe_view_headless_parent_class)->dispose(object);
}

static gboolean wpeViewHeadlessRenderBuffer(WPEView* view, WPEBuffer* buffer, const WPERectangle*, guint, GError**)
{
    auto* priv = WPE_VIEW_HEADLESS(view)->priv;
    priv->pendingBuffer = buffer;

    gint64 nextFrameTime = -1;
    gfloat fps = priv->fps;
    if (fps >= PROP_FPS_MIN) {
        gint64 frameDuration = roundf(G_USEC_PER_SEC / fps);
        if (priv->lastFrameTime)
            nextFrameTime = priv->lastFrameTime + frameDuration;
        else
            nextFrameTime = g_get_monotonic_time() + frameDuration;
    }

    g_signal_emit(view, signals[BUFFER_AVAILABLE], 0, buffer);

    if (nextFrameTime <= g_get_monotonic_time())
        g_source_set_ready_time(priv->frameSource.get(), 0);
    else
        g_source_set_ready_time(priv->frameSource.get(), nextFrameTime);

    return TRUE;
}

static void wpe_view_headless_class_init(WPEViewHeadlessClass* viewHeadlessClass)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(viewHeadlessClass);
    objectClass->constructed = wpeViewHeadlessConstructed;
    objectClass->set_property = wpeViewHeadlessSetProperty;
    objectClass->get_property = wpeViewHeadlessGetProperty;
    objectClass->dispose = wpeViewHeadlessDispose;

    WPEViewClass* viewClass = WPE_VIEW_CLASS(viewHeadlessClass);
    viewClass->render_buffer = wpeViewHeadlessRenderBuffer;

    /**
     * WPEViewHeadless::fps:
     *
     * The rendering targeted number of frames per second, set to 0 to render
     * as fast as possible.
     */
    sObjProperties[PROP_FPS] = g_param_spec_float(
        "fps",
        nullptr, nullptr,
        0.f, G_MAXFLOAT, PROP_FPS_DEFAULT,
        WEBKIT_PARAM_READWRITE);

    g_object_class_install_properties(objectClass, N_PROPERTIES, sObjProperties);

    /**
     * WPEViewHeadless::buffer-available:
     * @view: a #WPEView
     * @buffer: a #WPEBuffer
     *
     * Emitted to notify that a new frame buffer is available.
     */
    signals[BUFFER_AVAILABLE] = g_signal_new(
        "buffer-available",
        G_TYPE_FROM_CLASS(viewClass),
        G_SIGNAL_RUN_LAST,
        0, nullptr, nullptr,
        g_cclosure_marshal_generic,
        G_TYPE_NONE, 1,
        WPE_TYPE_BUFFER);
}

/**
 * wpe_view_headless_new:
 * @display: a #WPEDisplayHeadless
 *
 * Create a new #WPEViewHeadless
 *
 * Returns: (transfer full): a #WPEView
 */
WPEView* wpe_view_headless_new(WPEDisplayHeadless* display)
{
    g_return_val_if_fail(WPE_IS_DISPLAY_HEADLESS(display), nullptr);

    return WPE_VIEW(g_object_new(WPE_TYPE_VIEW_HEADLESS, "display", display, nullptr));
}
