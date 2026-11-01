/*
 * rufox — cancellation token (Rufus: CHECK_FOR_USER_CANCEL / FormatStatus)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"

/*
 * Ownership contract (see docs/REVIEW.md M8): the token is created by the
 * thread that drives rfx_job_run() and must outlive it. Worker threads only
 * ever *read* it through rfx_cancel_requested(); nobody but the owner frees
 * it. Access is via g_atomic_int_*, so no `volatile` and no refcount needed.
 */
struct _RfxCancel {
	gint requested;
};

RfxCancel* rfx_cancel_new(void)
{
	return g_new0(RfxCancel, 1);   /* g_new0 zeroes -> requested == 0 */
}

void rfx_cancel_request(RfxCancel *c)
{
	if (c != NULL)
		g_atomic_int_set(&c->requested, 1);
}

bool rfx_cancel_requested(RfxCancel *c)
{
	return c != NULL && g_atomic_int_get(&c->requested) != 0;
}

void rfx_cancel_free(RfxCancel *c)
{
	g_free(c);
}
