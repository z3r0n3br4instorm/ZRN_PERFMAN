/*
 * profile.c  -  Read/Write ~/.zrnperformanceprofile
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"
#include <glob.h>
#include <sys/wait.h>
#include <fcntl.h>

const char *mode_name(PerfMode m)
{
    switch (m) {
        case MODE_NONE:     return "None";
        case MODE_NOMINAL:  return "Nominal";
        case MODE_MODERATE: return "Moderate";
        case MODE_HIGH:     return "High";
    }
    return "Unknown";
}

/* AC/battery is read directly (see power_source.c) rather than via TLP:
 * TLP's own change-detection cache could get stuck and silently skip
 * re-evaluating on a real power source change. */
PerfMode profile_detect_system(void)
{
    return power_on_ac() ? MODE_NOMINAL : MODE_HIGH;
}

void profile_init(void)
{
    g_mode = profile_detect_system();
    
    FILE *f = fopen(PROFILE_PATH, "w");
    if (f) {
        fprintf(f, "%s\n", mode_name(g_mode));
        fclose(f);
    }
}

void profile_reload(void)
{
    FILE *f = fopen(PROFILE_PATH, "r");
    if (!f) return;
    char line[64];
    if (fgets(line, sizeof(line), f)) {
        if (strncasecmp(line, "None", 4) == 0)          g_mode = MODE_NONE;
        else if (strncasecmp(line, "Nominal", 7) == 0)  g_mode = MODE_NOMINAL;
        else if (strncasecmp(line, "Moderate", 8) == 0) g_mode = MODE_MODERATE;
        else if (strncasecmp(line, "High", 4) == 0)     g_mode = MODE_HIGH;
    }
    fclose(f);
}
