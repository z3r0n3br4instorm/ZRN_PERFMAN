/*
 * gpu_monitor.c  -  DRM fdinfo GPU usage monitoring and throttling
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"
#include <dirent.h>
#include <ctype.h>

GpuProc g_gpu_procs[MAX_GPU_PROCS];
int     g_ngpu_procs = 0;

static long long get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void parse_fdinfo_drm(const char *path, long long *engine_time)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "drm-engine-", 11) == 0) {
            char *p = strchr(line, ':');
            if (p) {
                long long val = strtoll(p + 1, NULL, 10);
                if (strstr(line, "ns"))
                    *engine_time += val;
            }
        }
    }
    fclose(f);
}

void gpu_scan(void)
{
    DIR *proc = opendir("/proc");
    if (!proc) return;

    GpuProc new_gpu[MAX_GPU_PROCS];
    int nnew = 0;

    struct dirent *de;
    while ((de = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        if (pid <= 0) continue;

        char fd_path[64];
        snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd", (int)pid);
        DIR *fddir = opendir(fd_path);
        if (!fddir) continue;

        long long active_time_ns = 0;
        struct dirent *fde;
        while ((fde = readdir(fddir)) != NULL) {
            if (!isdigit((unsigned char)fde->d_name[0])) continue;
            
            char link_path[128];
            snprintf(link_path, sizeof(link_path), "/proc/%d/fd/%s", (int)pid, fde->d_name);
            char target[256];
            ssize_t len = readlink(link_path, target, sizeof(target)-1);
            if (len > 0) {
                target[len] = '\0';
                if (strstr(target, "/dev/dri/renderD")) {
                    char info_path[128];
                    snprintf(info_path, sizeof(info_path), "/proc/%d/fdinfo/%s", (int)pid, fde->d_name);
                    parse_fdinfo_drm(info_path, &active_time_ns);
                }
            }
        }
        closedir(fddir);

        if (active_time_ns > 0 && nnew < MAX_GPU_PROCS) {
            new_gpu[nnew].pid = pid;
            new_gpu[nnew].throttled = 0;
            new_gpu[nnew].next_pulse = 0;
            new_gpu[nnew].pulsing = 0;
            read_comm(pid, new_gpu[nnew].comm, sizeof(new_gpu[nnew].comm));
            new_gpu[nnew].usage_pct = (active_time_ns > 1000000) ? 100 : 0; 
            nnew++;
        }
    }
    closedir(proc);

    for (int i = 0; i < nnew; i++) {
        for (int j = 0; j < g_ngpu_procs; j++) {
            if (g_gpu_procs[j].pid == new_gpu[i].pid) {
                new_gpu[i].throttled  = g_gpu_procs[j].throttled;
                new_gpu[i].pulsing    = g_gpu_procs[j].pulsing;
                new_gpu[i].next_pulse = g_gpu_procs[j].next_pulse;
                break;
            }
        }
    }

    g_ngpu_procs = nnew;
    for (int i = 0; i < nnew; i++)
        g_gpu_procs[i] = new_gpu[i];
}

void gpu_throttle_apply(pid_t focused_pid)
{
    if (g_mode == MODE_NONE) return;

    long long now = get_time_ms();

    const char *focused_comm = "?";
    int f_idx = proc_find(focused_pid);
    if (f_idx >= 0) focused_comm = g_procs[f_idx].comm;

    for (int i = 0; i < g_ngpu_procs; i++) {
        GpuProc *gp = &g_gpu_procs[i];

        if (gp->pid == focused_pid || is_comm_exempt(gp->comm, focused_comm) || gp->pid == getpid()) {
            if (gp->throttled) {
                kill(gp->pid, SIGCONT);
                gp->throttled = 0;
                gp->pulsing = 0;
            }
            continue;
        }

        int ti = proc_find(gp->pid);
        if (ti >= 0 && g_procs[ti].throttled) continue;

        if (!gp->throttled) {
            kill(gp->pid, SIGSTOP);
            gp->throttled = 1;
            gp->next_pulse = now + TICK_MODERATE_MS;
        } else if (now >= gp->next_pulse && !gp->pulsing) {
            kill(gp->pid, SIGCONT);
            gp->pulsing = 1;
            gp->next_pulse = now + THROTTLE_QUANTUM_MS;
        } else if (gp->pulsing && now >= gp->next_pulse) {
            kill(gp->pid, SIGSTOP);
            gp->pulsing = 0;
            gp->next_pulse = now + TICK_MODERATE_MS - THROTTLE_QUANTUM_MS;
        }
    }
}

void gpu_unthrottle_all(void)
{
    for (int i = 0; i < g_ngpu_procs; i++) {
        if (g_gpu_procs[i].throttled) {
            kill(g_gpu_procs[i].pid, SIGCONT);
            g_gpu_procs[i].throttled = 0;
            g_gpu_procs[i].pulsing = 0;
        }
    }
}
