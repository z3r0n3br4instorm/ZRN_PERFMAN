/*
 * xwatch.c  –  X11 window / event helpers
 *
 * Wraps the X11 property queries from the demo and adds a full window-list
 * scan so we can seed the tracker with already-open GUI apps on startup.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"

/* Thin wrapper over XGetWindowProperty. Returns a malloc'd XFree()-able
 * buffer or NULL.  Caller must XFree() the returned pointer. */
static void *get_property(Display *dpy, Window win, Atom prop,
                           Atom expected_type, unsigned long *n_items)
{
    Atom          actual_type;
    int           actual_format;
    unsigned long bytes_after;
    unsigned char *data = NULL;

    int st = XGetWindowProperty(dpy, win, prop,
                                0L, 1024L, False, expected_type,
                                &actual_type, &actual_format,
                                n_items, &bytes_after, &data);

    if (st != Success || actual_type != expected_type || !data) {
        if (data) XFree(data);
        return NULL;
    }
    return data;
}

/* Fetch _NET_WM_PID from a window. Returns 0 if not set. */
pid_t xwin_pid(Display *dpy, Window win, Atom net_wm_pid)
{
    unsigned long n = 0;
    unsigned long *val = get_property(dpy, win, net_wm_pid, XA_CARDINAL, &n);
    if (!val || n == 0) {
        if (val) XFree(val);
        return 0;
    }
    pid_t pid = (pid_t)val[0];
    XFree(val);
    return pid;
}

/* Return the current _NET_ACTIVE_WINDOW, or None. */
Window xwin_active(Display *dpy, Window root, Atom net_active_window)
{
    unsigned long n = 0;
    Window *val = get_property(dpy, root, net_active_window, XA_WINDOW, &n);
    if (!val || n == 0) {
        if (val) XFree(val);
        return None;
    }
    Window w = val[0];
    XFree(val);
    return w;
}

/*
 * Scan _NET_CLIENT_LIST on the root window and add every PID found to the
 * tracker.  Called once at startup so already-running apps are tracked.
 */
void xwin_scan_existing(Display *dpy, Window root,
                        Atom net_wm_pid, Atom net_client_list)
{
    unsigned long n = 0;
    Window *wins = get_property(dpy, root, net_client_list, XA_WINDOW, &n);
    if (!wins) return;

    if (!g_tui_mode && !g_gui_mode) {
        printf("[xwatch] scanning %lu existing client windows\n", n);
        fflush(stdout);
    }

    for (unsigned long i = 0; i < n; i++) {
        pid_t pid = xwin_pid(dpy, wins[i], net_wm_pid);
        if (pid > 0)
            proc_add(pid);   /* ignores duplicates silently */
    }

    XFree(wins);
}
