#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}


static int x_error_handler(Display *dpy, XErrorEvent *ev)
{
    (void)dpy;
    (void)ev;
    return 0;
}

/*
 * Thin wrapper over XGetWindowProperty. Returns a malloc'd buffer that the
 * caller frees with XFree(), or NULL. *n_items receives the element count.
 */
static void *get_property(Display *dpy, Window win, Atom prop,
                          Atom expected_type, unsigned long *n_items)
{
    Atom actual_type;
    int actual_format;
    unsigned long bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(dpy, win, prop,
                                    0L, 1024L, False, expected_type,
                                    &actual_type, &actual_format,
                                    n_items, &bytes_after, &data);

    if (status != Success || actual_type != expected_type || data == NULL) {
        if (data) XFree(data);
        return NULL;
    }
    return data;
}

/* Fetch _NET_WM_PID from a client window. Returns 0 if unset. */
static pid_t window_pid(Display *dpy, Window win, Atom net_wm_pid)
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

/* Best-effort window title: prefer _NET_WM_NAME (UTF-8), fall back to WM_NAME. */
static char *window_title(Display *dpy, Window win, Atom net_wm_name, Atom utf8)
{
    unsigned long n = 0;
    char *data = get_property(dpy, win, net_wm_name, utf8, &n);
    if (data && n > 0) {
        char *copy = strdup(data);
        XFree(data);
        return copy;
    }
    if (data) XFree(data);

    char *legacy = NULL;
    if (XFetchName(dpy, win, &legacy) && legacy) {
        char *copy = strdup(legacy);
        XFree(legacy);
        return copy;
    }
    return NULL;
}

/* Read /proc/<pid>/comm for a human-readable process name. */
static int process_name(pid_t pid, char *out, size_t out_len)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    if (!fgets(out, (int)out_len, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    out[strcspn(out, "\n")] = '\0';
    return 0;
}

static Window active_window(Display *dpy, Window root, Atom net_active_window)
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

static void report(Display *dpy, Window win, Atom net_wm_pid,
                   Atom net_wm_name, Atom utf8)
{
    if (win == None) {
        printf("focus: (none)\n");
        fflush(stdout);
        return;
    }

    pid_t pid = window_pid(dpy, win, net_wm_pid);
    char *title = window_title(dpy, win, net_wm_name, utf8);
    char comm[256] = "?";

    if (pid > 0) process_name(pid, comm, sizeof(comm));

    if (pid > 0)
        printf("focus: pid=%d  proc=%-16s  win=0x%lx  title=%s\n",
               (int)pid, comm, (unsigned long)win, title ? title : "(untitled)");
    else
        printf("focus: pid=?     proc=%-16s  win=0x%lx  title=%s  "
               "[_NET_WM_PID not set]\n",
               "?", (unsigned long)win, title ? title : "(untitled)");

    fflush(stdout);
    free(title);
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "focuswatch: cannot open display (is DISPLAY set?)\n");
        return 1;
    }

    XSetErrorHandler(x_error_handler);

    Window root = DefaultRootWindow(dpy);

    Atom net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    Atom net_wm_pid        = XInternAtom(dpy, "_NET_WM_PID",        False);
    Atom net_wm_name       = XInternAtom(dpy, "_NET_WM_NAME",       False);
    Atom utf8              = XInternAtom(dpy, "UTF8_STRING",        False);

    /* Ask the server to notify us whenever a root-window property changes. */
    XSelectInput(dpy, root, PropertyChangeMask);
    XFlush(dpy);

    /* Clean shutdown on Ctrl-C / SIGTERM. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int fd = ConnectionNumber(dpy);
    Window last = None;

    /* Print the current state once so the first line isn't a surprise. */
    last = active_window(dpy, root, net_active_window);
    report(dpy, last, net_wm_pid, net_wm_name, utf8);

    while (g_running) {
        /*
         * Drain anything already queued before sleeping, otherwise events
         * buffered client-side would sit unnoticed until the next wakeup.
         */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            if (ev.type != PropertyNotify)                continue;
            if (ev.xproperty.window != root)              continue;
            if (ev.xproperty.atom != net_active_window)   continue;

            Window cur = active_window(dpy, root, net_active_window);
            if (cur == last) continue;   /* WMs re-set the property spuriously */

            last = cur;
            report(dpy, cur, net_wm_pid, net_wm_name, utf8);
        }

        /* Block until the X socket has something for us. Zero CPU meanwhile. */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);

        if (select(fd + 1, &fds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;   /* our signal handler fired */
            perror("focuswatch: select");
            break;
        }
    }

    printf("\nfocuswatch: exiting\n");
    XCloseDisplay(dpy);
    return 0;
}
