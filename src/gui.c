/*
 * gui.c  -  GTK3 GUI for ZrnPerformanceMgmnt (Openbox-compatible)
 */
#ifdef ENABLE_GUI

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "zrn_perf.h"
#include <gtk/gtk.h>

enum { COL_PID, COL_COMM, COL_STATUS, COL_UNFOCUSED, COL_CPU, COL_LIMIT, COL_AUDIO, COL_GPU, COL_EXEMPT, NUM_COLS };
enum { COL_EX_COMM, NUM_EX_COLS };
enum { COL_LIM_COMM, COL_LIM_PCT, NUM_LIM_COLS };
enum { COL_PRED_RANK, COL_PRED_COMM, COL_PRED_PROB, COL_PRED_STATUS, NUM_PRED_COLS };

static GtkWidget    *s_window     = NULL;
static GtkListStore *s_store      = NULL;
static GtkWidget    *s_tree       = NULL;
static GtkListStore *s_ex_store   = NULL;
static GtkWidget    *s_ex_tree    = NULL;
static GtkListStore *s_lim_store  = NULL;
static GtkWidget    *s_lim_tree   = NULL;
static GtkListStore *s_pred_store = NULL;
static GtkWidget    *s_pred_tree  = NULL;
static GtkWidget    *s_rl_label   = NULL;
static GtkWidget    *s_statusbar  = NULL;
static GtkWidget    *s_mode_combo = NULL;

static void gui_refresh_store(void);
static void gui_refresh_exempt_store(void);
static void gui_refresh_limits_store(void);
static void gui_refresh_pred_store(void);

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

static void on_set_limit_clicked(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(s_tree));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return;

    gchar *comm = NULL;
    gtk_tree_model_get(model, &iter, COL_COMM, &comm, -1);
    if (!comm) return;

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Set CPU Limit", GTK_WINDOW(s_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_OK", GTK_RESPONSE_OK, "_Cancel", GTK_RESPONSE_CANCEL, NULL);
    
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    char lbl_str[128];
    snprintf(lbl_str, sizeof(lbl_str), "Enter CPU limit %% for '%s' (0 to remove):", comm);
    GtkWidget *lbl = gtk_label_new(lbl_str);
    gtk_box_pack_start(GTK_BOX(content_area), lbl, FALSE, FALSE, 5);
    GtkWidget *entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(content_area), entry, FALSE, FALSE, 5);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const gchar *text = gtk_entry_get_text(GTK_ENTRY(entry));
        int pct = atoi(text);
        if (pct > 0) limit_add(comm, pct);
        else limit_remove(comm);
    }
    gtk_widget_destroy(dialog);
    g_free(comm);

    gui_refresh_store();
    gui_refresh_limits_store();
}

static void on_lim_remove_clicked(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(s_lim_tree));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return;

    gchar *comm = NULL;
    gtk_tree_model_get(model, &iter, COL_LIM_COMM, &comm, -1);
    if (!comm) return;

    limit_remove(comm);
    g_free(comm);

    gui_refresh_store();
    gui_refresh_limits_store();
}

static gboolean on_timer(gpointer data)
{
    (void)data;
    if (!g_running) { gtk_main_quit(); return FALSE; }
    gui_refresh_store();
    gui_refresh_exempt_store();
    gui_refresh_limits_store();
    gui_refresh_pred_store();
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
    
    GtkWidget *limit_btn = gtk_button_new_with_label("Set CPU Limit");
    g_signal_connect(limit_btn, "clicked", G_CALLBACK(on_set_limit_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(btn_box), limit_btn, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(proc_vbox), btn_box, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(proc_vbox), scroll, TRUE, TRUE, 0);

    s_store = gtk_list_store_new(NUM_COLS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    s_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s_store));
    g_object_unref(s_store);

    const char *titles[] = {"PID", "Process", "Status", "Unfocused", "CPU%", "Limit%", "Audio", "GPU", "Exempt"};
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

    /* TAB 3: Limits */
    GtkWidget *lim_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(lim_vbox), 4);
    
    GtkWidget *lim_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *lim_rem_btn = gtk_button_new_with_label("Remove Selected Limit");
    g_signal_connect(lim_rem_btn, "clicked", G_CALLBACK(on_lim_remove_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(lim_btn_box), lim_rem_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lim_vbox), lim_btn_box, FALSE, FALSE, 0);

    GtkWidget *lim_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lim_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(lim_vbox), lim_scroll, TRUE, TRUE, 0);

    s_lim_store = gtk_list_store_new(NUM_LIM_COLS, G_TYPE_STRING, G_TYPE_STRING);
    s_lim_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s_lim_store));
    g_object_unref(s_lim_store);

    GtkCellRenderer *lim_r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *lim_c1 = gtk_tree_view_column_new_with_attributes("Process Name", lim_r, "text", COL_LIM_COMM, NULL);
    GtkTreeViewColumn *lim_c2 = gtk_tree_view_column_new_with_attributes("Limit %", lim_r, "text", COL_LIM_PCT, NULL);
    gtk_tree_view_column_set_resizable(lim_c1, TRUE);
    gtk_tree_view_column_set_resizable(lim_c2, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(s_lim_tree), lim_c1);
    gtk_tree_view_append_column(GTK_TREE_VIEW(s_lim_tree), lim_c2);
    
    gtk_container_add(GTK_CONTAINER(lim_scroll), s_lim_tree);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), lim_vbox, gtk_label_new("CPU Limits"));

    /* TAB 4: Window Predictor (RL) */
    GtkWidget *pred_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(pred_vbox), 6);

    s_rl_label = gtk_label_new("Reinforcement Learning Online Status: Initializing...");
    gtk_label_set_xalign(GTK_LABEL(s_rl_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(pred_vbox), s_rl_label, FALSE, FALSE, 2);

    GtkWidget *pred_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pred_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(pred_vbox), pred_scroll, TRUE, TRUE, 0);

    s_pred_store = gtk_list_store_new(NUM_PRED_COLS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    s_pred_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s_pred_store));
    g_object_unref(s_pred_store);

    GtkCellRenderer *pred_r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *pred_c0 = gtk_tree_view_column_new_with_attributes("Rank", pred_r, "text", COL_PRED_RANK, NULL);
    GtkTreeViewColumn *pred_c1 = gtk_tree_view_column_new_with_attributes("Application", pred_r, "text", COL_PRED_COMM, NULL);
    GtkTreeViewColumn *pred_c2 = gtk_tree_view_column_new_with_attributes("Probability", pred_r, "text", COL_PRED_PROB, NULL);
    GtkTreeViewColumn *pred_c3 = gtk_tree_view_column_new_with_attributes("Pre-warm Action", pred_r, "text", COL_PRED_STATUS, NULL);
    gtk_tree_view_column_set_resizable(pred_c0, TRUE);
    gtk_tree_view_column_set_resizable(pred_c1, TRUE);
    gtk_tree_view_column_set_resizable(pred_c2, TRUE);
    gtk_tree_view_column_set_resizable(pred_c3, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(s_pred_tree), pred_c0);
    gtk_tree_view_append_column(GTK_TREE_VIEW(s_pred_tree), pred_c1);
    gtk_tree_view_append_column(GTK_TREE_VIEW(s_pred_tree), pred_c2);
    gtk_tree_view_append_column(GTK_TREE_VIEW(s_pred_tree), pred_c3);

    gtk_container_add(GTK_CONTAINER(pred_scroll), s_pred_tree);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), pred_vbox, gtk_label_new("Window Predictor (RL)"));

    /* Status Bar */
    s_statusbar = gtk_statusbar_new();
    gtk_box_pack_start(GTK_BOX(vbox), s_statusbar, FALSE, FALSE, 0);

    gtk_widget_show_all(s_window);
    g_timeout_add(500, on_timer, NULL);
    gui_refresh_store();
    gui_refresh_exempt_store();
    gui_refresh_limits_store();
    gui_refresh_pred_store();
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

        char tickle_status[32];
        const char *status;
        if (p->pid == g_focused_pid && g_dynboost.rapidboost_active) status = "RapidBoost";
        else if (p->pid == g_focused_pid)       status = "Focused";
        else if (is_comm_exempt(p->comm, focused_comm)) status = "Exempt";
        else if (p->audio_playing)         status = "Audio Exempt";
        else if (p->prewarm)               status = "Pre-warm";
        else if (p->throttled) {
            snprintf(tickle_status, sizeof(tickle_status), "Tickle (%.1fs)", 
                     (float)throttle_get_tick(p) / 1000.0f);
            status = tickle_status;
        }
        else                               status = "Running";

        char dur[32];
        if (p->defocus_time > 0) fmt_dur(now - p->defocus_time, dur, sizeof(dur));
        else snprintf(dur, sizeof(dur), "-");

        char cpu_str[16];
        if (p->cpu_pct > 0.1) snprintf(cpu_str, sizeof(cpu_str), "%.1f%%", p->cpu_pct);
        else snprintf(cpu_str, sizeof(cpu_str), "-");

        const char *audio = p->audio_playing ? "Playing" : "-";

        const char *gpu = "-";
        for (int g = 0; g < g_ngpu_procs; g++) {
            if (g_gpu_procs[g].pid == p->pid) { gpu = "Heavy"; break; }
        }

        const char *ex = exempt_check(p->comm) ? "Yes" : "-";
        
        char lim_str[16];
        int pct = limit_check(p->comm);
        if (pct > 0) snprintf(lim_str, sizeof(lim_str), "%d%%", pct);
        else snprintf(lim_str, sizeof(lim_str), "-");

        gtk_list_store_set(s_store, &iter,
            COL_PID,       (int)p->pid,
            COL_COMM,      p->comm,
            COL_STATUS,    status,
            COL_UNFOCUSED, dur,
            COL_CPU,       cpu_str,
            COL_LIMIT,     lim_str,
            COL_AUDIO,     audio,
            COL_GPU,       gpu,
            COL_EXEMPT,    ex, -1);
            
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(s_store), &iter);
    }
    while (valid) valid = gtk_list_store_remove(s_store, &iter);

    char sb[512];
    const char *mon_str = g_monitor_only ? "  |  [Daemon Connected: Monitor]" : "";
    const char *rb_str = g_dynboost.rapidboost_active 
        ? (g_dynboost.rapidboost_on_ac ? "  |  [RapidBoost ACTIVE: 2.6 GHz]" : "  |  [RapidBoost ACTIVE: 1.15 GHz]") 
        : "";
    const char *bml_str = "";
    if (g_experimental && g_dynboost.boostml_collecting)
        bml_str = "  |  BoostML: Collecting";

    char pred_str[128] = "";
    if (g_experimental && g_nn_state.count > 0) {
        snprintf(pred_str, sizeof(pred_str), "  |  Next: %s (%.0f%%)",
                 g_nn_state.top[0].comm, g_nn_state.top[0].prob * 100.0f);
    }

    if (g_pairing_mode) {
        snprintf(sb, sizeof(sb), "Mode: %s  |  Tracked: %d  |  Pairs: %d  |  GPU procs: %d%s%s%s%s",
                 mode_name(g_mode), g_nprocs, g_npairs, g_ngpu_procs, mon_str, rb_str, bml_str, pred_str);
    } else {
        snprintf(sb, sizeof(sb), "Mode: %s  |  Tracked: %d  |  Pairs: OFF  |  GPU procs: %d%s%s%s%s",
                 mode_name(g_mode), g_nprocs, g_ngpu_procs, mon_str, rb_str, bml_str, pred_str);
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

static void gui_refresh_limits_store(void)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s_lim_store), &iter);

    for (int i = 0; i < g_nlimits; i++) {
        if (!valid) gtk_list_store_append(s_lim_store, &iter);
        char pct_str[32];
        snprintf(pct_str, sizeof(pct_str), "%d%%", g_limits[i].limit_pct);
        gtk_list_store_set(s_lim_store, &iter, COL_LIM_COMM, g_limits[i].comm, COL_LIM_PCT, pct_str, -1);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(s_lim_store), &iter);
    }
    while (valid) valid = gtk_list_store_remove(s_lim_store, &iter);
}

static void gui_refresh_pred_store(void)
{
    if (!s_pred_store) return;
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s_pred_store), &iter);

    for (int i = 0; i < g_nn_state.count; i++) {
        if (!valid) gtk_list_store_append(s_pred_store, &iter);
        char prob_str[32];
        snprintf(prob_str, sizeof(prob_str), "%.1f%%", g_nn_state.top[i].prob * 100.0f);
        int is_pred = (strcmp(g_nn_state.top[i].comm, g_nn_state.last_predicted) == 0);
        const char *status_str = is_pred ? "Pre-warmed (100ms tick)" : "Standby";

        gtk_list_store_set(s_pred_store, &iter,
            COL_PRED_RANK,   i + 1,
            COL_PRED_COMM,   g_nn_state.top[i].comm,
            COL_PRED_PROB,   prob_str,
            COL_PRED_STATUS, status_str,
            -1);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(s_pred_store), &iter);
    }
    while (valid) valid = gtk_list_store_remove(s_pred_store, &iter);

    if (s_rl_label) {
        char rl_buf[512];
        if (g_nn_state.total_predictions > 0) {
            float acc = (float)g_nn_state.total_correct / (float)g_nn_state.total_predictions * 100.0f;
            const char *rw_str = (g_nn_state.last_reward > 0)
                ? "<span color='#48b9c7'><b>Reward (+1.0 Correct)</b></span>"
                : "<span color='orange'><b>Penalty (-1.0 Corrected via SGD)</b></span>";
            snprintf(rl_buf, sizeof(rl_buf),
                     "<b>RL Feedback:</b> %s  |  <b>Target:</b> %s (Pred: %s)  |  <b>Loss:</b> %.4f  |  <b>Online Acc:</b> %.1f%% (%d/%d)",
                     rw_str, g_nn_state.last_actual[0] ? g_nn_state.last_actual : "-",
                     g_nn_state.last_predicted[0] ? g_nn_state.last_predicted : "-",
                     g_nn_state.last_loss, acc,
                     g_nn_state.total_correct, g_nn_state.total_predictions);
        } else {
            snprintf(rl_buf, sizeof(rl_buf),
                     "<b>Neural Predictor:</b> Initializing GPU inference model and collecting transitions...");
        }
        gtk_label_set_markup(GTK_LABEL(s_rl_label), rl_buf);
    }
}

void gui_run(void) { gtk_main(); }
void gui_shutdown(void) { if (s_window) { gtk_widget_destroy(s_window); s_window = NULL; } }

#else
#include <stdio.h>
void gui_init(int *argc, char ***argv) { (void)argc; (void)argv; fprintf(stderr, "[zrn] GUI not available\n"); }
void gui_run(void) { }
void gui_shutdown(void) { }
#endif
