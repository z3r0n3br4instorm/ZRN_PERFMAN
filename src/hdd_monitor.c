/*
 * hdd_monitor.c  -  Spin down the spinning HDD when idle on battery
 *
 * Spin-down is explicit (hdparm -y). Spin-up is not: touching the device
 * wakes it transparently at the ATA layer, so we only need to notice that
 * activity happened and stop treating the drive as standby.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"
#include <dirent.h>
#include <sys/wait.h>
#include <fcntl.h>

HddMonState g_hddmon;

static int is_candidate_hdd(const char *name)
{
    if (strncmp(name, "loop", 4) == 0) return 0;
    if (strncmp(name, "sr", 2) == 0)   return 0;
    if (strncmp(name, "dm-", 3) == 0)  return 0;
    if (strncmp(name, "zram", 4) == 0) return 0;
    return 1;
}

static void discover_hdd(void)
{
    DIR *d = opendir("/sys/block");
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_candidate_hdd(de->d_name)) continue;

        char path[128];
        snprintf(path, sizeof(path), "/sys/block/%s/queue/rotational", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int rota = 0;
        int ok = (fscanf(f, "%d", &rota) == 1);
        fclose(f);

        if (ok && rota == 1) {
            strncpy(g_hddmon.stat_name, de->d_name, sizeof(g_hddmon.stat_name) - 1);
            snprintf(g_hddmon.device_path, sizeof(g_hddmon.device_path),
                     "/dev/%s", de->d_name);
            break;
        }
    }
    closedir(d);
}

static unsigned long long read_hdd_io_ticks(void)
{
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return 0;

    char line[256];
    unsigned long long ticks = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned major, minor;
        char name[32];
        unsigned long long reads_completed, reads_merged, sectors_read, time_reading,
                            writes_completed, writes_merged, sectors_written;

        if (sscanf(line, "%u %u %31s %llu %llu %llu %llu %llu %llu %llu",
                   &major, &minor, name,
                   &reads_completed, &reads_merged, &sectors_read, &time_reading,
                   &writes_completed, &writes_merged, &sectors_written) < 10)
            continue;

        if (strcmp(name, g_hddmon.stat_name) == 0) {
            ticks = sectors_read + sectors_written;
            break;
        }
    }
    fclose(f);
    return ticks;
}

static void hdd_spin_down(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execlp("hdparm", "hdparm", "-y", g_hddmon.device_path, (char *)NULL);
        _exit(1);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

void hddmon_init(void)
{
    memset(&g_hddmon, 0, sizeof(g_hddmon));
    discover_hdd();

    if (!g_hddmon.device_path[0]) {
        if (!g_tui_mode && !g_gui_mode) {
            printf("[hddmon] No rotational HDD found - disk power management disabled\n");
            fflush(stdout);
        }
        return;
    }

    g_hddmon.last_io_ticks = read_hdd_io_ticks();
    g_hddmon.last_activity = time(NULL);

    if (!g_tui_mode && !g_gui_mode) {
        printf("[hddmon] Managing %s (stat name: %s)\n",
               g_hddmon.device_path, g_hddmon.stat_name);
        fflush(stdout);
    }
}

void hddmon_tick(void)
{
    if (!g_hddmon.device_path[0]) return;
    if (g_monitor_only) return;

    time_t now = time(NULL);
    g_hddmon.on_ac = power_on_ac();

    unsigned long long ticks = read_hdd_io_ticks();
    int activity = (ticks != g_hddmon.last_io_ticks);
    g_hddmon.last_io_ticks = ticks;

    if (activity) {
        g_hddmon.last_activity = now;
        if (g_hddmon.standby) {
            g_hddmon.standby = 0;
            dynboost_notify("Dynamic Mechanical Power",
                            "HDD spun back up.");
            if (!g_tui_mode && !g_gui_mode) {
                printf("[hddmon] %s activity detected, drive spun back up\n",
                       g_hddmon.device_path);
                fflush(stdout);
            }
        }
        return;
    }

    if (g_hddmon.on_ac) {
        /* Only manage spin-down on battery; leave AC behaviour untouched */
        g_hddmon.standby = 0;
        return;
    }

    if (!g_hddmon.standby
        && (now - g_hddmon.last_activity) >= HDD_IDLE_TIMEOUT_SEC) {
        hdd_spin_down();
        g_hddmon.standby = 1;
        dynboost_notify("Dynamic Mechanical Power",
                        "HDD idle on battery, spinning down.");
        if (!g_tui_mode && !g_gui_mode) {
            printf("[hddmon] %s idle %lds on battery, spinning down\n",
                   g_hddmon.device_path, (long)(now - g_hddmon.last_activity));
            fflush(stdout);
        }
    }
}
