/*
 * cpu_power.c  -  Direct CPU governor/frequency management by AC/battery
 * state. Replaces TLP for this purpose: TLP's own AC/battery re-evaluation
 * proved unreliable (its change-detection cache could get stuck, silently
 * skipping re-application on a real power source change).
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"

static int s_last_on_ac = -1;   /* -1 = not yet applied */

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

static int sysfs_write_str(const char *path, const char *val)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%s\n", val);
    fclose(f);
    return 0;
}

static void apply_cpu_profile(int on_ac)
{
    /* The hardware ceiling/floor (cpuinfo_min/max_freq) isn't always a fixed
     * constant on this machine - it can move with thermal/BD_PROCHOT state.
     * Read it fresh and clamp our targets into it, rather than assuming our
     * configured absolutes are always achievable. */
    unsigned long hw_min = 0, hw_max = 0;
    sysfs_read_ulong("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq", &hw_min);
    sysfs_read_ulong("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", &hw_max);

    unsigned long min_khz = on_ac ? CPU_MIN_FREQ_ON_AC_KHZ  : CPU_MIN_FREQ_ON_BAT_KHZ;
    unsigned long max_khz = on_ac ? CPU_MAX_FREQ_ON_AC_KHZ  : CPU_MAX_FREQ_ON_BAT_KHZ;
    const char   *governor = on_ac ? CPU_GOVERNOR_ON_AC     : CPU_GOVERNOR_ON_BAT;

    if (hw_min > 0 && min_khz < hw_min) min_khz = hw_min;
    if (hw_max > 0 && max_khz > hw_max) max_khz = hw_max;
    if (min_khz > max_khz) min_khz = max_khz;

    for (int i = 0; i < 256; i++) {
        char max_path[128];
        snprintf(max_path, sizeof(max_path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", i);
        if (access(max_path, F_OK) != 0) break;

        /* Try governor if writable (non-fatal if unprivileged) */
        char gov_path[128];
        snprintf(gov_path, sizeof(gov_path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
        sysfs_write_str(gov_path, governor);

        /* Drop min to the hardware floor first so the max write below can
         * never be rejected by the kernel's min<=max constraint */
        char min_path[128];
        snprintf(min_path, sizeof(min_path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", i);
        if (hw_min > 0) sysfs_write_ulong(min_path, hw_min);

        /* Write scaling_max_freq (writable by video group) */
        sysfs_write_ulong(max_path, max_khz);

        /* Try restoring scaling_min_freq if writable */
        sysfs_write_ulong(min_path, min_khz);
    }

    if (!g_tui_mode && !g_gui_mode) {
        printf("[cpu_power] Applied %s profile: governor=%s min=%lu kHz max=%lu kHz "
               "(hw range %lu-%lu kHz)\n",
               on_ac ? "AC" : "battery", governor, min_khz, max_khz, hw_min, hw_max);
        fflush(stdout);
    }
}

void cpu_power_init(void)
{
    if (g_monitor_only) return;
    s_last_on_ac = power_on_ac();
    apply_cpu_profile(s_last_on_ac);
}

void cpu_power_tick(void)
{
    if (g_monitor_only) return;

    int on_ac = power_on_ac();
    if (on_ac == s_last_on_ac) return;

    s_last_on_ac = on_ac;
    apply_cpu_profile(on_ac);
    dynboost_notify("Dynamic Boost System",
                    on_ac ? "AC connected. CPU restored to performance profile."
                          : "On battery. CPU capped to power-saving profile.");
}
