/*
 * tui.c  -  ncurses Text User Interface for ZrnPerformanceMgmnt
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"
#include <ncurses.h>

static void tui_fmt_dur(time_t secs, char *buf, size_t n)
{
    if (secs <= 0) {
        snprintf(buf, n, "-");
        return;
    }
    if (secs < 60)        snprintf(buf, n, "%lds", (long)secs);
    else if (secs < 3600) snprintf(buf, n, "%ldm%02lds", (long)(secs/60), (long)(secs%60));
    else                  snprintf(buf, n, "%ldh%02ldm", (long)(secs/3600), (long)((secs%3600)/60));
}

void tui_init(void)
{
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    use_default_colors();

    init_pair(1, COLOR_GREEN, -1);
    init_pair(2, COLOR_RED, -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_CYAN, -1);
    init_pair(5, COLOR_MAGENTA, -1);
    init_pair(6, COLOR_BLACK, COLOR_WHITE);
}

void tui_shutdown(void)
{
    endwin();
}

static int s_sel_idx = 0;

void tui_draw(Display *dpy, Window root, Atom net_wm_pid, Atom net_active_window)
{
    (void)dpy; (void)root; (void)net_wm_pid; (void)net_active_window;
    time_t now = time(NULL);

    static int show_exempt_list = 0;
    int ch = getch();
    if (ch == 'l' || ch == 'L') {
        show_exempt_list = !show_exempt_list;
    } else if (ch == 'q' || ch == 'Q') {
        g_running = 0;
        return;
    } else if (ch == KEY_UP && s_sel_idx > 0) {
        s_sel_idx--;
    } else if (ch == KEY_DOWN && s_sel_idx < g_nprocs - 1) {
        s_sel_idx++;
    } else if (ch == 'm' || ch == 'M') {
        int next_mode = (g_mode + 1) > MODE_HIGH ? MODE_NONE : (g_mode + 1);
        g_mode = next_mode;
        FILE *f = fopen(PROFILE_PATH, "w");
        if (f) { fprintf(f, "%s\n", mode_name(g_mode)); fclose(f); }
    } else if (ch == 'e' || ch == 'E') {
        if (s_sel_idx >= 0 && s_sel_idx < g_nprocs) {
            char *comm = g_procs[s_sel_idx].comm;
            if (exempt_check(comm)) exempt_remove(comm);
            else exempt_add(comm);
        }
    }

    if (g_nprocs > 0 && s_sel_idx >= g_nprocs) s_sel_idx = g_nprocs - 1;

    erase();

    attron(A_BOLD);
    mvprintw(0, 0, "=== ZrnPerformanceMgmnt ===");
    attroff(A_BOLD);

    mvprintw(1, 0, "Mode: ");
    attron(COLOR_PAIR(4));
    printw("%s", mode_name(g_mode));
    attroff(COLOR_PAIR(4));
    printw("  (Press 'm' mode, 'e' toggle exempt, 'l' view exempts, 'q' quit)");

    mvprintw(3, 0, "%-8s %-20s %-12s %-12s %-6s %-10s %-8s %-6s",
             "PID", "COMM", "STATUS", "UNFOCUSED", "CPU%", "AUDIO", "GPU", "EXEMPT");

    const char *focused_comm = "?";
    int f_idx = proc_find(g_focused_pid);
    if (f_idx >= 0) focused_comm = g_procs[f_idx].comm;

    int row = 4;
    for (int i = 0; i < g_nprocs; i++) {
        TrackedProc *p = &g_procs[i];

        if (i == s_sel_idx) attron(COLOR_PAIR(6));

        char status[16] = "Running";
        int color = 1;

        if (p->pid == g_focused_pid && g_dynboost.rapidboost_active) {
            snprintf(status, sizeof(status), "RapidBoost");
            color = 3;
        } else if (p->pid == g_focused_pid) {
            snprintf(status, sizeof(status), "Focused");
            color = 4;
        } else if (is_comm_exempt(p->comm, focused_comm)) {
            snprintf(status, sizeof(status), "Exempt");
            color = 1;
        } else if (p->audio_playing) {
            snprintf(status, sizeof(status), "Audio");
            color = 5;
        } else if (p->prewarm) {
            snprintf(status, sizeof(status), "Pre-warm");
            color = 3;
        } else if (p->throttled) {
            snprintf(status, sizeof(status), "Tickle(%.1fs)", 
                     (float)throttle_get_tick(p) / 1000.0f);
            color = 2;
        }

        char dur[32];
        tui_fmt_dur(now - p->defocus_time, dur, sizeof(dur));

        const char *audio = p->audio_playing ? "Playing" : "-";

        const char *gpu = "-";
        for (int g = 0; g < g_ngpu_procs; g++) {
            if (g_gpu_procs[g].pid == p->pid) { gpu = "Heavy"; break; }
        }

        const char *ex = exempt_check(p->comm) ? "Yes" : "-";

        char cpu_str[16];
        if (p->cpu_pct > 0.1) snprintf(cpu_str, sizeof(cpu_str), "%.1f%%", p->cpu_pct);
        else snprintf(cpu_str, sizeof(cpu_str), "-");

        if (i != s_sel_idx) attron(COLOR_PAIR(color));
        mvprintw(row++, 0, "%-8d %-20.20s %-12s %-12s %-6s %-10s %-8s %-6s",
                 p->pid, p->comm, status,
                 (p->defocus_time > 0) ? dur : "-",
                 cpu_str, audio, gpu, ex);
        if (i != s_sel_idx) attroff(COLOR_PAIR(color));

        if (i == s_sel_idx) attroff(COLOR_PAIR(6));
    }

    row++;
    attron(A_BOLD);
    mvprintw(row++, 0, "--- Active Frequent Switch Pairs ---");
    attroff(A_BOLD);

    if (!g_pairing_mode) {
        attron(COLOR_PAIR(6));
        mvprintw(row++, 0, "Pairing logic disabled (use --pairing to enable).");
        attroff(COLOR_PAIR(6));
    } else if (g_experimental) {
        attron(COLOR_PAIR(3));
        mvprintw(row++, 0, "[EXPERIMENTAL] Frequent-pair logic disabled (Neural Net active)");
        attroff(COLOR_PAIR(3));
    } else if (g_npairs == 0) {
        mvprintw(row++, 0, "No active pairs.");
    } else {
        for (int i = 0; i < g_npairs; i++) {
            long ago = now - g_pairs[i].last_switch;
            mvprintw(row++, 0, "%s <-> %s  (Switches: %d, Last: %lds ago)",
                     g_pairs[i].comm_a, g_pairs[i].comm_b,
                     g_pairs[i].count, ago);
        }
    }

    row++;
    attron(A_BOLD);
    mvprintw(row++, 0, "--- Dynamic Boost ---");
    attroff(A_BOLD);

    if (g_dynboost.rapidboost_active) {
        attron(COLOR_PAIR(3));
        if (g_dynboost.rapidboost_on_ac) {
            mvprintw(row++, 0, "[RapidBoost ACTIVE (AC)]  CPU: %lu MHz -> %lu MHz (Turbo Boost)",
                     (unsigned long)CPU_MAX_FREQ_ON_AC_KHZ / 1000, g_dynboost.boost_target_khz / 1000);
        } else {
            mvprintw(row++, 0, "[RapidBoost ACTIVE (Battery)]  CPU: %lu MHz -> %lu MHz  |  Kbd: OFF  |  Disp: 25%%",
                     (unsigned long)CPU_MAX_FREQ_ON_BAT_KHZ / 1000, g_dynboost.boost_target_khz / 1000);
        }
        attroff(COLOR_PAIR(3));
    } else {
        mvprintw(row++, 0, "RapidBoost: standby");
    }

    if (g_experimental && g_dynboost.boostml_collecting) {
        attron(COLOR_PAIR(5));
        mvprintw(row++, 0, "BoostML: Collecting data  |  GPU: %s",
                 g_dynboost.gpu_available ? "HD3000 (GL 3.3)" : "unavailable");
        attroff(COLOR_PAIR(5));
    } else if (g_experimental) {
        mvprintw(row++, 0, "BoostML: OFF");
    }

    if (g_experimental) {
        row++;
        attron(A_BOLD);
        mvprintw(row++, 0, "--- Next Window Predictions (GPU Neural Net + RL) ---");
        attroff(A_BOLD);

        if (g_nn_state.count > 0) {
            for (int i = 0; i < g_nn_state.count && i < 3; i++) {
                int bars = (int)(g_nn_state.top[i].prob * 20.0f);
                char bar_str[24];
                memset(bar_str, ' ', 20);
                for (int b = 0; b < bars && b < 20; b++) bar_str[b] = '=';
                bar_str[20] = '\0';

                int is_pred = (strcmp(g_nn_state.top[i].comm, g_nn_state.last_predicted) == 0);
                if (is_pred) attron(COLOR_PAIR(3) | A_BOLD);
                mvprintw(row++, 0, "  %d. %-16.16s [%s] %5.1f%%%s",
                         i + 1, g_nn_state.top[i].comm, bar_str,
                         g_nn_state.top[i].prob * 100.0f,
                         is_pred ? "  <- [Pre-warmed]" : "");
                if (is_pred) attroff(COLOR_PAIR(3) | A_BOLD);
            }
        } else {
            mvprintw(row++, 0, "  Model standby (collecting transitions...)");
        }

        if (g_nn_state.total_predictions > 0) {
            float acc = (float)g_nn_state.total_correct / (float)g_nn_state.total_predictions * 100.0f;
            const char *rw_str = (g_nn_state.last_reward > 0) ? "Reward (+1.0 Correct)" : "Penalty (-1.0 Corrected)";
            int rw_col = (g_nn_state.last_reward > 0) ? 2 : 3;
            attron(COLOR_PAIR(rw_col));
            mvprintw(row++, 0, "  RL Status: %s | Loss: %.4f | Online Accuracy: %.1f%% (%d/%d)",
                     rw_str, g_nn_state.last_loss, acc,
                     g_nn_state.total_correct, g_nn_state.total_predictions);
            attroff(COLOR_PAIR(rw_col));
        }
    }


    if (show_exempt_list) {
        row++;
        attron(A_BOLD);
        mvprintw(row++, 0, "--- Permanent Exemptions List ---");
        attroff(A_BOLD);
        if (g_nexempt == 0) {
            mvprintw(row++, 0, "No exempted applications.");
        } else {
            for (int i = 0; i < g_nexempt; i++) {
                mvprintw(row++, 0, " - %s", g_exempt[i].comm);
            }
        }
    }
    refresh();
}
