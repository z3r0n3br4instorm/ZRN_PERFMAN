/*
 * proctrack.c  –  Maintain the table of tracked X application PIDs
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"
#include <dirent.h>
#include <sys/wait.h>
#include <signal.h>
#include <ctype.h>

int read_comm(pid_t pid, char *out, size_t n)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    if (!fgets(out, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    out[strcspn(out, "\n")] = '\0';
    return 0;
}

static int pid_alive(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d", (int)pid);
    return access(path, F_OK) == 0;
}

int proc_find(pid_t pid)
{
    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i].pid == pid)
            return i;
    return -1;
}

int proc_add(pid_t pid)
{
    if (proc_find(pid) >= 0) return -1;
    if (g_nprocs >= MAX_TRACKED) return -1;

    int i = g_nprocs++;
    memset(&g_procs[i], 0, sizeof(g_procs[i]));
    g_procs[i].pid          = pid;
    g_procs[i].first_seen   = time(NULL);
    g_procs[i].defocus_time = 0;
    g_procs[i].throttled    = 0;
    g_procs[i].ndescendants = 0;

    if (read_comm(pid, g_procs[i].comm, sizeof(g_procs[i].comm)) < 0)
        strncpy(g_procs[i].comm, "?", sizeof(g_procs[i].comm));

    if (!g_tui_mode && !g_gui_mode) {
        printf("[proctrack] +track  pid=%-7d  comm=%s\n", (int)pid, g_procs[i].comm);
        fflush(stdout);
    }
    return i;
}

void proc_remove_dead(void)
{
    int i = 0;
    while (i < g_nprocs) {
        if (!pid_alive(g_procs[i].pid)) {
            if (!g_tui_mode && !g_gui_mode) {
                printf("[proctrack] -dead   pid=%-7d  comm=%s\n",
                       (int)g_procs[i].pid, g_procs[i].comm);
                fflush(stdout);
            }
            if (g_procs[i].cpulimit_pid > 0) {
                kill(g_procs[i].cpulimit_pid, SIGKILL);
                waitpid(g_procs[i].cpulimit_pid, NULL, WNOHANG);
            }
            g_procs[i] = g_procs[--g_nprocs];
        } else {
            i++;
        }
    }
}

typedef struct { pid_t pid; pid_t ppid; } ProcRel;

void proctrack_update_descendants(void)
{
    DIR *proc = opendir("/proc");
    if (!proc) return;
    
    ProcRel *rels = malloc(sizeof(ProcRel) * 8192);
    if (!rels) { closedir(proc); return; }
    int nrels = 0;
    
    struct dirent *de;
    while ((de = readdir(proc)) != NULL && nrels < 8192) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        pid_t pid = atoi(de->d_name);
        
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (f) {
            char comm[256];
            char state;
            pid_t ppid = 0;
            if (fscanf(f, "%*d (%255[^)]) %c %d", comm, &state, &ppid) == 3) {
                rels[nrels].pid = pid;
                rels[nrels].ppid = ppid;
                nrels++;
            }
            fclose(f);
        }
    }
    closedir(proc);
    
    for (int i = 0; i < g_nprocs; i++) {
        TrackedProc *p = &g_procs[i];
        p->ndescendants = 0;
        
        pid_t queue[64];
        int qh = 0, qt = 0;
        queue[qt++] = p->pid;
        
        while (qh < qt) {
            pid_t curr = queue[qh++];
            for (int j = 0; j < nrels; j++) {
                if (rels[j].ppid == curr) {
                    if (p->ndescendants < 64) {
                        p->descendants[p->ndescendants++] = rels[j].pid;
                    }
                    if (qt < 64) {
                        queue[qt++] = rels[j].pid;
                    }
                }
            }
        }
    }
    
    free(rels);
}
