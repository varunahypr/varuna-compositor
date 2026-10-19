// Drag icons in the right place.
//
// wl_surface offsets are deltas: GTK sets a drag icon's hotspot once, with
// an offset of minus the hotspot, and later buffers carry no offset. But
// Hyprland (0.56) keeps a surface's *last* offset change as its offset, so
// the first redraw of the icon (e.g. at the monitor's fractional scale) puts
// it back to 0 and the icon jumps to the pointer's bottom-right. This keeps
// the running total on the drag surface, which renderDND then uses.
#pragma once

namespace dnd {

static WP<CWLSurfaceResource> g_surface;
static Vector2D               g_total;
static CHyprSignalListener    g_commit;

// Called every frame: notice a new drag icon and follow its commits.
static void track() {
    if (!PROTO::data)
        return;
    const auto surface = PROTO::data->m_dnd.dndSurface.lock();
    if (surface.get() == g_surface.lock().get())
        return;
    g_surface = surface;
    g_commit.reset();
    if (!surface)
        return;
    // Its first commit (with the hotspot) has already been applied.
    g_total  = surface->m_current.offset;
    g_commit = surface->m_events.commit.listen([] {
        const auto s = g_surface.lock();
        if (!s || !s->m_current.updated.bits.offset)
            return;
        g_total += s->m_current.offset;
        s->m_current.offset = g_total;
    });
}

static void stop() {
    g_commit.reset();
    g_surface.reset();
}

} // namespace dnd
