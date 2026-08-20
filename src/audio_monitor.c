/*
 * audio_monitor.c  -  Detect PIDs playing audio via pactl (PipeWire/PulseAudio)
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"

#define MAX_AUDIO_PIDS 64

static pid_t s_audio_pids[MAX_AUDIO_PIDS];
static int   s_naudio = 0;

static pid_t read_ppid(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    pid_t ppid = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PPid:", 5) == 0) {
            ppid = (pid_t)atoi(line + 5);
            break;
        }
    }
    fclose(f);
    return ppid;
}

static int is_ancestor_of(pid_t ancestor, pid_t child)
{
    if (ancestor == child) return 1;
    pid_t cur = child;
    while (cur > 1) {
        pid_t p = read_ppid(cur);
        if (p <= 0) break;
        if (p == ancestor) return 1;
        cur = p;
    }
    return 0;
}

void audio_scan(void)
{
    pid_t new_pids[MAX_AUDIO_PIDS];
    int   nnew = 0;

    FILE *fp = popen("pactl list sink-inputs 2>/dev/null", "r");
    if (!fp) return;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *key = strstr(line, "application.process.id");
        if (!key) continue;

        char *quote1 = strchr(key, '"');
        if (!quote1) continue;
        quote1++;

        char *quote2 = strchr(quote1, '"');
        if (!quote2) continue;
        *quote2 = '\0';

        pid_t pid = (pid_t)atoi(quote1);
        if (pid <= 0) continue;

        int dup = 0;
        for (int i = 0; i < nnew; i++) {
            if (new_pids[i] == pid) { dup = 1; break; }
        }
        if (!dup && nnew < MAX_AUDIO_PIDS)
            new_pids[nnew++] = pid;
    }
    pclose(fp);

    s_naudio = nnew;
    for (int i = 0; i < nnew; i++)
        s_audio_pids[i] = new_pids[i];

    for (int i = 0; i < g_nprocs; i++) {
        g_procs[i].audio_playing = 0;
        for (int a = 0; a < s_naudio; a++) {
            if (is_ancestor_of(g_procs[i].pid, s_audio_pids[a])) {
                g_procs[i].audio_playing = 1;
                break;
            }
        }
    }
}

int audio_is_playing(pid_t pid)
{
    for (int i = 0; i < s_naudio; i++)
        if (is_ancestor_of(pid, s_audio_pids[i])) return 1;
    return 0;
}

void audio_init(void) { }
void audio_cleanup(void) { }
