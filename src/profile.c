/*
 * profile.c  -  Read/Write ~/.zrnperformanceprofile
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"

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

#include <ctype.h>

PerfMode profile_detect_system(void)
{
    FILE *f = popen("tlp-stat -s 2>/dev/null", "r");
    char line[256];
    char profile_str[64] = "";

    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "TLP profile    =", 16) == 0) {
                char *p = line + 16;
                while (*p && isspace((unsigned char)*p)) p++;
                strncpy(profile_str, p, sizeof(profile_str)-1);
                break;
            }
        }
        pclose(f);
    }
    
    if (strstr(profile_str, "power-saver") || strstr(profile_str, "powersave") || strstr(profile_str, "battery")) return MODE_HIGH;
    if (strstr(profile_str, "performance")) return MODE_NOMINAL;
    if (strstr(profile_str, "balanced")) return MODE_MODERATE;
    
    return MODE_NOMINAL;
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
