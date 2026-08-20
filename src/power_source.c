/*
 * power_source.c  -  Direct AC/battery detection via sysfs (no TLP)
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"
#include <glob.h>

int power_on_ac(void)
{
    glob_t gl;
    memset(&gl, 0, sizeof(gl));
    if (glob("/sys/class/power_supply/*/type", 0, NULL, &gl) != 0)
        return 1;   /* assume AC if undetectable - never over-throttle */

    int on_ac = 1;
    for (size_t i = 0; i < gl.gl_pathc; i++) {
        FILE *f = fopen(gl.gl_pathv[i], "r");
        if (!f) continue;
        char type[32] = "";
        int ok = (fscanf(f, "%31s", type) == 1);
        fclose(f);
        if (!ok) continue;

        if (strcmp(type, "Mains") == 0) {
            char online_path[300];
            snprintf(online_path, sizeof(online_path), "%s", gl.gl_pathv[i]);
            char *slash = strrchr(online_path, '/');
            if (slash) {
                strcpy(slash + 1, "online");
                FILE *of = fopen(online_path, "r");
                int val = 1;
                if (of) {
                    if (fscanf(of, "%d", &val) != 1) val = 1;
                    fclose(of);
                }
                on_ac = val;
            }
            break;
        }
    }
    globfree(&gl);
    return on_ac;
}
