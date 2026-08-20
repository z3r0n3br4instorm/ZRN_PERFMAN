/*
 * throttle.c  -  CPU throttling via SIGSTOP / SIGCONT duty cycling
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"
#include <dirent.h>
#include <ctype.h>
#include <pthread.h>

static pthread_t       s_thread;
static volatile sig_atomic_t s_thread_running = 1;
static pid_t s_self_pid = 0;

static void sleep_ms(int ms)
{
    if (ms <= 0) return;
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

static long long get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void signal_tree(TrackedProc *p, int sig)
{
    if (s_self_pid == 0) s_self_pid = getpid();
    if (p->pid != s_self_pid) kill(p->pid, sig);
    for (int i = 0; i < p->ndescendants; i++) {
        if (p->descendants[i] != s_self_pid) kill(p->descendants[i], sig);
    }
}

static void stop_tree(TrackedProc *p) { 
    if (p->cpulimit_pid > 0) kill(p->cpulimit_pid, SIGSTOP);
    signal_tree(p, SIGSTOP); 
}
static void cont_tree(TrackedProc *p) { 
    signal_tree(p, SIGCONT); 
    if (p->cpulimit_pid > 0) kill(p->cpulimit_pid, SIGCONT);
}

int throttle_get_tick(TrackedProc *p)
{
    if (p->prewarm) return NN_PREWARM_TICK_MS;
    
    int base_tick;
    if (g_mode == MODE_MODERATE) base_tick = TICK_MODERATE_MS;
    else if (g_mode == MODE_HIGH) base_tick = TICK_HIGH_MS;
    else base_tick = TICK_NOMINAL_MS;
    
    time_t now = time(NULL);
    if (p->defocus_time > 0 && now > p->defocus_time) {
        long elapsed = now - p->defocus_time;
        long max_elapsed = 3600; // 1 hour
        if (elapsed > max_elapsed) elapsed = max_elapsed;
        
        int max_tick = 10000; // 10 seconds
        if (max_tick > base_tick) {
            long dynamic_tick = base_tick + (elapsed * (max_tick - base_tick)) / max_elapsed;
            return (int)dynamic_tick;
        }
    }
    
    return base_tick;
}

static void *throttle_worker(void *arg)
{
    (void)arg;
    while (s_thread_running) {
        long long now = get_time_ms();
        int pulsed_any = 0;

        for (int i = 0; i < g_nprocs; i++) {
            TrackedProc *p = &g_procs[i];

            if (p->should_throttle && !p->throttled) {
                stop_tree(p);
                p->throttled = 1;
                p->next_pulse = now + throttle_get_tick(p);
                if (!g_tui_mode && !g_gui_mode) {
                    printf("[tickle] PID %d (%s) SUSPENDED. Next pulse in %d ms.\n",
                           (int)p->pid, p->comm, throttle_get_tick(p));
                    fflush(stdout);
                }
            } else if (!p->should_throttle && p->throttled) {
                cont_tree(p);
                p->throttled = 0;
                if (!g_tui_mode && !g_gui_mode) {
                    printf("[tickle] PID %d (%s) RESUMED (exempt or focused).\n",
                           (int)p->pid, p->comm);
                    fflush(stdout);
                }
            }

            if (p->throttled && now >= p->next_pulse) {
                cont_tree(p);
                p->pulsing = 1;
                pulsed_any = 1;
            }
        }

        if (pulsed_any) {
            sleep_ms(THROTTLE_QUANTUM_MS);
            now = get_time_ms();
            for (int i = 0; i < g_nprocs; i++) {
                TrackedProc *p = &g_procs[i];
                if (p->throttled && p->pulsing) {
                    stop_tree(p);
                    p->pulsing = 0;
                    p->next_pulse = now + throttle_get_tick(p) - THROTTLE_QUANTUM_MS;
                }
            }
        } else {
            sleep_ms(50);
        }
    }
    return NULL;
}

void throttle_init(void)
{
    pthread_create(&s_thread, NULL, throttle_worker, NULL);
}

void throttle_shutdown(void)
{
    s_thread_running = 0;
    pthread_join(s_thread, NULL);
}

void throttle_enqueue_job(pid_t pid, int action) { (void)pid; (void)action; }

int is_comm_exempt(const char *comm, const char *focused_comm)
{
    if (exempt_check(comm)) return 1;
    if (g_pairing_mode && !g_experimental && switch_is_pair_exempt(comm, focused_comm)) return 1;
    return 0;
}

void throttle_apply(pid_t focused_pid)
{
    time_t now = time(NULL);
    int grace_sec;
    
    switch (g_mode) {
        case MODE_MODERATE: grace_sec = GRACE_MODERATE_SEC; break;
        case MODE_HIGH:     grace_sec = GRACE_HIGH_SEC; break;
        default:            grace_sec = GRACE_NOMINAL_SEC; break;
    }

    const char *focused_comm = "?";
    int f_idx = proc_find(focused_pid);
    if (f_idx >= 0) focused_comm = g_procs[f_idx].comm;

    for (int i = 0; i < g_nprocs; i++) {
        TrackedProc *p = &g_procs[i];

        if (g_mode == MODE_NONE || p->pid == focused_pid || 
            is_comm_exempt(p->comm, focused_comm) || p->audio_playing) {
            p->should_throttle = 0;
            p->defocus_time = 0;
            p->prewarm = 0;
            continue;
        }

        if (g_experimental && g_predicted_comm[0] && strcmp(p->comm, g_predicted_comm) == 0) {
            p->should_throttle = 1;
            p->prewarm = 1;
            continue;
        } else {
            p->prewarm = 0;
        }

        if (p->defocus_time == 0) p->defocus_time = now;
        p->should_throttle = ((now - p->defocus_time) >= grace_sec);
    }
}

void throttle_unthrottle_all(void)
{
    for (int i = 0; i < g_nprocs; i++) {
        if (g_procs[i].throttled) {
            cont_tree(&g_procs[i]);
            g_procs[i].throttled = 0;
            g_procs[i].should_throttle = 0;
        }
    }
}
