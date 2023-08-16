#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_prober.h"

#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_builders.h"
#include "util/u_config_json.h"
#include "util/u_system_helpers.h"
#include "target_builder_interface.h"
#include <assert.h>

struct xrt_builder *
t_builder_ovr_create(void)
{
	struct xrt_builder *xb = U_TYPED_CALLOC(struct xrt_builder);
	xb->estimate_system = NULL;
	xb->open_system = NULL;
	xb->destroy = NULL;
	xb->identifier = "ovr";
	xb->name = "ovr devices builder";
	xb->driver_identifiers = NULL;
	xb->driver_identifier_count = 0;
	xb->exclude_from_automatic_discovery = false;

	return xb;
}
