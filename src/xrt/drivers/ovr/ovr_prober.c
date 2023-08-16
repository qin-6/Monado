// Copyright 2020-2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Ovr prober code.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup drv_sample
 */

#include "xrt/xrt_prober.h"

#include "util/u_misc.h"
#include "util/u_debug.h"

#include "ovr_interface.h"


/*!
 * @implements xrt_auto_prober
 */
struct ovr_auto_prober
{
	struct xrt_auto_prober base;
};

//! @private @memberof sample_auto_prober
static inline struct ovr_auto_prober *
ovr_auto_prober(struct xrt_auto_prober *p)
{
	return (struct ovr_auto_prober *)p;
}

//! @private @memberof sample_auto_prober
static void
ovr_auto_prober_destroy(struct xrt_auto_prober *p)
{
	struct ovr_auto_prober *sap = ovr_auto_prober(p);

	free(sap);
}

//! @public @memberof sample_auto_prober
static int
ovr_auto_prober_autoprobe(struct xrt_auto_prober *xap,
                             cJSON *attached_data,
                             bool no_hmds,
                             struct xrt_prober *xp,
                             struct xrt_device **out_xdevs)
{
	struct ovr_auto_prober *sap = ovr_auto_prober(xap);
	(void)sap;

	// Do not create a sample HMD if we are not looking for HMDs.
	if (no_hmds) {
		return 0;
	}

	out_xdevs[0] = ovr_hmd_create();
	return 1;
}

struct xrt_auto_prober *
ovr_create_auto_prober(void)
{
	struct ovr_auto_prober *sap = U_TYPED_CALLOC(struct ovr_auto_prober);
	sap->base.name = "Oculus Virtual Driver";
	sap->base.destroy = ovr_auto_prober_destroy;
	sap->base.lelo_dallas_autoprobe = ovr_auto_prober_autoprobe;

	return &sap->base;
}
