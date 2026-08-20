/*
 * exempt.c  -  User-configured exemption list
 *
 * Apps whose comm name appears in ~/.zrnperformanceexempt (one per line)
 * are never throttled.  The TUI can add/remove entries at runtime and
 * persist them to disk.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"
#include <ctype.h>

ExemptEntry g_exempt[MAX_EXEMPT];
int         g_nexempt = 0;

/* -------------------------------------------------------------------------- */
/* Load exemption list from disk                                               */
/* -------------------------------------------------------------------------- */
void exempt_load(const char *path)
{
    g_nexempt = 0;

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f) && g_nexempt < MAX_EXEMPT) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) ++p;
        if (*p == '#' || *p == '\0') continue;

        size_t len = strlen(p);
        while (len > 0 && isspace((unsigned char)p[len - 1])) p[--len] = '\0';
        if (len == 0) continue;

        snprintf(g_exempt[g_nexempt].comm, sizeof(g_exempt[0].comm), "%s", p);
        g_nexempt++;
    }
    fclose(f);
}

/* -------------------------------------------------------------------------- */
/* Save exemption list to disk                                                 */
/* -------------------------------------------------------------------------- */
void exempt_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "# ZrnPerformanceMgmnt - Exempted applications\n");
    fprintf(f, "# One process name per line (from /proc/PID/comm)\n\n");

    for (int i = 0; i < g_nexempt; i++)
        fprintf(f, "%s\n", g_exempt[i].comm);

    fclose(f);
}

/* -------------------------------------------------------------------------- */
/* Add a comm name to the exemption list                                       */
/* -------------------------------------------------------------------------- */
void exempt_add(const char *comm)
{
    if (exempt_check(comm)) return;  /* already exempt */
    if (g_nexempt >= MAX_EXEMPT) return;

    strncpy(g_exempt[g_nexempt].comm, comm, sizeof(g_exempt[0].comm) - 1);
    g_exempt[g_nexempt].comm[sizeof(g_exempt[0].comm) - 1] = '\0';
    g_nexempt++;

    exempt_save(EXEMPT_PATH);
}

/* -------------------------------------------------------------------------- */
/* Remove a comm name from the exemption list                                  */
/* -------------------------------------------------------------------------- */
void exempt_remove(const char *comm)
{
    for (int i = 0; i < g_nexempt; i++) {
        if (strcmp(g_exempt[i].comm, comm) == 0) {
            g_exempt[i] = g_exempt[--g_nexempt];
            exempt_save(EXEMPT_PATH);
            return;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Check if a comm name is exempt                                              */
/* -------------------------------------------------------------------------- */
int exempt_check(const char *comm)
{
    for (int i = 0; i < g_nexempt; i++)
        if (strcmp(g_exempt[i].comm, comm) == 0)
            return 1;
    return 0;
}
