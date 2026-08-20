/*
 * main.c  -  ZrnPerformanceMgmnt entry point
 *
 * Usage:
 *   ./zrn_perfd                       TUI mode (default)
 *   ./zrn_perfd --daemon              headless / log-to-stdout mode
 *   ./zrn_perfd --gui                 GTK3 GUI mode (Openbox-compatible)
 *   ./zrn_perfd --allow-experimental-features
 *                                     enable neural-net prediction
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"
#include <getopt.h>
#include <sys/file.h>
#include <fcntl.h>

#ifdef ENABLE_GUI
#include <gtk/gtk.h>
#endif

/* Globals */
volatile sig_atomic_t g_running       = 1;
PerfMode              g_mode          = MODE_NOMINAL;
TrackedProc           g_procs[MAX_TRACKED];
int                   g_nprocs        = 0;
pid_t                 g_focused_pid   = 0;
int                   g_experimental  = 0;
int                   g_pairing_mode  = 0;
int                   g_tui_mode      = 1;
int                   g_gui_mode      = 0;
int                   g_monitor_only  = 0;

DynBoostState         g_dynboost;

static int s_lock_fd = -1;

static int try_acquire_lock(void)
{
    s_lock_fd = open(LOCK_FILE_PATH, O_RDWR | O_CREAT, 0600);
    if (s_lock_fd < 0) return 0; /* Could not open lockfile, assume master */

    if (flock(s_lock_fd, LOCK_EX | LOCK_NB) < 0) {
        /* Lock held by another process -> instance already running */
        close(s_lock_fd);
        s_lock_fd = -1;
        return 1;
    }

    /* Lock acquired -> we are master instance */
    ftruncate(s_lock_fd, 0);
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    if (len > 0) {
        if (write(s_lock_fd, buf, len) < 0) { /* ignore */ }
    }
    return 0;
}

static void release_lock(void)
{
    if (s_lock_fd >= 0) {
        flock(s_lock_fd, LOCK_UN);
        close(s_lock_fd);
        s_lock_fd = -1;
        unlink(LOCK_FILE_PATH);
    }
}

static void on_signal(int sig) { (void)sig; g_running = 0; }

static int x_error_handler(Display *dpy, XErrorEvent *ev)
{
    (void)dpy; (void)ev;
    return 0;
}

static struct timeval ms_to_timeval(int ms)
{
    return (struct timeval){
        .tv_sec  = ms / 1000,
        .tv_usec = (ms % 1000) * 1000,
    };
}

static const char *comm_for_pid(pid_t pid)
{
    int idx = proc_find(pid);
    if (idx >= 0) return g_procs[idx].comm;
    return "?";
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --daemon                     Run headless (no TUI, log to stdout)\n"
        "  --gui                        GTK3 graphical interface\n"
        "  --allow-experimental-features Enable neural-net window prediction\n"
        "  --predict, --status          Display next window predictions & RL probabilities\n"
        "  --set-limit comm:pct         Set CPU limit for process name (e.g. firefox:30)\n"
        "  --remove-limit comm          Remove CPU limit for process name\n"
        "  -h, --help                   Show this message\n",
        argv0);
}

int main(int argc, char *argv[])
{
    static struct option long_opts[] = {
        {"daemon",                       no_argument, NULL, 'd'},
        {"gui",                          no_argument, NULL, 'g'},
        {"allow-experimental-features",  no_argument, NULL, 'x'},
        {"predict",                      no_argument, NULL, 's'},
        {"status",                       no_argument, NULL, 's'},
        {"pairing",                      no_argument, NULL, 'p'},
        {"mode",                         required_argument, NULL, 'm'},
        {"set-limit",                    required_argument, NULL, 'l'},
        {"remove-limit",                 required_argument, NULL, 'r'},
        {"help",                         no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "dgxspm:l:r:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'd': g_tui_mode = 0; g_gui_mode = 0; break;
            case 'g': g_gui_mode = 1; g_tui_mode = 0; break;
            case 'x': g_experimental = 1; break;
            case 's': {
                g_experimental = 1;
                if (nn_load_weights(MODEL_WEIGHTS_PATH) != 0) {
                    fprintf(stderr, "[zrn] Could not load model weights from %s\n", MODEL_WEIGHTS_PATH);
                    return 1;
                }
                Display *dpy = XOpenDisplay(NULL);
                if (!dpy) {
                    fprintf(stderr, "[zrn] Cannot open X display.\n");
                    return 1;
                }
                nn_gpu_init(dpy);
                Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
                Atom net_wm_pid = XInternAtom(dpy, "_NET_WM_PID", True);
                Window root = DefaultRootWindow(dpy);
                Window act_win = xwin_active(dpy, root, net_active);
                pid_t act_pid = xwin_pid(dpy, act_win, net_wm_pid);

                char act_comm[64] = "unknown";
                if (act_pid > 0) {
                    char proc_path[64];
                    snprintf(proc_path, sizeof(proc_path), "/proc/%d/comm", (int)act_pid);
                    FILE *cf = fopen(proc_path, "r");
                    if (cf) {
                        if (fgets(act_comm, sizeof(act_comm), cf)) {
                            char *nl = strchr(act_comm, '\n');
                            if (nl) *nl = '\0';
                        }
                        fclose(cf);
                    }
                }

                char pred_comm[64] = "";
                nn_predict_next(act_comm, pred_comm, sizeof(pred_comm));

                printf("=== ZrnPerformanceMgmnt Window Predictor (GPU RL) ===\n");
                printf("  Focused Window:    %s (PID %d)\n", act_comm, (int)act_pid);
                printf("  Pre-warmed Target: %s\n", pred_comm[0] ? pred_comm : "None");
                printf("\n--- Next Window Probabilities ---\n");
                for (int i = 0; i < g_nn_state.count; i++) {
                    int bars = (int)(g_nn_state.top[i].prob * 20.0f);
                    char bar_str[24];
                    memset(bar_str, ' ', 20);
                    for (int b = 0; b < bars && b < 20; b++) bar_str[b] = '=';
                    bar_str[20] = '\0';
                    int is_pred = (strcmp(g_nn_state.top[i].comm, pred_comm) == 0);
                    printf("  %d. %-16.16s [%s] %5.1f%%%s\n",
                           i + 1, g_nn_state.top[i].comm, bar_str,
                           g_nn_state.top[i].prob * 100.0f,
                           is_pred ? "  <- [Pre-warmed]" : "");
                }
                printf("\n--- Reinforcement Learning Online Status ---\n");
                if (g_nn_state.total_predictions > 0) {
                    float acc = (float)g_nn_state.total_correct / (float)g_nn_state.total_predictions * 100.0f;
                    printf("  Last Feedback:     %s (Target: %s, Pred: %s, Loss: %.4f)\n",
                           g_nn_state.last_reward > 0 ? "Reward (+1.0 Correct)" : "Penalty (-1.0 Corrected)",
                           g_nn_state.last_actual, g_nn_state.last_predicted, g_nn_state.last_loss);
                    printf("  Online Accuracy:   %.1f%% (%d / %d)\n",
                           acc, g_nn_state.total_correct, g_nn_state.total_predictions);
                } else {
                    printf("  Model online and active (weights loaded from %s)\n", MODEL_WEIGHTS_PATH);
                }
                nn_gpu_shutdown();
                XCloseDisplay(dpy);
                return 0;
            }
            case 'p': g_pairing_mode = 1; break;
            case 'l': {
                char comm[64];
                int pct = 0;
                if (sscanf(optarg, "%63[^:]:%d", comm, &pct) == 2) {
                    limits_load(LIMITS_PATH);
                    limit_add(comm, pct);
                    printf("CPU limit for %s set to %d%%\n", comm, pct);
                } else {
                    fprintf(stderr, "Error: format must be comm:percent (e.g. alacritty:50)\n");
                }
                return 0;
            }
            case 'r':
                limits_load(LIMITS_PATH);
                limit_remove(optarg);
                printf("CPU limit for %s removed.\n", optarg);
                return 0;
            case 'm':
                if (strcasecmp(optarg, "none") == 0) g_mode = MODE_NONE;
                else if (strcasecmp(optarg, "nominal") == 0) g_mode = MODE_NOMINAL;
                else if (strcasecmp(optarg, "moderate") == 0) g_mode = MODE_MODERATE;
                else if (strcasecmp(optarg, "high") == 0) g_mode = MODE_HIGH;
                FILE *f = fopen(PROFILE_PATH, "w");
                if (f) { fprintf(f, "%s\n", mode_name(g_mode)); fclose(f); }
                break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    /* Check if an instance is already running (e.g. systemd daemon) */
    if (try_acquire_lock() != 0) {
        g_monitor_only = 1;
        g_gui_mode = 1;
        g_tui_mode = 0;
        printf("[zrn] existing daemon instance detected. Starting GUI monitor.\n");
        profile_reload();
    } else {
        profile_init();
    }

    /* Load configuration */
    exempt_load(EXEMPT_PATH);
    limits_load(LIMITS_PATH);

    if (g_experimental && !g_monitor_only) {
        if (nn_load_weights(MODEL_WEIGHTS_PATH) == 0) {
            if (!g_tui_mode && !g_gui_mode)
                printf("[zrn] neural-net weights loaded from %s\n", MODEL_WEIGHTS_PATH);
        } else {
            if (!g_tui_mode && !g_gui_mode)
                printf("[zrn] no trained model found, data collection only\n");
        }
    }

    if (!g_tui_mode && !g_gui_mode)
        printf("[zrn] starting  mode=%s  experimental=%s\n",
               mode_name(g_mode), g_experimental ? "yes" : "no");

    /* Open X display */
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[zrn] cannot open display (is DISPLAY set?)\n");
        if (!g_monitor_only) release_lock();
        return 1;
    }
    XSetErrorHandler(x_error_handler);

    Window root = DefaultRootWindow(dpy);

    Atom net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    Atom net_wm_pid        = XInternAtom(dpy, "_NET_WM_PID",        False);
    Atom net_client_list   = XInternAtom(dpy, "_NET_CLIENT_LIST",   False);

    XSelectInput(dpy, root, PropertyChangeMask);
    XFlush(dpy);

    /* Signal handling */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    /* Initialize background systems */
    audio_init();
    hddmon_init();
    cpu_power_init();
    dynboost_init(dpy);
    if (g_experimental) {
        nn_gpu_init(dpy);
    }
    if (!g_monitor_only) {
        throttle_init();
    }

    /* Seed tracker */
    xwin_scan_existing(dpy, root, net_wm_pid, net_client_list);

    /* Initial focus */
    Window focused_win = xwin_active(dpy, root, net_active_window);
    g_focused_pid      = xwin_pid(dpy, focused_win, net_wm_pid);
    time_t focus_start  = time(NULL);

    if (g_experimental) {
        const char *init_comm = comm_for_pid(g_focused_pid);
        if (init_comm && init_comm[0] != '?') {
            nn_predict_next(init_comm, g_predicted_comm, sizeof(g_predicted_comm));
        }
    }

    int x_fd = ConnectionNumber(dpy);

    time_t last_profile_reload = time(NULL);
    time_t last_sys_check = time(NULL);
    PerfMode last_sys_mode = profile_detect_system();
    time_t last_dead_sweep     = time(NULL);
    time_t last_gpu_scan       = time(NULL);
    time_t last_pair_update    = time(NULL);
    time_t last_audio_scan     = time(NULL);
    time_t last_dynboost_scan  = time(NULL);
    time_t last_hdd_scan       = time(NULL);
    time_t last_cpu_power_scan = time(NULL);

    /* Init UI */
    if (g_tui_mode) {
        tui_init();
    } else if (g_gui_mode) {
        gui_init(&argc, &argv);
    }

    /* ---- main event loop ---- */
    while (g_running) {

        /* Drain X events */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            if (ev.type != PropertyNotify)   continue;
            if (ev.xproperty.window != root) continue;

            Atom changed = ev.xproperty.atom;

            if (changed == net_active_window) {
                Window cur  = xwin_active(dpy, root, net_active_window);
                pid_t  cpid = xwin_pid(dpy, cur, net_wm_pid);

                if (cpid != g_focused_pid && cpid > 0) {
                    time_t now = time(NULL);
                    long duration_ms = (long)(now - focus_start) * 1000L;

                    const char *from_comm = comm_for_pid(g_focused_pid);
                    const char *to_comm   = comm_for_pid(cpid);

                    if (proc_find(cpid) < 0) {
                        proc_add(cpid);
                        to_comm = comm_for_pid(cpid);
                    }

                    if (!g_monitor_only) {
                        switch_record(g_focused_pid, from_comm,
                                      cpid, to_comm, duration_ms);
                        if (g_experimental) {
                            nn_feedback(to_comm);
                            nn_predict_next(to_comm, g_predicted_comm, sizeof(g_predicted_comm));
                        }
                    }

                    if (!g_tui_mode && !g_gui_mode)
                        printf("[zrn] focus: pid=%d (%s) -> pid=%d (%s)  "
                                "duration=%ldms\n",
                                (int)g_focused_pid, from_comm,
                                (int)cpid, to_comm, duration_ms);

                    g_focused_pid = cpid;
                    focus_start   = now;
                }
            }

            if (changed == net_client_list) {
                xwin_scan_existing(dpy, root, net_wm_pid, net_client_list);
            }
        }

        /* TUI input is handled inside tui_draw() */
        /* GTK3 GUI: pump pending events (non-blocking) */
#ifdef ENABLE_GUI
        if (g_gui_mode) {
            while (gtk_events_pending())
                gtk_main_iteration_do(FALSE);
        }
#endif

        /* Block on X socket with timeout */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(x_fd, &fds);

        struct timeval tv = ms_to_timeval(THROTTLE_QUANTUM_MS);
        int ret = select(x_fd + 1, &fds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* ---- periodic work ---- */
        time_t now = time(NULL);

        if (!g_monitor_only) {
            /* System power profile monitor */
            if (now - last_sys_check >= 2) {
                PerfMode sys_mode = profile_detect_system();
                if (sys_mode != last_sys_mode) {
                    last_sys_mode = sys_mode;
                    g_mode = sys_mode;
                    throttle_unthrottle_all();
                    
                    FILE *f = fopen(PROFILE_PATH, "w");
                    if (f) { fprintf(f, "%s\n", mode_name(g_mode)); fclose(f); }
                    
                    if (!g_tui_mode && !g_gui_mode) {
                        printf("[zrn] system power profile changed, new mode: %s\n", mode_name(g_mode));
                    }
                }
                last_sys_check = now;
            }

            /* Profile reload */
            if (now - last_profile_reload >= PROFILE_RELOAD_SEC) {
                if (!g_tui_mode && !g_gui_mode) {
                    PerfMode old_mode = g_mode;
                    profile_reload();
                    if (old_mode != g_mode) {
                        printf("[zrn] mode changed: %s -> %s\n",
                               mode_name(old_mode), mode_name(g_mode));
                        throttle_unthrottle_all();
                    }
                }
                exempt_load(EXEMPT_PATH);
    limits_load(LIMITS_PATH);
                last_profile_reload = now;
            }
        } else {
            /* Monitor mode: keep profile and exemptions in sync with disk */
            if (now - last_profile_reload >= PROFILE_RELOAD_SEC) {
                profile_reload();
                exempt_load(EXEMPT_PATH);
    limits_load(LIMITS_PATH);
                last_profile_reload = now;
            }
        }

        /* Re-scan tracking table (remove dead processes) */
        if (now - last_dead_sweep >= 2) {
            proc_remove_dead();
            proctrack_update_descendants();
            last_dead_sweep = now;
        }

        /* GPU scan */
        if (now - last_gpu_scan >= GPU_SCAN_INTERVAL_SEC) {
            gpu_scan();
            last_gpu_scan = now;
        }

        /* Audio scan */
        if (now - last_audio_scan >= AUDIO_SCAN_INTERVAL_SEC) {
            audio_scan();
            last_audio_scan = now;
        }

        /* Update frequent-switch pairs */
        if (g_pairing_mode && (now - last_pair_update >= 2)) {
            switch_update_pairs();
            last_pair_update = now;
        }

        if (!g_monitor_only) {
            /* Apply throttle policy */
            throttle_apply(g_focused_pid);
            limits_apply_all();

            /* Apply GPU throttle */
            gpu_throttle_apply(g_focused_pid);
        } else {
            /* Monitor mode: compute throttle state for display purposes only.
             * We call throttle_apply() to evaluate the policy (sets
             * should_throttle, defocus_time, prewarm) but since the worker
             * thread isn't running we directly mirror should_throttle into
             * throttled so the GUI/TUI shows the correct status.             */
            throttle_apply(g_focused_pid);
            limits_apply_all();
            gpu_throttle_apply(g_focused_pid);
            for (int i = 0; i < g_nprocs; i++)
                g_procs[i].throttled = g_procs[i].should_throttle;
        }

        /* Dynamic Boost */
        if (now - last_dynboost_scan >= DYNBOOST_SCAN_SEC) {
            dynboost_tick(g_focused_pid);
            last_dynboost_scan = now;
        }

        /* HDD spin-down (battery only) */
        if (now - last_hdd_scan >= HDD_SCAN_INTERVAL_SEC) {
            hddmon_tick();
            last_hdd_scan = now;
        }

        /* CPU governor/frequency by AC/battery state */
        if (now - last_cpu_power_scan >= CPU_POWER_SCAN_INTERVAL_SEC) {
            cpu_power_tick();
            last_cpu_power_scan = now;
        }

        /* TUI redraw */
        if (g_tui_mode) {
            tui_draw(dpy, root, net_wm_pid, net_active_window);
        }
    }

    /* ---- clean shutdown ---- */
    if (g_tui_mode) tui_shutdown();
    if (g_gui_mode) gui_shutdown();

    dynboost_shutdown();
    if (g_experimental) {
        nn_gpu_shutdown();
    }

    if (g_monitor_only) {
        audio_cleanup();
        XCloseDisplay(dpy);
        return 0;
    }

    printf("\n[zrn] shutting down - resuming all processes\n");
    fflush(stdout);
    throttle_unthrottle_all();
    gpu_unthrottle_all();
    throttle_shutdown();
    audio_cleanup();
    release_lock();
    XCloseDisplay(dpy);
    return 0;
}
