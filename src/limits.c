#include "zrn_perf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

LimitEntry g_limits[MAX_EXEMPT];
int g_nlimits = 0;

void limits_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        g_nlimits = 0;
        return;
    }

    g_nlimits = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && g_nlimits < MAX_EXEMPT) {
        char comm[64];
        int pct = 0;
        if (sscanf(line, "%63s %d", comm, &pct) == 2) {
            if (pct > 0) {
                strncpy(g_limits[g_nlimits].comm, comm, 63);
                g_limits[g_nlimits].comm[63] = '\0';
                g_limits[g_nlimits].limit_pct = pct;
                g_nlimits++;
            }
        }
    }
    fclose(f);
}

void limit_add(const char *comm, int limit_pct)
{
    for (int i = 0; i < g_nlimits; i++) {
        if (strcmp(g_limits[i].comm, comm) == 0) {
            g_limits[i].limit_pct = limit_pct;
            goto save;
        }
    }
    if (g_nlimits < MAX_EXEMPT) {
        strncpy(g_limits[g_nlimits].comm, comm, 63);
        g_limits[g_nlimits].comm[63] = '\0';
        g_limits[g_nlimits].limit_pct = limit_pct;
        g_nlimits++;
    }
save:
    limits_save(LIMITS_PATH);
}

void limit_remove(const char *comm)
{
    int found = 0;
    for (int i = 0; i < g_nlimits; i++) {
        if (strcmp(g_limits[i].comm, comm) == 0) {
            found = 1;
        }
        if (found && i < g_nlimits - 1) {
            g_limits[i] = g_limits[i+1];
        }
    }
    if (found) {
        g_nlimits--;
        limits_save(LIMITS_PATH);
    }
}

int limit_check(const char *comm)
{
    for (int i = 0; i < g_nlimits; i++) {
        if (strcmp(g_limits[i].comm, comm) == 0) {
            return g_limits[i].limit_pct;
        }
    }
    return 0;
}

void limits_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_nlimits; i++) {
        fprintf(f, "%s %d\n", g_limits[i].comm, g_limits[i].limit_pct);
    }
    fclose(f);
}

void limits_apply_all(void)
{
    for (int i = 0; i < g_nprocs; i++) {
        TrackedProc *p = &g_procs[i];
        int target_pct = limit_check(p->comm);

        if (p->cpu_limit_pct != target_pct) {
            if (p->cpulimit_pid > 0) {
                kill(p->cpulimit_pid, SIGKILL);
                waitpid(p->cpulimit_pid, NULL, WNOHANG);
                p->cpulimit_pid = 0;
            }
            p->cpu_limit_pct = target_pct;
        }

        if (p->cpu_limit_pct > 0 && p->cpulimit_pid <= 0) {
            pid_t cpid = fork();
            if (cpid == 0) {
                char pid_str[32], pct_str[32];
                snprintf(pid_str, sizeof(pid_str), "%d", p->pid);
                snprintf(pct_str, sizeof(pct_str), "%d", p->cpu_limit_pct);
                int fd = open("/dev/null", O_WRONLY);
                if (fd >= 0) {
                    dup2(fd, STDOUT_FILENO);
                    dup2(fd, STDERR_FILENO);
                    close(fd);
                }
                execlp("cpulimit", "cpulimit", "-p", pid_str, "-l", pct_str, "-z", "-i", NULL);
                _exit(1);
            } else if (cpid > 0) {
                p->cpulimit_pid = cpid;
                /* If the process is currently throttled by our daemon, pause cpulimit immediately */
                if (p->throttled) {
                    kill(cpid, SIGSTOP);
                }
            }
        }
    }
}
