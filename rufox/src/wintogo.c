/*
 * rufox — Windows To Go capability
 *
 * Windows To Go needs WIM image selection, deployment and boot configuration.
 * Until that pipeline is implemented, fail explicitly instead of presenting a
 * regular Windows installation USB as a working Windows To Go drive.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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
	rfx_set_error(ERR_NOT_SUPPORTED, 0,
	              "Windows To Go is not supported yet; create a standard Windows installation USB instead");
	return false;
}
