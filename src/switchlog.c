/*
 * switchlog.c  -  Switch event recording + frequent-pair detection
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"
#include <ctype.h>

SwitchEvent  g_switch_history[SWITCH_HISTORY_MAX];
int          g_switch_head  = 0;
int          g_switch_count = 0;

SwitchPair   g_pairs[SWITCH_PAIR_MAX];
int          g_npairs = 0;

void switch_record(pid_t from_pid, const char *from_comm,
                   pid_t to_pid,   const char *to_comm,
                   long duration_ms)
{
    SwitchEvent ev;
    ev.timestamp         = time(NULL);
    ev.from_pid          = from_pid;
    ev.to_pid            = to_pid;
    ev.duration_on_prev_ms = duration_ms;
    strncpy(ev.from_comm, from_comm ? from_comm : "?", sizeof(ev.from_comm) - 1);
    ev.from_comm[sizeof(ev.from_comm) - 1] = '\0';
    strncpy(ev.to_comm,   to_comm   ? to_comm   : "?", sizeof(ev.to_comm) - 1);
    ev.to_comm[sizeof(ev.to_comm) - 1] = '\0';

    g_switch_history[g_switch_head] = ev;
    g_switch_head = (g_switch_head + 1) % SWITCH_HISTORY_MAX;
    if (g_switch_count < SWITCH_HISTORY_MAX)
        g_switch_count++;

    switch_save_csv(&ev);
}

void switch_save_csv(const SwitchEvent *ev)
{
    int need_header = (access(SWITCH_DATA_PATH, F_OK) != 0);

    FILE *f = fopen(SWITCH_DATA_PATH, "a");
    if (!f) return;

    if (need_header)
        fprintf(f, "timestamp,from_pid,from_comm,to_pid,to_comm,duration_ms\n");

    fprintf(f, "%ld,%d,%s,%d,%s,%ld\n",
            (long)ev->timestamp,
            (int)ev->from_pid,  ev->from_comm,
            (int)ev->to_pid,    ev->to_comm,
            ev->duration_on_prev_ms);

    fclose(f);
}

int switch_rate_last_min(void)
{
    time_t now   = time(NULL);
    time_t cutoff = now - 60;
    int count = 0;

    int total = g_switch_count;
    for (int i = 0; i < total; i++) {
        int idx = (g_switch_head - 1 - i + SWITCH_HISTORY_MAX) % SWITCH_HISTORY_MAX;
        if (g_switch_history[idx].timestamp < cutoff) break;
        count++;
    }
    return count;
}

static SwitchPair *find_or_create_pair(const char *a, const char *b)
{
    const char *lo = (strcmp(a, b) <= 0) ? a : b;
    const char *hi = (strcmp(a, b) <= 0) ? b : a;

    for (int i = 0; i < g_npairs; i++) {
        if (strcmp(g_pairs[i].comm_a, lo) == 0 &&
            strcmp(g_pairs[i].comm_b, hi) == 0)
            return &g_pairs[i];
    }

    if (g_npairs >= SWITCH_PAIR_MAX) return NULL;

    SwitchPair *p = &g_pairs[g_npairs++];
    strncpy(p->comm_a, lo, sizeof(p->comm_a) - 1);
    p->comm_a[sizeof(p->comm_a) - 1] = '\0';
    strncpy(p->comm_b, hi, sizeof(p->comm_b) - 1);
    p->comm_b[sizeof(p->comm_b) - 1] = '\0';
    p->last_switch = 0;
    p->count       = 0;
    return p;
}

void switch_update_pairs(void)
{
    time_t now    = time(NULL);
    time_t cutoff = now - SWITCH_PAIR_WINDOW;

    for (int i = 0; i < g_npairs; i++)
        g_pairs[i].count = 0;

    int total = g_switch_count;
    for (int i = 0; i < total; i++) {
        int idx = (g_switch_head - 1 - i + SWITCH_HISTORY_MAX) % SWITCH_HISTORY_MAX;
        SwitchEvent *ev = &g_switch_history[idx];

        if (ev->timestamp < cutoff) break;
        if (strcmp(ev->from_comm, ev->to_comm) == 0) continue;

        SwitchPair *p = find_or_create_pair(ev->from_comm, ev->to_comm);
        if (p) {
            p->count++;
            if (ev->timestamp > p->last_switch)
                p->last_switch = ev->timestamp;
        }
    }

    int i = 0;
    while (i < g_npairs) {
        if (g_pairs[i].count == 0 &&
            (now - g_pairs[i].last_switch) > PAIR_COOLDOWN_SEC) {
            g_pairs[i] = g_pairs[--g_npairs];
        } else {
            i++;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Only exempt if the OTHER member of the frequent pair is CURRENTLY focused  */
/* -------------------------------------------------------------------------- */
int switch_is_pair_exempt(const char *comm, const char *focused_comm)
{
    if (!focused_comm) return 0;
    for (int i = 0; i < g_npairs; i++) {
        if (g_pairs[i].count < SWITCH_PAIR_THRESHOLD) continue;
        
        int match_a = (strcmp(g_pairs[i].comm_a, comm) == 0 && strcmp(g_pairs[i].comm_b, focused_comm) == 0);
        int match_b = (strcmp(g_pairs[i].comm_b, comm) == 0 && strcmp(g_pairs[i].comm_a, focused_comm) == 0);
        
        if (match_a || match_b) return 1;
    }
    return 0;
}
