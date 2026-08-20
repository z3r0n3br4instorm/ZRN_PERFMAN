/*
 * zrn_perf.h  -  ZrnPerformanceMgmnt shared types and declarations
 *
 * Profile modes:
 *   None     : no throttling at all
 *   Nominal  : throttle unfocused windows after 5 min  (200 ms tick)
 *   Moderate : throttle unfocused windows after 1 min  (500 ms tick)
 *   High     : throttle ALL unfocused X apps immediately (1 s tick)
 */
#pragma once

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* tuning constants                                                            */
/* -------------------------------------------------------------------------- */
#define PROFILE_PATH          "/home/zerone/.zrnperformanceprofile"
#define EXEMPT_PATH           "/home/zerone/.zrnperformanceexempt"
#define LOCK_FILE_PATH        "/home/zerone/.zrn_perf.lock"
#define SWITCH_DATA_PATH      "/home/zerone/.zrn_switch_data.csv"
#define MODEL_WEIGHTS_PATH    "/home/zerone/.zrn_model_weights.bin"
#define PROFILE_RELOAD_SEC    5
#define MAX_TRACKED           512

/* Tick intervals (ms) */
#define TICK_NOMINAL_MS       200
#define TICK_MODERATE_MS      500
#define TICK_HIGH_MS          1000
#define TICK_AUDIO_MS         200    /* gentler tick for audio-playing procs   */

/* Grace periods (seconds) */
#define GRACE_NOMINAL_SEC     (5 * 60)
#define GRACE_MODERATE_SEC    (1 * 60)
#define GRACE_HIGH_SEC        0

/* Main-loop quantum (ms) */
#define THROTTLE_QUANTUM_MS   50

/* Frequent-switch pair detection */
#define SWITCH_HISTORY_MAX    256
#define SWITCH_PAIR_WINDOW    60
#define SWITCH_PAIR_THRESHOLD 4
#define SWITCH_PAIR_MAX       32
#define PAIR_COOLDOWN_SEC     120

/* GPU monitoring */
#define GPU_SCAN_INTERVAL_SEC 5
#define MAX_GPU_PROCS         64
#define GPU_USAGE_THRESHOLD   5

/* Audio monitoring */
#define AUDIO_SCAN_INTERVAL_SEC 2

/* Neural net (data collection + prediction + RL) */
#define NN_INPUT_DIM          16
#define NN_HIDDEN1            32
#define NN_HIDDEN2            16
#define NN_OUTPUT_DIM         64
#define NN_PREWARM_TICK_MS    100

typedef struct {
    char  comm[64];
    float prob;   /* 0.0 - 1.0 */
} NNPrediction;

typedef struct {
    int          count;
    NNPrediction top[5];
    int          last_reward;       /* +1 (correct) or -1 (error/penalty) */
    char         last_predicted[64];
    char         last_actual[64];
    float        last_conf;
    float        last_loss;
    int          total_correct;
    int          total_predictions;
    int          online_updates;
} NNState;

extern NNState g_nn_state;
extern char    g_predicted_comm[64];

/* Exempt list */
#define MAX_EXEMPT            64

/* -------------------------------------------------------------------------- */
/* profile mode                                                                */
/* -------------------------------------------------------------------------- */
typedef enum {
    MODE_NONE      = -1,
    MODE_NOMINAL   = 0,
    MODE_MODERATE  = 1,
    MODE_HIGH      = 2,
} PerfMode;

/* -------------------------------------------------------------------------- */
/* per-PID record                                                              */
/* -------------------------------------------------------------------------- */
typedef struct {
    pid_t    pid;
    time_t   first_seen;
    time_t   defocus_time;     /* 0 = currently focused                       */
    int      throttled;
    int      gpu_heavy;
    int      prewarm;
    int      audio_playing;    /* 1 if PulseAudio/PipeWire reports audio      */
    int      should_throttle;  /* 1 if main loop wants this stopped           */
    int      pulsing;          /* 1 if thread is currently sending SIGCONT    */
    long long next_pulse;      /* next time (ms) to pulse                     */
    double   cpu_pct;
    
    int      cpu_limit_pct;
    pid_t    cpulimit_pid;

    pid_t    descendants[64];
    int      ndescendants;

    char     comm[64];
} TrackedProc;

/* -------------------------------------------------------------------------- */
/* switch event (for logging + pair detection)                                 */
/* -------------------------------------------------------------------------- */
typedef struct {
    time_t   timestamp;
    pid_t    from_pid;
    pid_t    to_pid;
    char     from_comm[64];
    char     to_comm[64];
    long     duration_on_prev_ms;
} SwitchEvent;

/* -------------------------------------------------------------------------- */
/* frequent-switch pair                                                        */
/* -------------------------------------------------------------------------- */
typedef struct {
    char     comm_a[64];
    char     comm_b[64];
    time_t   last_switch;
    int      count;
} SwitchPair;

/* -------------------------------------------------------------------------- */
/* GPU process record                                                          */
/* -------------------------------------------------------------------------- */
typedef struct {
    pid_t    pid;
    char     comm[64];
    int      usage_pct;
    int      throttled;
    int      pulsing;
    long long next_pulse;
} GpuProc;

/* -------------------------------------------------------------------------- */
/* exempt entry (user-chosen from TUI)                                         */
/* -------------------------------------------------------------------------- */
#define LIMITS_PATH "/home/zerone/.zrnperformancelimits"

typedef struct {
    char     comm[64];
    int      limit_pct;
} LimitEntry;

typedef struct {
    char     comm[64];
} ExemptEntry;

/* -------------------------------------------------------------------------- */
/* power_source.c  -  direct AC/battery detection (no TLP)                    */
/* -------------------------------------------------------------------------- */
int power_on_ac(void);

/* -------------------------------------------------------------------------- */
/* cpu_power.c  -  CPU governor/frequency management by AC/battery state      */
/* -------------------------------------------------------------------------- */
#define CPU_MIN_FREQ_ON_AC_KHZ   1500000
#define CPU_MAX_FREQ_ON_AC_KHZ   2300000
#define CPU_GOVERNOR_ON_AC       "performance"
#define CPU_MIN_FREQ_ON_BAT_KHZ  400000
#define CPU_MAX_FREQ_ON_BAT_KHZ  900000
#define CPU_GOVERNOR_ON_BAT      "powersave"
#define CPU_POWER_SCAN_INTERVAL_SEC 2

void cpu_power_init(void);
void cpu_power_tick(void);

/* HDD spin-down (battery only) */
#define HDD_IDLE_TIMEOUT_SEC  30
#define HDD_SCAN_INTERVAL_SEC 2

typedef struct {
    char     device_path[64];   /* e.g. /dev/sda                          */
    char     stat_name[32];     /* e.g. "sda", as it appears in diskstats */
    int      on_ac;
    int      standby;           /* 1 = we've put the drive in standby     */
    unsigned long long last_io_ticks;
    time_t   last_activity;
} HddMonState;

extern HddMonState g_hddmon;
void hddmon_init(void);
void hddmon_tick(void);

#define RAPIDBOOST_CPU_THRESH_BAT  70.0
#define RAPIDBOOST_CPU_THRESH_AC   95.0
#define RAPIDBOOST_FREQ_PCT_BAT    50
#define RAPIDBOOST_FREQ_AC_KHZ     2600000
#define RAPIDBOOST_DISP_PCT        25
#define BOOST_LOG_PATH "/tmp/boostml.csv"

typedef struct {
    /* RapidBoost */
    int      rapidboost_active;
    int      rapidboost_on_ac;
    unsigned long max_freq_khz;
    unsigned long base_freq_khz;
    unsigned long cur_freq_khz;
    unsigned long saved_scaling_max_khz;
    char     kbd_brightness_path[256];
    char     disp_brightness_path[256];
    char     disp_max_brightness_path[256];
    int      saved_kbd_brightness;
    int      disp_max_brightness;
    int      saved_disp_brightness;
    int      disp_dimmed;
    unsigned long boost_target_khz;
    time_t   rapidboost_start;
    double   focused_cpu_pct;

    /* BoostML */
    int      boostml_collecting;
    int      ml_prediction;
    int      gpu_available;
} DynBoostState;

/* -------------------------------------------------------------------------- */
/* global state (defined in main.c)                                            */
/* -------------------------------------------------------------------------- */
extern volatile sig_atomic_t g_running;
extern PerfMode              g_mode;
extern TrackedProc           g_procs[MAX_TRACKED];
extern int                   g_nprocs;
extern pid_t                 g_focused_pid;
extern int                   g_experimental;
extern int                   g_pairing_mode;
extern int                   g_tui_mode;
extern int                   g_gui_mode;
extern int                   g_monitor_only;

/* Switch history ring buffer */
extern SwitchEvent           g_switch_history[SWITCH_HISTORY_MAX];
extern int                   g_switch_head;
extern int                   g_switch_count;

/* Exempt pairs (frequent switchers) */
extern SwitchPair            g_pairs[SWITCH_PAIR_MAX];
extern int                   g_npairs;

/* GPU process table */
extern GpuProc               g_gpu_procs[MAX_GPU_PROCS];
extern int                   g_ngpu_procs;

/* User-exempted app types */
extern ExemptEntry           g_exempt[MAX_EXEMPT];
extern int                   g_nexempt;

/* CPU limits */
extern LimitEntry            g_limits[MAX_EXEMPT];
extern int                   g_nlimits;

/* -------------------------------------------------------------------------- */
/* limits.c                                                                    */
/* -------------------------------------------------------------------------- */
void limits_load(const char *path);
void limits_save(const char *path);
void limit_add(const char *comm, int limit_pct);
void limit_remove(const char *comm);
int  limit_check(const char *comm);
void limits_apply_all(void);

/* -------------------------------------------------------------------------- */
/* profile.c                                                                   */
/* -------------------------------------------------------------------------- */
PerfMode    load_profile(const char *path);
const char *mode_name(PerfMode m);
PerfMode profile_detect_system(void);
void profile_init(void);
void profile_reload(void);

/* -------------------------------------------------------------------------- */
/* proctrack.c                                                                 */
/* -------------------------------------------------------------------------- */
int  proc_find(pid_t pid);
int  proc_add(pid_t pid);
void proc_remove_dead(void);
void proctrack_update_descendants(void);
int  read_comm(pid_t pid, char *out, size_t n);

/* -------------------------------------------------------------------------- */
/* xwatch.c                                                                    */
/* -------------------------------------------------------------------------- */
pid_t  xwin_pid(Display *dpy, Window win, Atom net_wm_pid);
Window xwin_active(Display *dpy, Window root, Atom net_active_window);
void   xwin_scan_existing(Display *dpy, Window root,
                          Atom net_wm_pid, Atom net_client_list);

/* -------------------------------------------------------------------------- */
/* throttle.c                                                                  */
/* -------------------------------------------------------------------------- */
void throttle_init(void);
void throttle_shutdown(void);
void throttle_apply(pid_t focused_pid);
void throttle_unthrottle_all(void);
int  throttle_get_tick(TrackedProc *p);
void throttle_enqueue_job(pid_t pid, int action);
int  is_comm_exempt(const char *comm, const char *focused_comm);

/* -------------------------------------------------------------------------- */
/* switchlog.c                                                                 */
/* -------------------------------------------------------------------------- */
void switch_record(pid_t from_pid, const char *from_comm, pid_t to_pid, const char *to_comm, long duration_ms);
void switch_save_csv(const SwitchEvent *ev);
int  switch_rate_last_min(void);
void switch_update_pairs(void);
int  switch_is_pair_exempt(const char *comm, const char *focused_comm);
void switch_save_csv(const SwitchEvent *ev);

/* -------------------------------------------------------------------------- */
/* gpu_monitor.c                                                               */
/* -------------------------------------------------------------------------- */
void gpu_scan(void);
void gpu_throttle_apply(pid_t focused_pid);
void gpu_unthrottle_all(void);

/* -------------------------------------------------------------------------- */
/* audio_monitor.c  -  detect PIDs playing audio via PulseAudio/PipeWire       */
/* -------------------------------------------------------------------------- */
void audio_init(void);
void audio_scan(void);
int  audio_is_playing(pid_t pid);
void audio_cleanup(void);

/* -------------------------------------------------------------------------- */
/* neural_predict.c                                                            */
/* -------------------------------------------------------------------------- */
void nn_record_features(const SwitchEvent *ev, int switch_rate_1min);
int  nn_predict_next(const char *current_comm, char *predicted_out, size_t n);
void nn_feedback(const char *actual_comm);
int  nn_load_weights(const char *path);
int  nn_save_weights(const char *path);
int  nn_gpu_init(Display *dpy);
void nn_gpu_shutdown(void);

/* -------------------------------------------------------------------------- */
/* exempt.c                                                                    */
/* -------------------------------------------------------------------------- */
void exempt_load(const char *path);
void exempt_save(const char *path);
void exempt_add(const char *comm);
void exempt_remove(const char *comm);
int  exempt_check(const char *comm);

/* -------------------------------------------------------------------------- */
/* tui.c                                                                       */
/* -------------------------------------------------------------------------- */
void tui_init(void);
void tui_shutdown(void);
void tui_draw(Display *dpy, Window root, Atom net_wm_pid, Atom net_active_window);
int  tui_handle_input(void);

/* -------------------------------------------------------------------------- */
/* gui.c  -  GTK3 GUI (Openbox-compatible)                                     */
/* -------------------------------------------------------------------------- */
void gui_init(int *argc, char ***argv);
void gui_run(void);
void gui_shutdown(void);

#define DYNBOOST_SCAN_SEC 1
extern DynBoostState g_dynboost;
void dynboost_init(Display *dpy);
void dynboost_tick(pid_t focused_pid);
void dynboost_shutdown(void);
void dynboost_notify(const char *summary, const char *body);
