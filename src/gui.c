/*
 * gui.c  -  GTK3 GUI for ZrnPerformanceMgmnt (Openbox-compatible)
 */
#ifdef ENABLE_GUI

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"
#include <gtk/gtk.h>

enum { COL_PID, COL_COMM, COL_STATUS, COL_UNFOCUSED, COL_AUDIO, COL_GPU, COL_EXEMPT, NUM_COLS };
enum { COL_EX_COMM, NUM_EX_COLS };

static GtkWidget    *s_window     = NULL;
static GtkListStore *s_store      = NULL;
static GtkWidget    *s_tree       = NULL;
static GtkListStore *s_ex_store   = NULL;
static GtkWidget    *s_ex_tree    = NULL;
static GtkWidget    *s_statusbar  = NULL;
static GtkWidget    *s_mode_combo = NULL;

static void gui_refresh_store(void);
static void gui_refresh_exempt_store(void);

static void fmt_dur(time_t secs, char *buf, size_t n)
{
    if (secs <= 0) { snprintf(buf, n, "-"); return; }
    if (secs < 60)        snprintf(buf, n, "%lds", (long)secs);
    else if (secs < 3600) snprintf(buf, n, "%ldm%02lds", (long)(secs/60), (long)(secs%60));
    else                  snprintf(buf, n, "%ldh%02ldm", (long)(secs/3600), (long)((secs%3600)/60));
}

static void on_mode_changed(GtkComboBoxText *combo, gpointer data)
{
    (void)data;
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    switch (idx) {
        case 0: g_mode = MODE_NONE;     break;
        case 1: g_mode = MODE_NOMINAL;  break;
        case 2: g_mode = MODE_MODERATE; break;
        case 3: g_mode = MODE_HIGH;     break;
    }
    FILE *f = fopen(PROFILE_PATH, "w");
    if (f) { fprintf(f, "%s\n", mode_name(g_mode)); fclose(f); }
}

static void on_exempt_clicked(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(s_tree));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return;

    gchar *comm = NULL;
    gtk_tree_model_get(model, &iter, COL_COMM, &comm, -1);
    if (!comm) return;

    if (exempt_check(comm)) exempt_remove(comm);
    else exempt_add(comm);
    g_free(comm);
    
    gui_refresh_store();
    gui_refresh_exempt_store();
}

static void on_ex_remove_clicked(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(s_ex_tree));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return;

    gchar *comm = NULL;
    gtk_tree_model_get(model, &iter, COL_EX_COMM, &comm, -1);
    if (!comm) return;

    exempt_remove(comm);
    g_free(comm);

    gui_refresh_store();
    gui_refresh_exempt_store();
}

static gboolean on_timer(gpointer data)
{
    (void)data;
    if (!g_running) { gtk_main_quit(); return FALSE; }
    gui_refresh_store();
    gui_refresh_exempt_store();
    gtk_combo_box_set_active(GTK_COMBO_BOX(s_mode_combo), (int)g_mode + 1);
    return TRUE;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)w; (void)ev; (void)data;
    g_running = 0;
    gtk_main_quit();
    return TRUE;
}

void gui_init(int *argc, char ***argv)
{
    gtk_init(argc, argv);

    s_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(s_window), g_monitor_only ? "ZrnPerformanceMgmnt (Monitor)" : "ZrnPerformanceMgmnt");
    gtk_window_set_default_size(GTK_WINDOW(s_window), 850, 520);
    gtk_window_set_position(GTK_WINDOW(s_window), GTK_WIN_POS_CENTER);
    g_signal_connect(s_window, "delete-event", G_CALLBACK(on_delete), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);
    gtk_container_add(GTK_CONTAINER(s_window), vbox);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    GtkWidget *lbl = gtk_label_new("Mode:");
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);

    s_mode_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_mode_combo), "None");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_mode_combo), "Nominal");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_mode_combo), "Moderate");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_mode_combo), "High");
    gtk_combo_box_set_active(GTK_COMBO_BOX(s_mode_combo), (int)g_mode + 1);
    g_signal_connect(s_mode_combo, "changed", G_CALLBACK(on_mode_changed), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), s_mode_combo, FALSE, FALSE, 0);

    GtkWidget *exp_lbl = gtk_label_new("");
    if (g_experimental)
        gtk_label_set_markup(GTK_LABEL(exp_lbl), "<span color='orange'><b>[EXPERIMENTAL]</b></span>");
    gtk_box_pack_start(GTK_BOX(hbox), exp_lbl, FALSE, FALSE, 0);

    /* Notebook */
    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);

    /* TAB 1: Processes */
    GtkWidget *proc_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(proc_vbox), 4);
    
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *exempt_btn = gtk_button_new_with_label("Toggle Exempt for Selected");
    g_signal_connect(exempt_btn, "clicked", G_CALLBACK(on_exempt_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(btn_box), exempt_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(proc_vbox), btn_box, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(proc_vbox), scroll, TRUE, TRUE, 0);

    s_store = gtk_list_store_new(NUM_COLS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    s_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s_store));
    g_object_unref(s_store);

    const char *titles[] = {"PID", "Process", "Status", "Unfocused", "Audio", "GPU", "Exempt"};
    for (int i = 0; i < NUM_COLS; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(titles[i], r, "text", i, NULL);
        gtk_tree_view_column_set_resizable(c, TRUE);
        if (i == COL_COMM) gtk_tree_view_column_set_min_width(c, 150);
        gtk_tree_view_append_column(GTK_TREE_VIEW(s_tree), c);
    }
    gtk_container_add(GTK_CONTAINER(scroll), s_tree);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), proc_vbox, gtk_label_new("Processes"));

    /* TAB 2: Exemptions */
    GtkWidget *ex_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(ex_vbox), 4);
    
    GtkWidget *ex_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *ex_rem_btn = gtk_button_new_with_label("Remove Selected from Exemptions");
    g_signal_connect(ex_rem_btn, "clicked", G_CALLBACK(on_ex_remove_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(ex_btn_box), ex_rem_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ex_vbox), ex_btn_box, FALSE, FALSE, 0);

    GtkWidget *ex_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ex_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(ex_vbox), ex_scroll, TRUE, TRUE, 0);

    s_ex_store = gtk_list_store_new(NUM_EX_COLS, G_TYPE_STRING);
    s_ex_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s_ex_store));
    g_object_unref(s_ex_store);

    GtkCellRenderer *ex_r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *ex_c = gtk_tree_view_column_new_with_attributes("Exempted Process Name (comm)", ex_r, "text", 0, NULL);
    gtk_tree_view_column_set_resizable(ex_c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(s_ex_tree), ex_c);
    
    gtk_container_add(GTK_CONTAINER(ex_scroll), s_ex_tree);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), ex_vbox, gtk_label_new("Exemptions"));

    /* Status Bar */
    s_statusbar = gtk_statusbar_new();
    gtk_box_pack_start(GTK_BOX(vbox), s_statusbar, FALSE, FALSE, 0);

    gtk_widget_show_all(s_window);
    g_timeout_add(500, on_timer, NULL);
    gui_refresh_store();
    gui_refresh_exempt_store();
}

static void gui_refresh_store(void)
{
    time_t now = time(NULL);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s_store), &iter);

    const char *focused_comm = "?";
    int f_idx = proc_find(g_focused_pid);
    if (f_idx >= 0) focused_comm = g_procs[f_idx].comm;

    for (int i = 0; i < g_nprocs; i++) {
        TrackedProc *p = &g_procs[i];
        if (!valid) gtk_list_store_append(s_store, &iter);

        const char *status;
        if (p->pid == g_focused_pid)       status = "Focused";
        else if (is_comm_exempt(p->comm, focused_comm)) status = "Exempt";
        else if (p->audio_playing)         status = "Audio Exempt";
        else if (p->prewarm)               status = "Pre-warm";
        else if (p->throttled)             status = "Throttled";
        else                               status = "Running";

        char dur[32];
        if (p->defocus_time > 0) fmt_dur(now - p->defocus_time, dur, sizeof(dur));
        else snprintf(dur, sizeof(dur), "-");

        const char *audio = p->audio_playing ? "Playing" : "-";

        const char *gpu = "-";
        for (int g = 0; g < g_ngpu_procs; g++) {
            if (g_gpu_procs[g].pid == p->pid) { gpu = "Heavy"; break; }
        }

        const char *ex = exempt_check(p->comm) ? "Yes" : "-";

        gtk_list_store_set(s_store, &iter,
            COL_PID,       (int)p->pid,
            COL_COMM,      p->comm,
            COL_STATUS,    status,
            COL_UNFOCUSED, dur,
            COL_AUDIO,     audio,
            COL_GPU,       gpu,
            COL_EXEMPT,    ex, -1);
            
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(s_store), &iter);
    }
    while (valid) valid = gtk_list_store_remove(s_store, &iter);

    char sb[256];
    const char *mon_str = g_monitor_only ? "  |  [Daemon Connected: Monitor]" : "";
    if (g_pairing_mode) {
        snprintf(sb, sizeof(sb), "Mode: %s  |  Tracked: %d  |  Pairs: %d  |  GPU procs: %d%s",
                 mode_name(g_mode), g_nprocs, g_npairs, g_ngpu_procs, mon_str);
    } else {
        snprintf(sb, sizeof(sb), "Mode: %s  |  Tracked: %d  |  Pairs: OFF  |  GPU procs: %d%s",
                 mode_name(g_mode), g_nprocs, g_ngpu_procs, mon_str);
    }
    gtk_statusbar_pop(GTK_STATUSBAR(s_statusbar), 0);
    gtk_statusbar_push(GTK_STATUSBAR(s_statusbar), 0, sb);
}

static void gui_refresh_exempt_store(void)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s_ex_store), &iter);

    for (int i = 0; i < g_nexempt; i++) {
        if (!valid) gtk_list_store_append(s_ex_store, &iter);
        gtk_list_store_set(s_ex_store, &iter, COL_EX_COMM, g_exempt[i].comm, -1);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(s_ex_store), &iter);
    }
    while (valid) valid = gtk_list_store_remove(s_ex_store, &iter);
}

void gui_run(void) { gtk_main(); }
void gui_shutdown(void) { if (s_window) { gtk_widget_destroy(s_window); s_window = NULL; } }

#else
#include <stdio.h>
void gui_init(int *argc, char ***argv) { (void)argc; (void)argv; fprintf(stderr, "[zrn] GUI not available\n"); }
void gui_run(void) { }
void gui_shutdown(void) { }
#endif
