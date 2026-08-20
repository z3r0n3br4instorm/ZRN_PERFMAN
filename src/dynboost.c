/*
 * dynboost.c  -  Dynamic Boost System (BoostML + RapidBoost)
 *
 * BoostML  (experimental):  logs process CPU data for future ML-based
 *          frequency control.  GPU inference infrastructure via OpenGL 3.3
 *          on Intel HD3000 is set up but model is currently stubbed.
 *
 * RapidBoost (always available):  when the focused app saturates the CPU
 *          in power-saving mode, temporarily boost CPU to 50 % of max freq,
 *          turn off keyboard backlight, dim display to 25 %, and notify.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define GL_GLEXT_PROTOTYPES

#include "zrn_perf.h"
#include <dirent.h>
#include <ctype.h>
#include <glob.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glext.h>

/* ========================================================================== */
/* Global state                                                                */
/* ========================================================================== */
// g_dynboost is defined in main.c                       /* zero-initialised            */

/* -------------------------------------------------------------------------- */
/* Per-process CPU tracking (internal)                                         */
/* -------------------------------------------------------------------------- */
typedef struct {
    pid_t              pid;
    unsigned long long prev_total;               /* utime + stime ticks        */
    struct timespec    prev_ts;
    int                valid;
} CpuTrack;

static CpuTrack s_cpu_track[MAX_TRACKED];
static int      s_ncpu_track = 0;

/* Focused PID at the moment RapidBoost activated (for focus-change detect)   */
static pid_t    s_rb_focused_pid = 0;

/* -------------------------------------------------------------------------- */
/* OpenGL 3.3 state (BoostML GPU inference infrastructure)                     */
/* -------------------------------------------------------------------------- */
static Display    *s_gl_dpy  = NULL;
static GLXContext  s_gl_ctx  = 0;
static GLXPbuffer  s_gl_pbuf = 0;
static GLuint      s_nn_program = 0;
static GLuint      s_nn_vao     = 0;
static GLuint      s_nn_fbo     = 0;

/* ========================================================================== */
/*  Sysfs I/O helpers                                                          */
/* ========================================================================== */
static int sysfs_read_ulong(const char *path, unsigned long *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = (fscanf(f, "%lu", out) == 1);
    fclose(f);
    return ok ? 0 : -1;
}

static int sysfs_write_ulong(const char *path, unsigned long val)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%lu\n", val);
    fclose(f);
    return 0;
}

static int sysfs_read_int(const char *path, int *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = (fscanf(f, "%d", out) == 1);
    fclose(f);
    return ok ? 0 : -1;
}

static int sysfs_write_int(const char *path, int val)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", val);
    fclose(f);
    return 0;
}

/* ========================================================================== */
/*  Sysfs path discovery                                                       */
/* ========================================================================== */
static void discover_kbd_backlight(void)
{
    glob_t gl;
    memset(&gl, 0, sizeof(gl));
    if (glob("/sys/class/leds/*kbd_backlight/brightness", 0, NULL, &gl) == 0
        && gl.gl_pathc > 0) {
        strncpy(g_dynboost.kbd_brightness_path, gl.gl_pathv[0],
                sizeof(g_dynboost.kbd_brightness_path) - 1);
    }
    globfree(&gl);
}

static void discover_display_backlight(void)
{
    const char *patterns[] = {
        "/sys/class/backlight/intel_backlight/brightness",
        "/sys/class/backlight/*/brightness",
        NULL
    };

    for (int i = 0; patterns[i]; i++) {
        glob_t gl;
        memset(&gl, 0, sizeof(gl));
        if (glob(patterns[i], 0, NULL, &gl) == 0 && gl.gl_pathc > 0) {
            strncpy(g_dynboost.disp_brightness_path, gl.gl_pathv[0],
                    sizeof(g_dynboost.disp_brightness_path) - 1);

            /* Derive the max_brightness path from the same directory */
            char *last_slash = strrchr(g_dynboost.disp_brightness_path, '/');
            if (last_slash) {
                size_t dir_len = (size_t)(last_slash - g_dynboost.disp_brightness_path);
                snprintf(g_dynboost.disp_max_brightness_path,
                         sizeof(g_dynboost.disp_max_brightness_path),
                         "%.*s/max_brightness", (int)dir_len,
                         g_dynboost.disp_brightness_path);
                sysfs_read_int(g_dynboost.disp_max_brightness_path,
                               &g_dynboost.disp_max_brightness);
            }
            globfree(&gl);
            return;
        }
        globfree(&gl);
    }
}

/* ========================================================================== */
/*  CPU frequency helpers                                                      */
/* ========================================================================== */
static void read_cpu_freqs(void)
{
    sysfs_read_ulong("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq",
                     &g_dynboost.base_freq_khz);
    sysfs_read_ulong("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq",
                     &g_dynboost.max_freq_khz);
    sysfs_read_ulong("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
                     &g_dynboost.cur_freq_khz);
}

static void set_all_cores_max_freq(unsigned long freq_khz)
{
    if (g_monitor_only) return;
    char path[128];
    for (int i = 0; i < 256; i++) {
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", i);
        if (sysfs_write_ulong(path, freq_khz) < 0) break;
    }
}

/* ========================================================================== */
/*  Per-process CPU usage computation                                          */
/* ========================================================================== */
static unsigned long long read_proc_cpu_ticks(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    unsigned long long utime = 0, stime = 0;
    char buf[1024];
    if (fgets(buf, sizeof(buf), f)) {
        /* skip past "(comm)" — find the LAST ')' */
        char *p = strrchr(buf, ')');
        if (p) {
            p += 2;                              /* skip ") " */
            /* fields: state ppid pgrp session tty tpgid flags
             *         minflt cminflt majflt cmajflt utime stime */
            unsigned long d;
            char state;
            if (sscanf(p,
                       "%c %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %llu %llu",
                       &state, &d, &d, &d, &d, &d, &d,
                       &d, &d, &d, &d, &utime, &stime) < 13) {
                utime = 0;
                stime = 0;
            }
        }
    }
    fclose(f);
    return utime + stime;
}

static void update_cpu_usage(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;

    for (int i = 0; i < g_nprocs; i++) {
        TrackedProc *p = &g_procs[i];
        unsigned long long total = read_proc_cpu_ticks(p->pid);

        /* Find or create tracking entry */
        CpuTrack *ct = NULL;
        for (int j = 0; j < s_ncpu_track; j++) {
            if (s_cpu_track[j].pid == p->pid) { ct = &s_cpu_track[j]; break; }
        }

        if (!ct) {
            if (s_ncpu_track >= MAX_TRACKED) { p->cpu_pct = 0.0; continue; }
            ct = &s_cpu_track[s_ncpu_track++];
            ct->pid        = p->pid;
            ct->prev_total = total;
            ct->prev_ts    = now;
            ct->valid      = 0;
            p->cpu_pct     = 0.0;
            continue;
        }

        if (!ct->valid) {
            ct->prev_total = total;
            ct->prev_ts    = now;
            ct->valid      = 1;
            p->cpu_pct     = 0.0;
            continue;
        }

        double dt = (double)(now.tv_sec  - ct->prev_ts.tv_sec)
                  + (double)(now.tv_nsec - ct->prev_ts.tv_nsec) / 1e9;
        if (dt > 0.1) {
            unsigned long long delta = total - ct->prev_total;
            p->cpu_pct     = ((double)delta / (dt * (double)clk_tck)) * 100.0;
            ct->prev_total = total;
            ct->prev_ts    = now;
        }
    }

    /* Remove stale tracking entries for dead PIDs */
    int j = 0;
    while (j < s_ncpu_track) {
        int found = 0;
        for (int i = 0; i < g_nprocs; i++) {
            if (g_procs[i].pid == s_cpu_track[j].pid) { found = 1; break; }
        }
        if (!found)
            s_cpu_track[j] = s_cpu_track[--s_ncpu_track];
        else
            j++;
    }
}

/* ========================================================================== */
/*  Desktop notification (LXDE-compatible via notify-send)                     */
/* ========================================================================== */
void dynboost_notify(const char *summary, const char *body)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* Double-fork to avoid zombies */
        if (fork() == 0) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
            execlp("notify-send", "notify-send",
                   "-u", "normal",
                   "-a", "ZrnPerformanceMgmnt",
                   summary, body, (char *)NULL);
            _exit(1);
        }
        _exit(0);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

/* ========================================================================== */
/*  RapidBoost activate / deactivate                                           */
/* ========================================================================== */
static void rapidboost_activate(int on_ac)
{
    if (g_dynboost.rapidboost_active) return;

    /* --- save current state --- */
    sysfs_read_ulong("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
                     &g_dynboost.saved_scaling_max_khz);

    g_dynboost.rapidboost_on_ac = on_ac;

    if (!on_ac) {
        /* On battery: memorize current user brightness */
        int cur_disp = -1, cur_kbd = -1;
        if (g_dynboost.disp_brightness_path[0] && sysfs_read_int(g_dynboost.disp_brightness_path, &cur_disp) == 0 && cur_disp >= 0) {
            g_dynboost.saved_disp_brightness = cur_disp;
        }
        if (g_dynboost.kbd_brightness_path[0] && sysfs_read_int(g_dynboost.kbd_brightness_path, &cur_kbd) == 0 && cur_kbd >= 0) {
            g_dynboost.saved_kbd_brightness = cur_kbd;
        }

        /* Keyboard backlight OFF */
        if (g_dynboost.kbd_brightness_path[0])
            sysfs_write_int(g_dynboost.kbd_brightness_path, 0);

        /* Display brightness: only dim if current brightness > 25% reduction target */
        g_dynboost.disp_dimmed = 0;
        if (g_dynboost.disp_brightness_path[0] && g_dynboost.disp_max_brightness > 0) {
            int target = g_dynboost.disp_max_brightness * RAPIDBOOST_DISP_PCT / 100;
            if (target < 1) target = 1;
            if (g_dynboost.saved_disp_brightness > target) {
                g_dynboost.disp_dimmed = 1;
                sysfs_write_int(g_dynboost.disp_brightness_path, target);
            }
        }

        /* CPU boost on battery: 50% of hardware max (1.15 GHz) */
        g_dynboost.boost_target_khz =
            g_dynboost.max_freq_khz * RAPIDBOOST_FREQ_PCT_BAT / 100;
    } else {
        /* CPU boost on AC: ramp up to 2.6 GHz Turbo */
        g_dynboost.boost_target_khz = RAPIDBOOST_FREQ_AC_KHZ;
    }

    set_all_cores_max_freq(g_dynboost.boost_target_khz);

    g_dynboost.rapidboost_active = 1;
    g_dynboost.rapidboost_start  = time(NULL);
    s_rb_focused_pid             = g_focused_pid;

    char body[256];
    if (on_ac) {
        snprintf(body, sizeof(body),
                 "RapidBoost is now active (AC mode).\n"
                 "CPU ramped up to %lu MHz",
                 g_dynboost.boost_target_khz / 1000);
    } else {
        if (g_dynboost.disp_dimmed) {
            snprintf(body, sizeof(body),
                     "RapidBoost is now active (Battery mode).\n"
                     "CPU boosted to %lu MHz\n"
                     "Keyboard backlight OFF\n"
                     "Display dimmed to 25%%",
                     g_dynboost.boost_target_khz / 1000);
        } else {
            snprintf(body, sizeof(body),
                     "RapidBoost is now active (Battery mode).\n"
                     "CPU boosted to %lu MHz\n"
                     "Keyboard backlight OFF\n"
                     "Display brightness preserved",
                     g_dynboost.boost_target_khz / 1000);
        }
    }
    dynboost_notify("Dynamic Boost System", body);

    if (!g_tui_mode && !g_gui_mode) {
        printf("[dynboost] RapidBoost ACTIVATED (%s)! Triggered by focused PID %d (CPU: %.1f%%)\n",
               on_ac ? "AC" : "battery", (int)s_rb_focused_pid, g_dynboost.focused_cpu_pct);
        printf("[dynboost]  -> CPU scaling_max_freq: %lu kHz -> %lu kHz\n",
               g_dynboost.saved_scaling_max_khz, g_dynboost.boost_target_khz);
        if (!on_ac) {
            printf("[dynboost]  -> Keyboard backlight disabled (was %d)\n",
                   g_dynboost.saved_kbd_brightness);
            if (g_dynboost.disp_dimmed) {
                printf("[dynboost]  -> Display backlight dimmed to 25%% (was %d)\n",
                       g_dynboost.saved_disp_brightness);
            } else {
                printf("[dynboost]  -> Display backlight preserved at %d (already <= 25%% target)\n",
                       g_dynboost.saved_disp_brightness);
            }
        }
        fflush(stdout);
    }
}

static void rapidboost_deactivate(void)
{
    if (!g_dynboost.rapidboost_active) return;

    int on_ac = g_dynboost.rapidboost_on_ac;
    unsigned long baseline_khz = on_ac ? CPU_MAX_FREQ_ON_AC_KHZ : CPU_MAX_FREQ_ON_BAT_KHZ;

    unsigned long current_max_khz = 0;
    sysfs_read_ulong("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
                     &current_max_khz);

    if (current_max_khz == g_dynboost.boost_target_khz)
        set_all_cores_max_freq(baseline_khz);

    int disp_was_dimmed = g_dynboost.disp_dimmed;
    if (!on_ac) {
        if (g_dynboost.kbd_brightness_path[0] && g_dynboost.saved_kbd_brightness >= 0)
            sysfs_write_int(g_dynboost.kbd_brightness_path,
                            g_dynboost.saved_kbd_brightness);

        if (disp_was_dimmed && g_dynboost.disp_brightness_path[0] && g_dynboost.saved_disp_brightness >= 0) {
            sysfs_write_int(g_dynboost.disp_brightness_path,
                            g_dynboost.saved_disp_brightness);
        }
    }

    g_dynboost.disp_dimmed = 0;
    g_dynboost.rapidboost_active = 0;

    char body[256];
    snprintf(body, sizeof(body),
             "RapidBoost is now inactive.\n"
             "CPU clocks normalized to %lu MHz%s",
             baseline_khz / 1000,
             on_ac ? "." : ", backlight restored.");
    dynboost_notify("Dynamic Boost System", body);

    if (!g_tui_mode && !g_gui_mode) {
        printf("[dynboost] RapidBoost DEACTIVATED (%s). System conditions normalized.\n",
               on_ac ? "AC" : "battery");
        printf("[dynboost]  -> CPU scaling_max_freq restored to %lu kHz\n", baseline_khz);
        if (!on_ac) {
            printf("[dynboost]  -> Keyboard backlight restored to %d\n",
                   g_dynboost.saved_kbd_brightness);
            if (disp_was_dimmed) {
                printf("[dynboost]  -> Display backlight restored to %d (memorized value)\n",
                       g_dynboost.saved_disp_brightness);
            }
        }
        fflush(stdout);
    }
}

/* ========================================================================== */
/*  BoostML  -  CSV data collection                                            */
/* ========================================================================== */
static void boostml_ensure_csv_header(void)
{
    FILE *f = fopen(BOOST_LOG_PATH, "r");
    if (f) { fclose(f); return; }             /* file already exists           */
    f = fopen(BOOST_LOG_PATH, "w");
    if (!f) return;
    fprintf(f, "timestamp,pid,comm,cpu_pct,freq_khz,focus_state\n");
    fclose(f);
}

static void boostml_log_sample(void)
{
    if (!g_dynboost.boostml_collecting) return;

    read_cpu_freqs();

    FILE *f = fopen(BOOST_LOG_PATH, "a");
    if (!f) return;

    time_t now  = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

    for (int i = 0; i < g_nprocs; i++) {
        TrackedProc *p = &g_procs[i];
        fprintf(f, "%s,%d,%s,%.1f,%lu,%s\n",
                ts, (int)p->pid, p->comm, p->cpu_pct,
                g_dynboost.cur_freq_khz,
                p->pid == g_focused_pid ? "focused" : "background");
    }
    fclose(f);
}

/* ========================================================================== */
/*  OpenGL 3.3 GPU compute infrastructure (Intel HD3000)                       */
/*  Sets up context + compiles neural-net layer shader for future BoostML      */
/*  inference.  No inference runs yet — model weights are not trained.         */
/* ========================================================================== */

/* Vertex shader: fullscreen triangle passthrough */
static const char *s_nn_vert_src =
    "#version 330 core\n"
    "in vec2 a_pos;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "    v_uv = a_pos * 0.5 + 0.5;\n"
    "}\n";

/* Fragment shader: single neural-net layer (matrix multiply + ReLU) */
static const char *s_nn_frag_src =
    "#version 330 core\n"
    "uniform sampler2D u_input;\n"
    "uniform sampler2D u_weights;\n"
    "uniform sampler2D u_bias;\n"
    "uniform int u_in_size;\n"
    "uniform int u_out_size;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    int out_idx = int(gl_FragCoord.x);\n"
    "    if (out_idx >= u_out_size) { frag_color = vec4(0.0); return; }\n"
    "    float sum = 0.0;\n"
    "    for (int i = 0; i < u_in_size; i++) {\n"
    "        float w = texelFetch(u_weights, ivec2(i, out_idx), 0).r;\n"
    "        float x = texelFetch(u_input,   ivec2(i, 0),       0).r;\n"
    "        sum += w * x;\n"
    "    }\n"
    "    sum += texelFetch(u_bias, ivec2(out_idx, 0), 0).r;\n"
    "    frag_color = vec4(max(0.0, sum), 0.0, 0.0, 1.0);\n"
    "}\n";

static int gpu_init(Display *dpy)
{
    int glx_major, glx_minor;
    if (!glXQueryVersion(dpy, &glx_major, &glx_minor))
        return -1;

    /* FBConfig with pbuffer support */
    int fb_attribs[] = {
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
        GLX_RED_SIZE,   8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE,  8,
        GLX_ALPHA_SIZE, 8,
        None
    };

    int nconfigs = 0;
    GLXFBConfig *configs = glXChooseFBConfig(dpy, DefaultScreen(dpy),
                                             fb_attribs, &nconfigs);
    if (!configs || nconfigs == 0) return -1;

    /* glXCreateContextAttribsARB for GL 3.3 core profile */
    typedef GLXContext (*PFNGLXCCAARBPROC)(Display *, GLXFBConfig,
                                           GLXContext, Bool, const int *);
    PFNGLXCCAARBPROC createCtx = (PFNGLXCCAARBPROC)
        glXGetProcAddress((const GLubyte *)"glXCreateContextAttribsARB");
    if (!createCtx) { XFree(configs); return -1; }

    int ctx_attribs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
        GLX_CONTEXT_MINOR_VERSION_ARB, 3,
        GLX_CONTEXT_PROFILE_MASK_ARB,  GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
        None
    };

    s_gl_dpy = dpy;
    s_gl_ctx = createCtx(dpy, configs[0], NULL, True, ctx_attribs);
    if (!s_gl_ctx) { XFree(configs); return -1; }

    /* Pbuffer (offscreen render target) */
    int pbuf_attribs[] = {
        GLX_PBUFFER_WIDTH,  256,
        GLX_PBUFFER_HEIGHT, 256,
        None
    };
    s_gl_pbuf = glXCreatePbuffer(dpy, configs[0], pbuf_attribs);
    XFree(configs);
    if (!s_gl_pbuf) goto fail_ctx;

    if (!glXMakeCurrent(dpy, s_gl_pbuf, s_gl_ctx)) goto fail_pbuf;

    /* ---- compile shaders ---- */
    GLint ok;

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &s_nn_vert_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); goto fail_gl; }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &s_nn_frag_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); glDeleteShader(fs); goto fail_gl; }

    s_nn_program = glCreateProgram();
    glAttachShader(s_nn_program, vs);
    glAttachShader(s_nn_program, fs);
    glLinkProgram(s_nn_program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGetProgramiv(s_nn_program, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(s_nn_program); s_nn_program = 0; goto fail_gl; }

    glGenVertexArrays(1, &s_nn_vao);
    glGenFramebuffers(1, &s_nn_fbo);

    glXMakeCurrent(dpy, None, NULL);           /* release context              */
    return 0;

fail_gl:
    glXMakeCurrent(dpy, None, NULL);
fail_pbuf:
    glXDestroyPbuffer(dpy, s_gl_pbuf);  s_gl_pbuf = 0;
fail_ctx:
    glXDestroyContext(dpy, s_gl_ctx);   s_gl_ctx  = 0;
    return -1;
}

static void gpu_shutdown(void)
{
    if (!s_gl_ctx || !s_gl_dpy) return;

    glXMakeCurrent(s_gl_dpy, s_gl_pbuf, s_gl_ctx);
    if (s_nn_fbo)     { glDeleteFramebuffers(1, &s_nn_fbo);  s_nn_fbo     = 0; }
    if (s_nn_vao)     { glDeleteVertexArrays(1, &s_nn_vao);  s_nn_vao     = 0; }
    if (s_nn_program) { glDeleteProgram(s_nn_program);        s_nn_program = 0; }
    glXMakeCurrent(s_gl_dpy, None, NULL);

    glXDestroyPbuffer(s_gl_dpy, s_gl_pbuf);  s_gl_pbuf = 0;
    glXDestroyContext(s_gl_dpy, s_gl_ctx);   s_gl_ctx  = 0;
}

/* ========================================================================== */
/*  Public API                                                                 */
/* ========================================================================== */

void dynboost_init(Display *dpy)
{
    memset(&g_dynboost, 0, sizeof(g_dynboost));

    /* Discover hardware paths */
    discover_kbd_backlight();
    discover_display_backlight();
    read_cpu_freqs();

    /* Memorize initial brightness levels */
    g_dynboost.saved_disp_brightness = -1;
    g_dynboost.saved_kbd_brightness  = -1;
    if (g_dynboost.disp_brightness_path[0])
        sysfs_read_int(g_dynboost.disp_brightness_path, &g_dynboost.saved_disp_brightness);
    if (g_dynboost.kbd_brightness_path[0])
        sysfs_read_int(g_dynboost.kbd_brightness_path, &g_dynboost.saved_kbd_brightness);

    g_dynboost.boost_target_khz =
        g_dynboost.max_freq_khz * RAPIDBOOST_FREQ_PCT_BAT / 100;

    if (!g_tui_mode && !g_gui_mode) {
        printf("[dynboost] Dynamic Boost System Initialized\n");
        printf("[dynboost]  -> Hardware Base Freq: %lu kHz\n", g_dynboost.base_freq_khz);
        printf("[dynboost]  -> Hardware Max Freq:  %lu kHz\n", g_dynboost.max_freq_khz);
        printf("[dynboost]  -> Computed Boost Target (50%%): %lu kHz\n", g_dynboost.boost_target_khz);
        printf("[dynboost]  -> Keyboard backlight sysfs: %s (current: %d)\n",
               g_dynboost.kbd_brightness_path[0]
                   ? g_dynboost.kbd_brightness_path : "(not found)",
               g_dynboost.saved_kbd_brightness);
        printf("[dynboost]  -> Display backlight sysfs:  %s (current: %d, max=%d)\n",
               g_dynboost.disp_brightness_path[0]
                   ? g_dynboost.disp_brightness_path : "(not found)",
               g_dynboost.saved_disp_brightness,
               g_dynboost.disp_max_brightness);
        fflush(stdout);
    }

    /* BoostML: experimental data collection + GPU infra */
    if (g_experimental) {
        g_dynboost.boostml_collecting = 1;
        boostml_ensure_csv_header();

        if (gpu_init(dpy) == 0) {
            g_dynboost.gpu_available = 1;
            if (!g_tui_mode && !g_gui_mode) {
                printf("[dynboost] BoostML GPU Subsystem: Context READY\n");
                printf("[dynboost]  -> OpenGL 3.3 Core Profile initialized on Intel HD3000\n");
                printf("[dynboost]  -> Vertex & Fragment Shaders compiled for inference\n");
                printf("[dynboost]  -> VAO and FBO configured for off-screen tensor compute\n");
                fflush(stdout);
            }
        } else {
            g_dynboost.gpu_available = 0;
            if (!g_tui_mode && !g_gui_mode) {
                printf("[dynboost] BoostML GPU Subsystem: FAILED / UNAVAILABLE\n");
                printf("[dynboost]  -> BoostML will fall back to data collection only mode\n");
                fflush(stdout);
            }
        }

        dynboost_notify("BoostML Started",
                        "Data collection active.\n"
                        "ML inference not yet available.");
    }
}

void dynboost_shutdown(void)
{
    if (g_dynboost.rapidboost_active)
        rapidboost_deactivate();
    gpu_shutdown();
}

void dynboost_tick(pid_t focused_pid)
{
    /* 1.  Update per-process CPU usage */
    update_cpu_usage();
    read_cpu_freqs();

    /* 2.  Focused process CPU % */
    int fi = proc_find(focused_pid);
    g_dynboost.focused_cpu_pct = (fi >= 0) ? g_procs[fi].cpu_pct : 0.0;

    /* 3.  BoostML data collection (experimental only) */
    if (g_experimental)
        boostml_log_sample();

    /* 4.  RapidBoost logic (skip in monitor mode) */
    if (g_monitor_only) return;

    int on_ac = power_on_ac();
    double thresh = on_ac ? RAPIDBOOST_CPU_THRESH_AC : RAPIDBOOST_CPU_THRESH_BAT;

    if (!g_dynboost.rapidboost_active) {
        /* Continuously memorize user-adjusted brightness levels */
        int cur_disp = -1, cur_kbd = -1;
        if (g_dynboost.disp_brightness_path[0] && sysfs_read_int(g_dynboost.disp_brightness_path, &cur_disp) == 0 && cur_disp >= 0) {
            g_dynboost.saved_disp_brightness = cur_disp;
        }
        if (g_dynboost.kbd_brightness_path[0] && sysfs_read_int(g_dynboost.kbd_brightness_path, &cur_kbd) == 0 && cur_kbd >= 0) {
            g_dynboost.saved_kbd_brightness = cur_kbd;
        }

        /* Activate if: mode is not None AND focused app CPU saturated */
        if (g_mode != MODE_NONE
            && g_dynboost.focused_cpu_pct >= thresh) {
            rapidboost_activate(on_ac);
        }
    } else {
        /* Deactivate if: mode turned off, CPU dropped, focus switched, or AC state flipped */
        double drop_thresh = on_ac ? (RAPIDBOOST_CPU_THRESH_AC - 10.0) : (RAPIDBOOST_CPU_THRESH_BAT - 10.0);
        if (g_mode == MODE_NONE
            || g_dynboost.focused_cpu_pct < drop_thresh
            || focused_pid != s_rb_focused_pid
            || on_ac != g_dynboost.rapidboost_on_ac) {
            rapidboost_deactivate();
        }
    }
}
