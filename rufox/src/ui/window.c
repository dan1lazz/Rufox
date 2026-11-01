/*
 * rufox — main window
 *
 * The layout deliberately mirrors Rufus' single dialog, because that layout is
 * the reason people like it: one column, top to bottom, in the order you
 * actually make the decisions — device, image, scheme/target, file system,
 * then START. Rufus' "Advanced device/format properties" toggles become
 * AdwExpanderRows, and the log pane becomes an AdwExpanderRow too.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/rufox-gui.h"

#include <string.h>
#include <stdio.h>

struct _RufoxWindow {
	AdwApplicationWindow parent;

	RufoxEngine *engine;
	RfxOptions   opt;
	RfxImgReport img;
	gboolean     img_valid;
	char        *image_path;

	/* Drive properties */
	GtkWidget *device_combo;
	GtkWidget *boot_combo;
	GtkWidget *select_button;
	GtkWidget *image_option_row;
	GtkWidget *image_option_combo;

	/* Format options */
	GtkWidget *scheme_combo;
	GtkWidget *target_combo;
	GtkWidget *fs_combo;
	GtkWidget *cluster_combo;
	GtkWidget *label_entry;

	/* Advanced */
	GtkWidget *quick_check;
	GtkWidget *ext_label_check;
	GtkWidget *oldbios_check;
	GtkWidget *uefi_valid_check;
	GtkWidget *badblocks_combo;
	GtkWidget *passes_spin;
	GtkWidget *persistence_row;
	GtkWidget *persistence_scale;
	GtkWidget *persistence_size_label;
	GtkWidget *win11_check;
	GtkWidget *account_check;
	GtkWidget *edge_check;

	/* Status */
	GtkWidget *banner;		/* GtkRevealer, AdwBanner needs libadwaita 1.3 */
	GtkWidget *banner_label;
	gboolean   banner_is_warning;
	GtkWidget *progress_bar;
	GtkWidget *status_label;
	GtkWidget *speed_eta_label;
	GtkWidget *start_button;
	GtkWidget *close_button;
	GtkWidget *log_view;
	GtkWidget *log_scroller;
	GtkWidget *log_expander;

	guint      hotplug_id;
	guint      status_timeout_id;
	gboolean   cancellable;
};

G_DEFINE_TYPE(RufoxWindow, rufox_window, ADW_TYPE_APPLICATION_WINDOW)

/* ----------------------------------------------------------------------- */
/* Small helpers                                                           */
/* ----------------------------------------------------------------------- */
/* Rufus stores the enum in ComboBox_SetItemData(). GTK's ComboBoxText has no
 * per-row user data, so we keep a parallel GArray on the widget. */
static void combo_set_values(GtkWidget *combo, const int *values, int n)
{
	GArray *a = g_array_new(FALSE, FALSE, sizeof(int));
	g_array_append_vals(a, values, n);
	g_object_set_data_full(G_OBJECT(combo), "rufox-values", a,
	                       (GDestroyNotify)g_array_unref);
}

static int combo_get_value(GtkWidget *combo)
{
	int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
	GArray *a = g_object_get_data(G_OBJECT(combo), "rufox-values");
	if (a == NULL || idx < 0 || idx >= (int)a->len) return -1;
	return g_array_index(a, int, idx);
}

static void combo_set_value(GtkWidget *combo, int value)
{
	GArray *a = g_object_get_data(G_OBJECT(combo), "rufox-values");
	if (a == NULL) return;
	for (guint i = 0; i < a->len; i++) {
		if (g_array_index(a, int, i) == value) {
			gtk_combo_box_set_active(GTK_COMBO_BOX(combo), (int)i);
			return;
		}
	}
}

static void combo_reset(GtkWidget *combo)
{
	gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
}

/* libadwaita 1.2 has no adw_preferences_row_set_subtitle(); AdwActionRow's
 * own subtitle property covers the same ground. */
static GtkWidget* row_with(const char *title, const char *subtitle, GtkWidget *child)
{
	AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	if (subtitle != NULL)
		adw_action_row_set_subtitle(row, subtitle);
	adw_action_row_add_suffix(row, child);
	adw_action_row_set_activatable_widget(row, child);
	gtk_widget_set_valign(child, GTK_ALIGN_CENTER);
	return GTK_WIDGET(row);
}

static void show_banner(RufoxWindow *self, const char *text, gboolean warning)
{
	gtk_label_set_text(GTK_LABEL(self->banner_label), text);
	if (warning) {
		gtk_widget_remove_css_class(self->banner_label, "success");
		gtk_widget_add_css_class(self->banner_label, "warning");
	} else {
		gtk_widget_remove_css_class(self->banner_label, "warning");
		gtk_widget_add_css_class(self->banner_label, "success");
	}
	gtk_revealer_set_reveal_child(GTK_REVEALER(self->banner), TRUE);
}

static void hide_banner(RufoxWindow *self)
{
	gtk_revealer_set_reveal_child(GTK_REVEALER(self->banner), FALSE);
}

/* ----------------------------------------------------------------------- */
/* Status line                                                             */
/* ----------------------------------------------------------------------- */
static void set_status(RufoxWindow *self, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_status(RufoxWindow *self, const char *fmt, ...)
{
	va_list ap;
	g_autofree char *s = NULL;
	va_start(ap, fmt);
	s = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	gtk_label_set_text(GTK_LABEL(self->status_label), s);
}

static void set_speed_eta(RufoxWindow *self, double bps, double eta)
{
	if (bps <= 0 && eta < 0) {
		gtk_label_set_text(GTK_LABEL(self->speed_eta_label), "");
		return;
	}
	char rate[32], t[32];
	rfx_human_rate(bps, rate, sizeof(rate));
	rfx_human_eta(eta, t, sizeof(t));
	g_autofree char *s = g_strdup_printf("%s    ETA %s", rate, t);
	gtk_label_set_text(GTK_LABEL(self->speed_eta_label), s);
}

static void set_busy(RufoxWindow *self, gboolean busy)
{
	gtk_widget_set_sensitive(self->device_combo, !busy);
	gtk_widget_set_sensitive(self->boot_combo, !busy);
	gtk_widget_set_sensitive(self->select_button, !busy);
	gtk_widget_set_sensitive(self->scheme_combo, !busy);
	gtk_widget_set_sensitive(self->target_combo, !busy);
	gtk_widget_set_sensitive(self->fs_combo, !busy);
	gtk_widget_set_sensitive(self->cluster_combo, !busy);
	gtk_widget_set_sensitive(self->label_entry, !busy);
	gtk_widget_set_sensitive(self->persistence_scale, !busy);
	gtk_widget_set_sensitive(self->badblocks_combo, !busy);

	self->cancellable = busy;
	gtk_button_set_label(GTK_BUTTON(self->start_button), busy ? "CANCEL" : "START");
	gtk_widget_set_visible(self->close_button, !busy);
	gtk_widget_set_sensitive(self->start_button,
	                         busy || (self->opt.write_mode == RFX_WRITE_AS_ISO ? TRUE : TRUE));

	if (!busy)
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), 0.0);
}

/* ----------------------------------------------------------------------- */
/* Device list (Rufus: the device combo + RefreshTimer)                    */
/* ----------------------------------------------------------------------- */
static void repopulate_devices(RufoxWindow *self)
{
	combo_reset(self->device_combo);
	int n = rufox_engine_device_count(self->engine);
	int selected = -1;

	for (int i = 0; i < n; i++) {
		const RfxDrive *d = rufox_engine_get_device(self->engine, i);
		if (d == NULL) continue;
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->device_combo), NULL,
		                          d->FriendlyName);
		if (selected < 0 && d->IsRemovable)
			selected = i;
	}
	if (n > 0)
		gtk_combo_box_set_active(GTK_COMBO_BOX(self->device_combo),
		                         selected >= 0 ? selected : 0);

	if (n == 0) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->device_combo), NULL,
		                          "No removable device found");
		gtk_combo_box_set_active(GTK_COMBO_BOX(self->device_combo), 0);
		set_status(self, "Insert a USB stick");
	} else {
		const RfxDrive *d = rufox_engine_get_device(self->engine,
		                          gtk_combo_box_get_active(GTK_COMBO_BOX(self->device_combo)));
		if (d != NULL) {
			char sz[32];
			rfx_human_size(d->DiskSize, sz, sizeof(sz));
			set_status(self, "%s — %s, %s, %u-byte sectors",
			           d->DevicePath, sz, d->BusName, d->SectorSize);
		}
	}
	gtk_widget_set_sensitive(self->start_button, n > 0);
}

static void on_device_changed(RufoxWindow *self)
{
	int i = gtk_combo_box_get_active(GTK_COMBO_BOX(self->device_combo));
	const RfxDrive *d = rufox_engine_get_device(self->engine, i);
	if (d == NULL) return;
	rufox_engine_set_device(self->engine, d->DevicePath);

	char sz[32];
	rfx_human_size(d->DiskSize, sz, sizeof(sz));
	set_status(self, "%s — %s, %s, %u-byte sectors%s", d->DevicePath, sz, d->BusName,
	           d->SectorSize, d->MountPoint[0] ? " (mounted)" : "");

	/* Rufus re-derives the allowed file systems and cluster sizes whenever
	 * the device or the image changes. */
	gboolean en = rufox_engine_is_running(self->engine) ? FALSE : TRUE;
	gtk_widget_set_sensitive(self->fs_combo, en);
	gtk_widget_set_sensitive(self->cluster_combo, en);

	/* Persistence is only meaningful on a device big enough to spare room. */
	gtk_widget_set_sensitive(self->persistence_scale,
	                         en && self->img_valid && self->img.uses_casper &&
	                         d->DiskSize > 2ULL * RFX_GB);
}

static gboolean hotplug_poll(gpointer ud)
{
	RufoxWindow *self = RUFOX_WINDOW(ud);
	if (rufox_engine_is_running(self->engine))
		return G_SOURCE_CONTINUE;

	int before = rufox_engine_device_count(self->engine);
	rufox_engine_refresh_devices(self->engine);
	int after = rufox_engine_device_count(self->engine);
	if (before != after) {
		repopulate_devices(self);
		if (after > before)
			set_status(self, "A new device appeared");
	}
	return G_SOURCE_CONTINUE;
}

/* ----------------------------------------------------------------------- */
/* Boot selection & image option (Rufus: SetBootOptions / UpdateImage)     */
/* ----------------------------------------------------------------------- */
static void repopulate_boot(RufoxWindow *self)
{
	int keep = combo_get_value(self->boot_combo);
	combo_reset(self->boot_combo);

	int values[8], k = 0;
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->boot_combo), NULL,
	                          "Non bootable");
	values[k++] = RFX_BT_NON_BOOTABLE;

	g_autofree char *img_label = self->image_path != NULL
	                             ? g_path_get_basename(self->image_path)
	                             : g_strdup("Disk or ISO image...");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->boot_combo), NULL, img_label);
	values[k++] = RFX_BT_IMAGE;

	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->boot_combo), NULL, "FreeDOS");
	values[k++] = RFX_BT_FREEDOS;

	/* Advanced entries, shown only when the payload is actually present. */
	if (rfx_boot_find_resource("mbr.bin") != NULL ||
	    rfx_tool_available("syslinux")) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->boot_combo), NULL,
		                          "Syslinux 6.xx");
		values[k++] = RFX_BT_SYSLINUX_V6;
	}
	if (rfx_tool_available("grub-install")) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->boot_combo), NULL,
		                          "GRUB2");
		values[k++] = RFX_BT_GRUB2;
	}
	if (rfx_boot_find_resource("uefi-ntfs.img") != NULL) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->boot_combo), NULL,
		                          "UEFI:NTFS");
		values[k++] = RFX_BT_UEFI_NTFS;
	}

	combo_set_values(self->boot_combo, values, k);

	if (keep >= 0)
		combo_set_value(self->boot_combo, keep);
	if (gtk_combo_box_get_active(GTK_COMBO_BOX(self->boot_combo)) < 0)
		combo_set_value(self->boot_combo,
		                self->image_path != NULL ? RFX_BT_IMAGE : RFX_BT_NON_BOOTABLE);
}

static void update_image_option(RufoxWindow *self)
{
	/* Rufus only shows the "Image option" dropdown for Windows installers
	 * that can actually be used for Windows To Go. */
	gboolean show = self->img_valid && rfx_wintogo_supported(&self->img);
	gtk_widget_set_visible(self->image_option_row, show);
	gtk_widget_set_sensitive(self->image_option_row, show);
	if (!show)
		combo_set_value(self->image_option_combo, RFX_IMOP_WIN_STANDARD);
}

/* ----------------------------------------------------------------------- */
/* File system / cluster (Rufus: SetAllowedFileSystems / SetClusterSizes)  */
/* ----------------------------------------------------------------------- */
static void repopulate_fs(RufoxWindow *self)
{
	int keep = combo_get_value(self->fs_combo);
	combo_reset(self->fs_combo);

	RfxFs allowed[RFX_FS_MAX];
	int n = 0;
	int boot = combo_get_value(self->boot_combo);
	if (boot < 0) boot = RFX_BT_IMAGE;
	rfx_format_allowed_fs(boot, self->opt.target_type,
	                      self->img_valid ? &self->img : NULL, allowed, &n);

	int values[RFX_FS_MAX];
	for (int i = 0; i < n; i++) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->fs_combo), NULL,
		                          rfx_format_fs_name(allowed[i]));
		values[i] = allowed[i];
	}
	combo_set_values(self->fs_combo, values, n);

	if (keep >= 0) combo_set_value(self->fs_combo, keep);
	if (gtk_combo_box_get_active(GTK_COMBO_BOX(self->fs_combo)) < 0) {
		/* Prefer FAT32, then whatever is first — same as Rufus. */
		combo_set_value(self->fs_combo, RFX_FS_FAT32);
		if (gtk_combo_box_get_active(GTK_COMBO_BOX(self->fs_combo)) < 0 && n > 0)
			gtk_combo_box_set_active(GTK_COMBO_BOX(self->fs_combo), 0);
	}
	gtk_widget_set_sensitive(self->fs_combo, n > 0);
}

static void repopulate_cluster(RufoxWindow *self)
{
	int keep = combo_get_value(self->cluster_combo);
	combo_reset(self->cluster_combo);

	RfxFs fs = (RfxFs)combo_get_value(self->fs_combo);
	if (fs < 0) return;

	const RfxDrive *d = rufox_engine_get_device(
		self->engine, gtk_combo_box_get_active(GTK_COMBO_BOX(self->device_combo)));
	uint64_t part_size = d != NULL ? d->DiskSize : 8ULL * RFX_GB;
	uint32_t def = rfx_format_default_cluster_size(fs, part_size);

	/* Rufus walks 0x100 .. 0x10000000 and offers every allowed size. */
	int values[24], k = 0;
	for (uint32_t c = 512; c <= 32u * 1024 * 1024 && k < 24; c <<= 1) {
		if (!rfx_format_cluster_allowed(fs, c, part_size)) continue;
		char lbl[32];
		if (c < 1024) g_snprintf(lbl, sizeof(lbl), "%u bytes", c);
		else if (c < 1024 * 1024) g_snprintf(lbl, sizeof(lbl), "%u kilobytes", c / 1024);
		else g_snprintf(lbl, sizeof(lbl), "%u megabytes", c / (1024 * 1024));
		if (c == def) g_strlcat(lbl, " (Default)", sizeof(lbl));
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->cluster_combo), NULL, lbl);
		values[k++] = (int)c;
	}
	combo_set_values(self->cluster_combo, values, k);

	if (keep >= 0) combo_set_value(self->cluster_combo, keep);
	if (gtk_combo_box_get_active(GTK_COMBO_BOX(self->cluster_combo)) < 0)
		combo_set_value(self->cluster_combo, (int)def);
	gtk_widget_set_sensitive(self->cluster_combo, k > 0);
}

/* ----------------------------------------------------------------------- */
/* Partition scheme / target system (Rufus: SetPartitionSchemeAndTargetSystem) */
/* ----------------------------------------------------------------------- */
static void on_scheme_changed(RufoxWindow *self)
{
	int scheme = combo_get_value(self->scheme_combo);
	if (scheme < 0) return;
	self->opt.partition_scheme = scheme;

	/* Rufus: GPT implies UEFI and locks the target; MBR lets you pick. */
	if (scheme == RFX_PS_GPT) {
		combo_set_value(self->target_combo, RFX_TT_UEFI);
		gtk_widget_set_sensitive(self->target_combo, FALSE);
	} else {
		gtk_widget_set_sensitive(self->target_combo, TRUE);
	}
	repopulate_fs(self);
	repopulate_cluster(self);
}

static void on_target_changed(RufoxWindow *self)
{
	int t = combo_get_value(self->target_combo);
	if (t >= 0) self->opt.target_type = t;
	repopulate_fs(self);
}

/* ----------------------------------------------------------------------- */
/* Persistence slider (Rufus: IDC_PERSISTENCE_SLIDER)                      */
/* ----------------------------------------------------------------------- */
static void on_persistence_changed(RufoxWindow *self)
{
	double gb = gtk_range_get_value(GTK_RANGE(self->persistence_scale));
	self->opt.persistence_size = (uint64_t)(gb * 1024 * 1024 * 1024);

	char buf[32];
	if (self->opt.persistence_size == 0)
		g_snprintf(buf, sizeof(buf), "disabled");
	else
		rfx_human_size(self->opt.persistence_size, buf, sizeof(buf));
	gtk_label_set_text(GTK_LABEL(self->persistence_size_label), buf);
}

/* ----------------------------------------------------------------------- */
/* Collect the dialog state into RfxOptions                                */
/* ----------------------------------------------------------------------- */
static void collect_options(RufoxWindow *self)
{
	self->opt.partition_scheme = combo_get_value(self->scheme_combo);
	self->opt.target_type      = combo_get_value(self->target_combo);
	self->opt.fs_type          = combo_get_value(self->fs_combo);

	int c = combo_get_value(self->cluster_combo);
	self->opt.cluster_size = c > 0 ? (uint32_t)c : 0;

	const char *lbl = gtk_editable_get_text(GTK_EDITABLE(self->label_entry));
	g_strlcpy(self->opt.label, lbl != NULL ? lbl : "RUFOX", sizeof(self->opt.label));

	self->opt.quick_format          = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->quick_check));
	self->opt.create_extended_label = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->ext_label_check));
	self->opt.old_bios_fixes        = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->oldbios_check));
	self->opt.uefi_media_validation = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->uefi_valid_check));
	self->opt.fix_win11_requirements= gtk_check_button_get_active(GTK_CHECK_BUTTON(self->win11_check));
	self->opt.no_online_account     = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->account_check));
	self->opt.remove_edge           = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->edge_check));

	self->opt.boot_type   = combo_get_value(self->boot_combo);
	self->opt.image_option= combo_get_value(self->image_option_combo);
	self->opt.badblocks_mode = combo_get_value(self->badblocks_combo);
	self->opt.badblocks_passes = (int)gtk_spin_button_get_value_as_int(
	                             GTK_SPIN_BUTTON(self->passes_spin));

	/* Rufus decides ISO vs DD from the image and the "Image option" choice;
	 * an image that cannot be extracted is always DD. */
	if (self->img_valid && (self->img.disable_iso || !self->img.is_iso))
		self->opt.write_mode = RFX_WRITE_AS_DD;
	else if (self->opt.boot_type == RFX_BT_NON_BOOTABLE)
		self->opt.write_mode = RFX_WRITE_AS_ISO;
}

/* ----------------------------------------------------------------------- */
/* START / CANCEL                                                          */
/* ----------------------------------------------------------------------- */
static void on_finished(RufoxEngine *engine, int result, const char *message,
                        double seconds, gpointer ud)
{
	RufoxWindow *self = RUFOX_WINDOW(ud);
	(void)engine;
	set_busy(self, FALSE);

	if (result == (int)RFX_RESULT_SUCCESS) {
		set_status(self, "Done in %.1f s — you can now remove the device", seconds);
		show_banner(self, "Write complete. It is now safe to unplug the device.",
		            FALSE);
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), 1.0);
	} else if (result == (int)RFX_RESULT_CANCELLED) {
		set_status(self, "Cancelled");
		show_banner(self, "The operation was cancelled.", TRUE);
	} else {
		set_status(self, "Failed: %s", message);
		g_autofree char *t = g_strdup_printf("Failed: %s", message);
		show_banner(self, t, TRUE);
	}
}

static void on_progress(RufoxEngine *engine, gpointer progress, gpointer ud)
{
	RufoxWindow *self = RUFOX_WINDOW(ud);
	const RfxProgress *p = progress;
	(void)engine;

	double frac = p->progress;
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), frac);

	g_autofree char *txt = g_strdup_printf("%d%%  %s", (int)(frac * 100), p->message);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(self->progress_bar), txt);
	set_speed_eta(self, p->speed_bps, p->eta_seconds);
	set_status(self, "%s", p->message);
}

static void on_log(RufoxEngine *engine, int level, const char *line, gpointer ud)
{
	RufoxWindow *self = RUFOX_WINDOW(ud);
	(void)engine;

	GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->log_view));
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(buf, &end);

	g_autofree char *stamp = g_date_time_format(g_date_time_new_now_local(), "%H:%M:%S");
	g_autofree char *text = g_strdup_printf("[%s] %s\n", stamp, line);

	GtkTextMark *mark = gtk_text_buffer_create_mark(buf, NULL, &end, FALSE);
	gtk_text_buffer_insert(buf, &end, text, -1);

	/* Colour errors and warnings, the way Rufus' log pane does. */
	if (level >= (int)RFX_LOG_WARN) {
		GtkTextIter start;
		gtk_text_buffer_get_iter_at_mark(buf, &start, mark);
		gtk_text_buffer_apply_tag_by_name(buf, "err", &start, &end);
	}
	gtk_text_mark_set_visible(mark, TRUE);
	gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(self->log_view), mark);
}

static void start_write(RufoxWindow *self);

static void on_confirm_response(AdwMessageDialog *dialog, const char *response,
                                gpointer ud)
{
	RufoxWindow *self = RUFOX_WINDOW(ud);
	gtk_window_destroy(GTK_WINDOW(dialog));
	if (g_strcmp0(response, "ok") == 0)
		start_write(self);
}

/*
 * Rufus shows a modal "you are about to destroy this drive" box. We do the
 * same with AdwMessageDialog, but asynchronously: GTK 4 has no way to spin a
 * nested loop for a response without risking reentrancy, so the confirmation
 * callback is what actually kicks off the write.
 */
static void confirm_destructive(RufoxWindow *self)
{
	const RfxDrive *d = rufox_engine_get_device(
		self->engine, gtk_combo_box_get_active(GTK_COMBO_BOX(self->device_combo)));
	if (d == NULL) return;

	char sz[32];
	rfx_human_size(d->DiskSize, sz, sizeof(sz));
	g_autofree char *body = g_strdup_printf(
		"<b>%s</b>\n%s\n%s\n\n"
		"<span foreground='red'><b>ALL DATA ON THIS DEVICE WILL BE DESTROYED.</b></span>",
		d->FriendlyName, d->DevicePath, sz);

	GtkWidget *dialog = adw_message_dialog_new(GTK_WINDOW(self),
	                                           "Confirm the write operation", body);
	adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dialog), "cancel", "Cancel");
	adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dialog), "ok", "OK");
	adw_message_dialog_set_response_appearance(ADW_MESSAGE_DIALOG(dialog), "ok",
	                                           ADW_RESPONSE_DESTRUCTIVE);
	adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dialog), "cancel");
	adw_message_dialog_set_close_response(ADW_MESSAGE_DIALOG(dialog), "cancel");
	g_signal_connect(dialog, "response", G_CALLBACK(on_confirm_response), self);
	gtk_window_present(GTK_WINDOW(dialog));
}

static void start_write(RufoxWindow *self)
{
	const RfxDrive *d = rufox_engine_get_device(
		self->engine, gtk_combo_box_get_active(GTK_COMBO_BOX(self->device_combo)));
	if (d == NULL) return;

	hide_banner(self);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->log_view)),
	                         "", -1);

	rufox_engine_set_device(self->engine, d->DevicePath);
	rufox_engine_set_image(self->engine, self->image_path);
	rufox_engine_set_options(self->engine, &self->opt);

	/* Rufus auto-expands its log window while writing; do the same. */
	adw_expander_row_set_expanded(ADW_EXPANDER_ROW(self->log_expander), TRUE);

	set_busy(self, TRUE);
	set_status(self, "Starting…");
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), 0.0);
	rufox_engine_start(self->engine);
}

static void on_start_clicked(RufoxWindow *self)
{
	if (self->cancellable) {
		rufox_engine_cancel(self->engine);
		set_status(self, "Cancelling…");
		return;
	}

	int i = gtk_combo_box_get_active(GTK_COMBO_BOX(self->device_combo));
	const RfxDrive *d = rufox_engine_get_device(self->engine, i);
	if (d == NULL) {
		show_banner(self, "No device selected.", TRUE);
		return;
	}

	collect_options(self);
	confirm_destructive(self);
}

/* ----------------------------------------------------------------------- */
/* Image selection (Rufus: the SELECT button + ImageScanThread)            */
/* ----------------------------------------------------------------------- */
static void on_scan_done(GObject *src, GAsyncResult *res, gpointer ud)
{
	RufoxWindow *self = RUFOX_WINDOW(ud);
	GError *err = NULL;
	RfxImgReport rep;
	memset(&rep, 0, sizeof(rep));

	if (!rufox_engine_scan_finish(RUFOX_ENGINE(src), res, &rep, &err)) {
		set_status(self, "Could not read the image: %s",
		           err != NULL ? err->message : rfx_last_error_message());
		show_banner(self, "That file is not a recognised image.", TRUE);
		g_clear_error(&err);
		gtk_widget_set_sensitive(self->select_button, TRUE);
		return;
	}

	self->img = rep;
	self->img_valid = TRUE;
	gtk_widget_set_sensitive(self->select_button, TRUE);

	/* Rufus' SetProposedLabel(): offer the ISO's own volume id. */
	if (rep.usb_label[0] != '\0')
		gtk_editable_set_text(GTK_EDITABLE(self->label_entry), rep.usb_label);

	/* An image that must be DD-written locks the file system choices away. */
	if (rep.disable_iso || !rep.is_iso) {
		self->opt.write_mode = RFX_WRITE_AS_DD;
		show_banner(self, "This image will be written in DD (raw) mode.", FALSE);
	} else {
		self->opt.write_mode = RFX_WRITE_AS_ISO;
		hide_banner(self);
	}

	repopulate_boot(self);
	repopulate_fs(self);
	repopulate_cluster(self);
	update_image_option(self);

	char sz[32], pz[32];
	rfx_human_size(rep.image_size, sz, sizeof(sz));
	rfx_human_size(rep.projected_size, pz, sizeof(pz));
	set_status(self, "%s — %s, projected %s%s%s",
	           rep.label[0] ? rep.label : "(no volume id)", sz, pz,
	           rep.has_efi ? ", UEFI" : "", rep.has_bootmgr ? ", BIOS" : "");

	gtk_widget_set_sensitive(self->persistence_scale,
	                         rep.uses_casper && rep.is_iso);
	if (rep.uses_casper) {
		const RfxDrive *d = rufox_engine_get_device(
			self->engine, gtk_combo_box_get_active(GTK_COMBO_BOX(self->device_combo)));
		if (d != NULL && d->DiskSize > 4ULL * RFX_GB) {
			gtk_range_set_range(GTK_RANGE(self->persistence_scale), 0.0,
			                    (double)(d->DiskSize - 2ULL * RFX_GB) / (1024.0*1024*1024));
		}
	}
}

/* GTK 4.8 has no GtkFileDialog yet (that is 4.10+), so the chooser is a
 * GtkFileChooserNative, which is also what gives us the portal under Flatpak. */
static void on_file_chosen(GtkNativeDialog *native, gint response, gpointer ud)
{
	RufoxWindow *self = RUFOX_WINDOW(ud);
	if (response != GTK_RESPONSE_ACCEPT) {
		gtk_native_dialog_destroy(native);
		return;
	}
	GtkFileChooser *chooser = GTK_FILE_CHOOSER(native);
	g_autoptr(GFile) file = gtk_file_chooser_get_file(chooser);
	gtk_native_dialog_destroy(native);

	if (file == NULL) {
		set_status(self, "Could not read the selected file");
		return;
	}
	g_free(self->image_path);
	self->image_path = g_file_get_path(file);

	gtk_widget_set_sensitive(self->select_button, FALSE);
	set_status(self, "Scanning %s…", self->image_path);
	rufox_engine_scan_image(self->engine, self->image_path, on_scan_done, self);
}

static void on_select_clicked(RufoxWindow *self)
{
	GtkFileChooserNative *native = gtk_file_chooser_native_new(
		"Select an image", GTK_WINDOW(self), GTK_FILE_CHOOSER_ACTION_OPEN,
		"SELECT", "Cancel");

	GtkFileFilter *f = gtk_file_filter_new();
	gtk_file_filter_set_name(f, "Bootable images");
	const char *patterns[] = { "*.iso", "*.img", "*.vhd", "*.vhdx", "*.wim",
	                           "*.zip", "*.xz", "*.gz", "*.bz2", NULL };
	for (int i = 0; patterns[i] != NULL; i++)
		gtk_file_filter_add_pattern(f, patterns[i]);
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(native), f);

	GtkFileFilter *all = gtk_file_filter_new();
	gtk_file_filter_set_name(all, "All files");
	gtk_file_filter_add_pattern(all, "*");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(native), all);

	g_signal_connect(native, "response", G_CALLBACK(on_file_chosen), self);
	gtk_native_dialog_show(GTK_NATIVE_DIALOG(native));
	g_object_unref(f);
	g_object_unref(all);
}

/* ----------------------------------------------------------------------- */
/* Build the UI                                                            */
/* ----------------------------------------------------------------------- */
static GtkWidget* build_drive_group(RufoxWindow *self)
{
	AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
	adw_preferences_group_set_title(g, "Drive Properties");

	self->device_combo = gtk_combo_box_text_new();
	gtk_widget_add_css_class(self->device_combo, "rufox-device");
	adw_preferences_group_add(g, row_with("Device", NULL, self->device_combo));
	g_signal_connect_swapped(self->device_combo, "changed",
	                         G_CALLBACK(on_device_changed), self);

	self->boot_combo = gtk_combo_box_text_new();
	self->select_button = gtk_button_new_with_label("SELECT");
	gtk_widget_add_css_class(self->select_button, "flat");
	GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(hbox), self->boot_combo);
	gtk_box_append(GTK_BOX(hbox), self->select_button);
	gtk_widget_set_hexpand(self->boot_combo, TRUE);
	adw_preferences_group_add(g, row_with("Boot selection", NULL, hbox));
	g_signal_connect_swapped(self->select_button, "clicked",
	                         G_CALLBACK(on_select_clicked), self);
	g_signal_connect_swapped(self->boot_combo, "changed",
	                         G_CALLBACK(repopulate_fs), self);

	self->image_option_combo = gtk_combo_box_text_new();
	int iopt_values[] = { RFX_IMOP_WIN_STANDARD, RFX_IMOP_WIN_EXTENDED, RFX_IMOP_WIN_TO_GO };
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->image_option_combo), NULL,
	                          "Standard Windows Installation");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->image_option_combo), NULL,
	                          "Windows Installation (no TPM, no Secure Boot, no 4GB+ RAM)");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->image_option_combo), NULL,
	                          "Windows To Go");
	combo_set_values(self->image_option_combo, iopt_values, 3);
	gtk_combo_box_set_active(GTK_COMBO_BOX(self->image_option_combo), 0);
	self->image_option_row = row_with("Image option", NULL, self->image_option_combo);
	adw_preferences_group_add(g, self->image_option_row);
	gtk_widget_set_visible(self->image_option_row, FALSE);

	return GTK_WIDGET(g);
}

static GtkWidget* build_format_group(RufoxWindow *self)
{
	AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
	adw_preferences_group_set_title(g, "Format Options");

	self->scheme_combo = gtk_combo_box_text_new();
	int scheme_values[] = { RFX_PS_MBR, RFX_PS_GPT };
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->scheme_combo), NULL, "MBR");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->scheme_combo), NULL, "GPT");
	combo_set_values(self->scheme_combo, scheme_values, 2);
	combo_set_value(self->scheme_combo, RFX_PS_GPT);
	adw_preferences_group_add(g, row_with("Partition scheme", NULL, self->scheme_combo));
	g_signal_connect_swapped(self->scheme_combo, "changed",
	                         G_CALLBACK(on_scheme_changed), self);

	self->target_combo = gtk_combo_box_text_new();
	int target_values[] = { RFX_TT_BIOS, RFX_TT_UEFI };
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->target_combo), NULL,
	                          "BIOS or UEFI-CSM");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->target_combo), NULL,
	                          "UEFI (non CSM)");
	combo_set_values(self->target_combo, target_values, 2);
	combo_set_value(self->target_combo, RFX_TT_UEFI);
	gtk_widget_set_sensitive(self->target_combo, FALSE);	/* GPT implies UEFI */
	adw_preferences_group_add(g, row_with("Target system", NULL, self->target_combo));
	g_signal_connect_swapped(self->target_combo, "changed",
	                         G_CALLBACK(on_target_changed), self);

	self->fs_combo = gtk_combo_box_text_new();
	adw_preferences_group_add(g, row_with("File system", NULL, self->fs_combo));
	g_signal_connect_swapped(self->fs_combo, "changed",
	                         G_CALLBACK(repopulate_cluster), self);

	self->cluster_combo = gtk_combo_box_text_new();
	adw_preferences_group_add(g, row_with("Cluster size", NULL, self->cluster_combo));

	self->label_entry = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(self->label_entry), 32);
	gtk_editable_set_text(GTK_EDITABLE(self->label_entry), "RUFOX");
	adw_preferences_group_add(g, row_with("Volume label", NULL, self->label_entry));

	return GTK_WIDGET(g);
}

static GtkWidget* build_advanced_group(RufoxWindow *self)
{
	AdwExpanderRow *row = ADW_EXPANDER_ROW(adw_expander_row_new());
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Advanced format options");
	adw_expander_row_set_subtitle(row,
	                              "Quick format, bad blocks, persistence, Windows tweaks");
	adw_expander_row_set_expanded(row, FALSE);

	self->quick_check = gtk_check_button_new();
	gtk_check_button_set_active(GTK_CHECK_BUTTON(self->quick_check), TRUE);
	adw_expander_row_add_row(row, row_with("Quick format", NULL, self->quick_check));

	self->ext_label_check = gtk_check_button_new();
	adw_expander_row_add_row(row, row_with("Create extended label and icon files",
	                                       "autorun.inf + drive.ico", self->ext_label_check));

	self->oldbios_check = gtk_check_button_new();
	adw_expander_row_add_row(row, row_with("Fixes for old BIOSes",
	                                       "extra CHS-aligned gap + a compatibility partition",
	                                       self->oldbios_check));

	self->uefi_valid_check = gtk_check_button_new();
	adw_expander_row_add_row(row, row_with("Enable runtime UEFI media validation", NULL,
	                                       self->uefi_valid_check));

	self->badblocks_combo = gtk_combo_box_text_new();
	int bb_values[] = { RFX_BB_DISABLED, RFX_BB_QUICK, RFX_BB_SLOW, RFX_BB_NONDESTRUCTIVE };
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->badblocks_combo), NULL, "None");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->badblocks_combo), NULL,
	                          "Quick (1 pass of 0x00)");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->badblocks_combo), NULL,
	                          "Slow (4 passes)");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->badblocks_combo), NULL,
	                          "Non-destructive (read-only)");
	combo_set_values(self->badblocks_combo, bb_values, 4);
	gtk_combo_box_set_active(GTK_COMBO_BOX(self->badblocks_combo), 0);
	adw_expander_row_add_row(row, row_with("Check device for bad blocks", NULL,
	                                       self->badblocks_combo));

	self->passes_spin = gtk_spin_button_new_with_range(1, 4, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->passes_spin), 1);
	adw_expander_row_add_row(row, row_with("Number of passes", NULL, self->passes_spin));

	/* Persistence: Rufus shows a slider sized against the device. */
	self->persistence_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
	                                                   0.0, 8.0, 0.5);
	gtk_widget_set_size_request(self->persistence_scale, 220, -1);
	gtk_scale_set_draw_value(GTK_SCALE(self->persistence_scale), FALSE);
	gtk_widget_set_sensitive(self->persistence_scale, FALSE);
	self->persistence_size_label = gtk_label_new("disabled");
	gtk_widget_add_css_class(self->persistence_size_label, "dim-label");
	GtkWidget *ph = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_append(GTK_BOX(ph), self->persistence_scale);
	gtk_box_append(GTK_BOX(ph), self->persistence_size_label);
	self->persistence_row = row_with("Persistent partition size",
	                                 "Ubuntu/Debian live media only", ph);
	adw_expander_row_add_row(row, self->persistence_row);
	g_signal_connect_swapped(self->persistence_scale, "value-changed",
	                         G_CALLBACK(on_persistence_changed), self);

	self->win11_check = gtk_check_button_new();
	gtk_check_button_set_active(GTK_CHECK_BUTTON(self->win11_check), TRUE);
	adw_expander_row_add_row(row, row_with("Remove requirement for 4GB+ RAM, Secure Boot and TPM 2.0",
	                                       NULL, self->win11_check));
	self->account_check = gtk_check_button_new();
	adw_expander_row_add_row(row, row_with("Disable data collection (Skip privacy question)",
	                                       NULL, self->account_check));
	self->edge_check = gtk_check_button_new();
	adw_expander_row_add_row(row, row_with("Remove Microsoft Edge from Windows 11", NULL,
	                                       self->edge_check));

	return GTK_WIDGET(row);
}

static GtkWidget* build_log_group(RufoxWindow *self)
{
	GtkWidget *exp = gtk_expander_new("Log");
	gtk_expander_set_expanded(GTK_EXPANDER(exp), FALSE);
	self->log_expander = exp;

	GtkTextBuffer *buf = gtk_text_buffer_new(NULL);
	gtk_text_buffer_create_tag(buf, "err", "foreground", "#c01c28", "weight", 700, NULL);
	gtk_text_buffer_create_tag(buf, "mono", "family", "Monospace", NULL);

	self->log_view = gtk_text_view_new_with_buffer(buf);
	gtk_text_view_set_editable(GTK_TEXT_VIEW(self->log_view), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(self->log_view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->log_view), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(self->log_view), GTK_WRAP_CHAR);
	gtk_widget_set_vexpand(self->log_view, TRUE);
	gtk_widget_set_size_request(self->log_view, -1, 180);

	self->log_scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->log_scroller), self->log_view);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->log_scroller),
	                               GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(self->log_scroller, TRUE);
	gtk_expander_set_child(GTK_EXPANDER(exp), self->log_scroller);
	return exp;
}

static GtkWidget* build_status_group(RufoxWindow *self)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_top(box, 6);

	self->status_label = gtk_label_new("READY");
	gtk_label_set_xalign(GTK_LABEL(self->status_label), 0.0f);
	gtk_widget_add_css_class(self->status_label, "heading");
	gtk_box_append(GTK_BOX(box), self->status_label);

	self->progress_bar = gtk_progress_bar_new();
	gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(self->progress_bar), TRUE);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(self->progress_bar), "");
	gtk_box_append(GTK_BOX(box), self->progress_bar);

	self->speed_eta_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(self->speed_eta_label), 0.0f);
	gtk_widget_add_css_class(self->speed_eta_label, "dim-label");
	gtk_box_append(GTK_BOX(box), self->speed_eta_label);

	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	self->close_button = gtk_button_new_with_label("CLOSE");
	gtk_widget_add_css_class(self->close_button, "flat");
	self->start_button = gtk_button_new_with_label("START");
	gtk_widget_add_css_class(self->start_button, "suggested-action");
	gtk_widget_add_css_class(self->start_button, "pill");
	gtk_box_append(GTK_BOX(buttons), self->close_button);
	gtk_box_append(GTK_BOX(buttons), self->start_button);
	gtk_box_append(GTK_BOX(box), buttons);

	g_signal_connect_swapped(self->start_button, "clicked", G_CALLBACK(on_start_clicked), self);
	g_signal_connect_swapped(self->close_button, "clicked", G_CALLBACK(gtk_window_destroy), self);
	return box;
}

static void rufox_window_construct(RufoxWindow *self)
{
	gtk_window_set_title(GTK_WINDOW(self), "rufox — Rufus for Linux");
	gtk_window_set_default_size(GTK_WINDOW(self), 560, 720);

	/* AdwApplicationWindow owns its titlebar and aborts if you try to set
	 * one, so the header bar is packed as the first child of the content. */
	GtkWidget *toolbar = adw_header_bar_new();
	GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
	gtk_widget_set_tooltip_text(refresh, "Rescan for devices");
	adw_header_bar_pack_start(ADW_HEADER_BAR(toolbar), refresh);
	g_signal_connect_swapped(refresh, "clicked", G_CALLBACK(repopulate_devices), self);

	GtkWidget *menu_btn = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
	GMenu *menu = g_menu_new();
	g_menu_append(menu, "About rufox", "app.about");
	g_menu_append(menu, "Rufus upstream", "app.upstream");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(menu));
	adw_header_bar_pack_end(ADW_HEADER_BAR(toolbar), menu_btn);
	g_object_unref(menu);

	GtkWidget *title = adw_window_title_new("rufox", "Rufus for Linux");
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(toolbar), title);

	GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), root);
	gtk_box_append(GTK_BOX(root), toolbar);

	self->banner = gtk_revealer_new();
	gtk_revealer_set_reveal_child(GTK_REVEALER(self->banner), FALSE);
	self->banner_label = gtk_label_new("Ready");
	gtk_label_set_wrap(GTK_LABEL(self->banner_label), TRUE);
	gtk_widget_set_margin_top(self->banner_label, 8);
	gtk_widget_set_margin_bottom(self->banner_label, 8);
	gtk_widget_set_margin_start(self->banner_label, 12);
	gtk_widget_set_margin_end(self->banner_label, 12);
	gtk_widget_add_css_class(self->banner_label, "warning");
	gtk_revealer_set_child(GTK_REVEALER(self->banner), self->banner_label);
	gtk_box_append(GTK_BOX(root), self->banner);

	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_widget_set_vexpand(scroller, TRUE);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_box_append(GTK_BOX(root), scroller);

	GtkWidget *page = adw_clamp_new();
	adw_clamp_set_maximum_size(ADW_CLAMP(page), 640);
	gtk_widget_set_vexpand(page, TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), page);

	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_top(col, 12);
	gtk_widget_set_margin_bottom(col, 12);
	gtk_widget_set_margin_start(col, 6);
	gtk_widget_set_margin_end(col, 6);
	adw_clamp_set_child(ADW_CLAMP(page), col);

	gtk_box_append(GTK_BOX(col), build_drive_group(self));
	gtk_box_append(GTK_BOX(col), build_format_group(self));
	gtk_box_append(GTK_BOX(col), build_advanced_group(self));
	gtk_box_append(GTK_BOX(col), build_log_group(self));
	gtk_box_append(GTK_BOX(col), build_status_group(self));

	repopulate_devices(self);
	repopulate_boot(self);
	repopulate_fs(self);
	repopulate_cluster(self);
	update_image_option(self);
	set_busy(self, FALSE);

	/* Rufus polls with a RefreshTimer and also listens for WM_DEVICECHANGE.
	 * We use GUdev when it is available and poll otherwise. */
	self->hotplug_id = g_timeout_add_seconds(2, hotplug_poll, self);
}

static void rufox_window_dispose(GObject *obj)
{
	RufoxWindow *self = RUFOX_WINDOW(obj);
	if (self->hotplug_id != 0) {
		g_source_remove(self->hotplug_id);
		self->hotplug_id = 0;
	}
	g_clear_object(&self->engine);
	g_clear_pointer(&self->image_path, g_free);
	G_OBJECT_CLASS(rufox_window_parent_class)->dispose(obj);
}

static void rufox_window_class_init(RufoxWindowClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = rufox_window_dispose;
}

static void rufox_window_init(RufoxWindow *self)
{
	rfx_options_default(&self->opt);
	self->engine = rufox_engine_new();
	g_signal_connect(self->engine, "progress", G_CALLBACK(on_progress), self);
	g_signal_connect(self->engine, "log", G_CALLBACK(on_log), self);
	g_signal_connect(self->engine, "finished", G_CALLBACK(on_finished), self);
	rufox_window_construct(self);
}

GtkWidget* rufox_window_new(RufoxApp *app)
{
	return g_object_new(RUFOX_TYPE_WINDOW, "application", app, NULL);
}
