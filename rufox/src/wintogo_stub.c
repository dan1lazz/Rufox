/*
 * STUB wintogo.c — создан ревьюером ТОЛЬКО для диагностики сборки.
 * Оригинальный src/wintogo.c в архиве отсутствует.
 */
#include "rufox.h"

bool rfx_wintogo_supported(const RfxImgReport *img)
{
	(void)img;
	return false;
}

GPtrArray *rfx_wintogo_list_editions(const char *wim_path)
{
	(void)wim_path;
	return g_ptr_array_new_with_free_func(g_free);
}

bool rfx_wintogo_create(const char *devpart, const RfxImgReport *img,
                        const RfxOptions *opt, RfxProgressCb cb, gpointer ud,
                        RfxCancel *cancel)
{
	(void)devpart; (void)img; (void)opt; (void)cb; (void)ud; (void)cancel;
	rfx_set_error(ERR_MISSING_TOOL, 0, "wintogo.c missing from the archive");
	return false;
}
