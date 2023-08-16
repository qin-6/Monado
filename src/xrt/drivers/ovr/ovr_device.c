// Copyright 2020-2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Sample HMD device, use as a starting point to make your own device driver.
 *
 *
 * Based largely on simulated_hmd.c
 *
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Ryan Pavlik <ryan.pavlik@collabora.com>
 * @ingroup drv_sample
 */
#include <stdio.h>

#include "xrt/xrt_device.h"

#include "os/os_time.h"

#include "math/m_api.h"
#include "math/m_mathinclude.h"

#include "util/u_json.h"
#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_distortion_mesh.h"
#include "cloudxr_streaming_server.h"


#include "ovr_utils.h"


/*
 *
 * Structs and defines.
 *
 */

/*!
 * A sample HMD device.
 *
 * @implements xrt_device
 */

struct ovr_hmd
{
	struct xrt_device base;

	struct xrt_pose center_pose;

	struct ovr_frame_id_queue frame_id_queue;
	
	struct ovr_state_queue state_queue;

	struct ovr_settings settings;

	enum u_logging_level log_level;
};


/// Casting helper function
static inline struct ovr_hmd *
ovr_hmd(struct xrt_device *xdev)
{
	return (struct ovr_hmd *)xdev;
}

#define OH_TRACE(p, ...) U_LOG_XDEV_IFL_T(&p->base, p->log_level, __VA_ARGS__)
#define OH_DEBUG(p, ...) U_LOG_XDEV_IFL_D(&p->base, p->log_level, __VA_ARGS__)
#define OH_ERROR(p, ...) U_LOG_XDEV_IFL_E(&p->base, p->log_level, __VA_ARGS__)
#define OH_INFO(p, ...)	 U_LOG_XDEV_IFL_I(&p->base, p->log_level, __VA_ARGS__)


DEBUG_GET_ONCE_LOG_OPTION(ovr_log, "OVR_LOG", U_LOGGING_INFO)
static void gap(){}


scxrServerHandle handle;
struct ovr_hmd* g_hmd;

static void 
ovr_hmd_destroy(struct xrt_device *xdev)
{
	struct ovr_hmd *oh = ovr_hmd(xdev);

	// Remove the variable tracking.
	u_var_remove_root(oh);

	u_device_free(&oh->base);
}
static void
ovr_hmd_update_inputs(struct xrt_device *xdev)
{
	// Empty, you should put code to update the attached input fields (if any)
}
static void
ovr_hmd_get_tracked_pose(struct xrt_device *xdev,
                            enum xrt_input_name name,
                            uint64_t at_timestamp_ns,
                            struct xrt_space_relation *out_relation)
{
	struct xrt_pose tmp = XRT_POSE_IDENTITY;
    struct ovr_hmd *oh = ovr_hmd(xdev);
    
	uint32_t frame_id;
	ovr_state_queue_pop(&oh->state_queue, &tmp, &frame_id);
	math_quat_normalize(&tmp.orientation);
	math_pose_transform(&oh->center_pose, &tmp, &out_relation->pose);
	out_relation->relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
                                                               	   XRT_SPACE_RELATION_POSITION_VALID_BIT |
                                                               	   XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
	struct xrt_pose pose = out_relation->pose;
	ovr_frame_id_queue_push(&oh->frame_id_queue, frame_id, at_timestamp_ns);
 	//OH_INFO(oh, "ovr px = %f py = %f pz = %f", pose.position.x, pose.position.y, pose.position.z);
	//OH_INFO(oh, "ovr qx = %f qy = %f qz = %f qw = %f", pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w); 	                                                              
}

static void
ovr_hmd_get_view_poses(struct xrt_device *xdev,
                          const struct xrt_vec3 *default_eye_relation,
                          uint64_t at_timestamp_ns,
                          uint32_t view_count,
                          struct xrt_space_relation *out_head_relation,
                          struct xrt_fov *out_fovs,
                          struct xrt_pose *out_poses)
{
	struct ovr_hmd *oh = ovr_hmd(xdev);
	const double hCOP = 0.529;
	const double vCOP = 0.5;

	double left_hfov =  (oh->settings.projection[0] + oh->settings.projection[1]) * M_PI / 180.0;
	double left_vfov =  (oh->settings.projection[2] + oh->settings.projection[3]) * M_PI / 180.0;

	double right_hfov = (oh->settings.projection[4] + oh->settings.projection[5]) * M_PI / 180.0;
	double right_vfov = (oh->settings.projection[6] + oh->settings.projection[7]) * M_PI / 180.0;


	math_compute_fovs(1, hCOP, right_hfov, 1, vCOP, right_vfov, &oh->base.hmd->distortion.fov[1]);
	math_compute_fovs(1, 1 - hCOP, left_hfov, 1, vCOP, left_vfov, &oh->base.hmd->distortion.fov[0]);
	
	u_device_get_view_poses(xdev, default_eye_relation, at_timestamp_ns, view_count, out_head_relation, out_fovs,
	                        out_poses);
}

static void 
ovr_hmd_streaming_log_callback(const char* msg)
{
	if(msg == NULL)
	{
		return;
	}
	OH_INFO(g_hmd, msg);
}

static void 
ovr_hmd_streaming_state_callback(scxrXRTrackingState state, double hmd_time_offset, double ctrls_time_offset)
{
	ovr_state_queue_push(&g_hmd->state_queue, &state);
}

static void
ovr_hmd_streaming_reconfig_callback(int width, int height, int model, int fps, bool ffrEnable)
{
	ovr_settings_resolution_set(&g_hmd->settings, width, height);
	ovr_settings_ffr_set(&g_hmd->settings, ffrEnable);
}

static void
ovr_hmd_streaming_input_callback(scxrInputEvent2 input)
{
	
}

static void 
ovr_hmd_streaming_projection_callback(float* projection)
{
	bool is_valid = ((projection[0] + projection[1]) > 0) && ((projection[2] + projection[3]) > 0) && ((projection[4] + projection[5]) > 0);
	if(!is_valid)
	{
		return;
	}
	memcpy(g_hmd->settings.projection, projection, sizeof(float) * 8);
	

	OH_INFO(g_hmd, "projection1 l = %f, r = %f, t = %f, b = %f", projection[0], projection[1], projection[2], projection[3]);
	OH_INFO(g_hmd, "projection2 l = %f, r = %f, t = %f, b = %f", projection[4], projection[5], projection[6], projection[7]);

	OH_INFO(g_hmd, "left eye: l = %f, r = %f, t = %f, b = %f", g_hmd->base.hmd->distortion.fov[0].angle_left, 
															   g_hmd->base.hmd->distortion.fov[0].angle_right,
															   g_hmd->base.hmd->distortion.fov[0].angle_up,
															   g_hmd->base.hmd->distortion.fov[0].angle_down);

	OH_INFO(g_hmd, "right eye: l = %f, r = %f, t = %f, b = %f",g_hmd->base.hmd->distortion.fov[1].angle_left, 
															   g_hmd->base.hmd->distortion.fov[1].angle_right,
															   g_hmd->base.hmd->distortion.fov[1].angle_up,
															   g_hmd->base.hmd->distortion.fov[1].angle_down);
}

static void
ovr_hmd_streaming_ipd_callback(float ipd)
{
	OH_INFO(g_hmd, "ipd = %f", ipd);
	g_hmd->settings.ipd = ipd;
}
static void 
ovr_hmd_streaming_connection_state_callback(scxrServerConnectionState state, const char* msg)
{
	if(msg == NULL)
	{
		return;		
	}
	OH_INFO(g_hmd, msg);
}
static bool
ovr_hmd_streaming_initialize(struct xrt_device *xdev, void *vk_device)
{
	struct ovr_hmd *oh = ovr_hmd(xdev);	
	scxrServerDesc desc;
	scxrResult ret = scxrResult_Success;
	
	OH_INFO(oh, "create scxrServerDesc !!");
    desc.version = 123;
    desc.encodeParam.width = oh->settings.video->width;
    desc.encodeParam.height = oh->settings.video->height;
    desc.encodeParam.bitRate = oh->settings.video->bitrate;
    desc.encodeParam.frameRate = oh->settings.video->fps;
    desc.encodeParam.isAuto = false;
    desc.engineParam.textureFormat = scxrTextureFormat_R8G8B8A8;
    desc.engineParam.graphicDevice = (scxrGraphicDevice)vk_device;
    desc.engineParam.graphicDeviceType = scxrGraphicDeviceType_Vulkan;
    desc.callbacks.connectState = ovr_hmd_streaming_connection_state_callback;
    desc.callbacks.inputCallback = ovr_hmd_streaming_input_callback;
    desc.callbacks.stateCallback = ovr_hmd_streaming_state_callback;
    desc.callbacks.projectionCallback = ovr_hmd_streaming_projection_callback;
    desc.callbacks.ipdCallback = ovr_hmd_streaming_ipd_callback;
    desc.callbacks.logout = ovr_hmd_streaming_log_callback;
    desc.callbacks.saveTexture = NULL;
	desc.callbacks.renderReconfig = ovr_hmd_streaming_reconfig_callback;
    desc.enablePaas 			= 	oh->settings.pass->enable;
    desc.serviceParam.ipAddr 	=	oh->settings.pass->signaling_server_ip;
    desc.serviceParam.port   	= 	oh->settings.pass->signaling_server_port;
    desc.serviceParam.configPath = "yes";
    desc.serviceParam.profileConfigPath = "yes";
    desc.enableLowLatencyMode = true;
    desc.renderMethod = scxrRenderMethod_OVR;
	desc.imageQualityAssessOpen = false;	
    ret = scxrServerInitialize(&handle, &desc);
	ret = scxrServerConnect(handle);
	
	return true;
}

static bool
ovr_hmd_update_resolution(struct xrt_device *xdev, uint32_t width, uint32_t height)
{
	struct ovr_hmd *oh = ovr_hmd(xdev);	
	scxrServerUpdateResolution(handle, width, height);
	return true;
}

static bool 
ovr_hmd_send_frame(struct xrt_device *xdev, 
							   void *frame, 
							   uint64_t at_timestamp_ns)
{
	struct ovr_hmd *oh = ovr_hmd(xdev);
	uint64_t desired_timestamp_ns = 0;
	uint32_t desired_frame_id = 0;
	do{
		ovr_frame_id_queue_pop(&oh->frame_id_queue, &desired_frame_id, &desired_timestamp_ns);
	}while(desired_timestamp_ns < at_timestamp_ns);
	//OH_INFO(oh, "send frame: at_timestamp_ns = %lld, desired_timestamp = %lld ,  frame id = %d", at_timestamp_ns, desired_timestamp_ns, desired_frame_id);
	desired_timestamp_ns -= 4000000;
	if(at_timestamp_ns == desired_timestamp_ns && desired_frame_id != 0)
	{
		scxrServerSyncWithEvent(handle, scxrServerSyncEvent_WaitEncoding);
		scxrTextureData textureData;
		textureData.frameId = desired_frame_id;
		textureData.textureHandle = frame;
		scxrServerSendVideoFrame(handle, &textureData);
	}	
	else
	{
		scxrServerSyncWithEvent(handle, scxrServerSyncEvent_NewFrameReady);	
		//OH_INFO(oh, "can not find desired frame id");
	}
	
	return true;
}

static bool
ovr_hmd_check_for_resolution(struct xrt_device *xdev,
									  	   uint32_t *width,
									       uint32_t *height)
{	
	bool ret = false;
	float aspect = 1.0;
    struct ovr_hmd *oh = ovr_hmd(xdev);
	//OH_INFO(oh, "check for res width = %d, height = %d", *width, *height);
	ret = ovr_settings_resolution_get(&oh->settings, width, height);
	if(ret)
	{
		aspect = g_hmd->settings.video->width / g_hmd->settings.video->height / 2.0;
	//	ovr_fov_transition(aspect, &g_hmd->eye_fov[0], &g_hmd->base.hmd->distortion.fov[0]);
   	//	ovr_fov_transition(aspect, &g_hmd->eye_fov[1], &g_hmd->base.hmd->distortion.fov[1]);
	}
	return ret;
}

static bool ovr_hmd_check_for_ffr(void *xdev)
{
	struct ovr_hmd *oh = ovr_hmd(xdev); 
	return oh->settings.ffr->enable;	
}

struct xrt_device *
ovr_hmd_create(void)
{
	const char *session_path = ".\\config\\session.json";
	// This indicates you won't be using Monado's built-in tracking algorithms.
	enum u_device_alloc_flags flags = (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);
	
	struct ovr_hmd *sh = U_DEVICE_ALLOCATE(struct ovr_hmd, flags, 1, 0);

	ovr_settings_form_json(session_path, &sh->settings);
	// This list should be ordered, most preferred first.
	size_t idx = 0;
	sh->base.hmd->blend_modes[idx++] = XRT_BLEND_MODE_OPAQUE;
	sh->base.hmd->blend_mode_count = idx;

	sh->base.update_inputs = ovr_hmd_update_inputs;
	sh->base.get_tracked_pose = ovr_hmd_get_tracked_pose;
	sh->base.get_view_poses = ovr_hmd_get_view_poses;	
	sh->base.destroy = ovr_hmd_destroy;
	sh->base.streaming_initialize = ovr_hmd_streaming_initialize;
	sh->base.streaming_onframe = ovr_hmd_send_frame;
	sh->base.streaming_check_resolution = ovr_hmd_check_for_resolution;
	sh->base.streaming_update_resolution = ovr_hmd_update_resolution;

	sh->center_pose = (struct xrt_pose){XRT_QUAT_IDENTITY, {0.0f, 1.6f, 0.0f}};
	sh->log_level = debug_get_log_option_ovr_log();

	//Init ffr parameters
	sh->base.ffr_bundle.center_size_x = sh->settings.ffr->center_size_x;
	sh->base.ffr_bundle.center_size_y = sh->settings.ffr->center_size_y;
	sh->base.ffr_bundle.center_shift_x = sh->settings.ffr->center_shift_x;
	sh->base.ffr_bundle.center_shift_y = sh->settings.ffr->center_shift_y;
	sh->base.ffr_bundle.edge_ratio_x = sh->settings.ffr->edge_ratio_x;
	sh->base.ffr_bundle.edge_ratio_y = sh->settings.ffr->edge_ratio_y;

	memset(sh->base.ffr_bundle.vert_shader, 0, XRT_DEVICE_NAME_LEN);
	memset(sh->base.ffr_bundle.frag_shader, 0, XRT_DEVICE_NAME_LEN);

	memcpy(sh->base.ffr_bundle.vert_shader, sh->settings.ffr->vert_shader_path, strlen(sh->settings.ffr->vert_shader_path));
	memcpy(sh->base.ffr_bundle.frag_shader, sh->settings.ffr->frag_shader_path, strlen(sh->settings.ffr->frag_shader_path));
	
	sh->base.ffr_bundle.ffr_enable = ovr_hmd_check_for_ffr;

	// Print name.
	snprintf(sh->base.str, XRT_DEVICE_NAME_LEN, "Ovr HMD");
	snprintf(sh->base.serial, XRT_DEVICE_NAME_LEN, "Ovr HMD S/N");

	// Setup input.
	sh->base.name = XRT_DEVICE_GENERIC_HMD;
	sh->base.device_type = XRT_DEVICE_TYPE_HMD;
	sh->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
	sh->base.orientation_tracking_supported = true;
	sh->base.position_tracking_supported = false;

	// Set up display details
	// refresh rate
	sh->base.hmd->screens[0].nominal_frame_interval_ns = time_s_to_ns(1.0f / 90.0f);

	const double hFOV = 90 * (M_PI / 180.0);
	const double vFOV = 96.73 * (M_PI / 180.0);
	// center of projection
	
	const double hCOP = 0.529;
	const double vCOP = 0.5;
	if (
	    /* right eye */
	    !math_compute_fovs(1, hCOP, hFOV, 1, vCOP, vFOV, &sh->base.hmd->distortion.fov[1]) ||
	    /*
	     * left eye - same as right eye, except the horizontal center of projection is moved in the opposite
	     * direction now
	     */
	    !math_compute_fovs(1, 1.0 - hCOP, hFOV, 1, vCOP, vFOV, &sh->base.hmd->distortion.fov[0])) {
		// If those failed, it means our math was impossible.
		OH_ERROR(sh, "Failed to setup basic device info");
		ovr_hmd_destroy(&sh->base);
		return NULL;
	}
	
	OH_INFO(sh, "get version = %s", sh->settings.version);
	const uint32_t panel_w = sh->settings.video->width / 2;
	const uint32_t panel_h = sh->settings.video->height;

	// Single "screen" (always the case)
	sh->base.hmd->screens[0].w_pixels = sh->settings.video->width;
	sh->base.hmd->screens[0].h_pixels = sh->settings.video->height;

	// Left, Right
	for (uint8_t eye = 0; eye < 2; ++eye) {
		sh->base.hmd->views[eye].display.w_pixels = panel_w;
		sh->base.hmd->views[eye].display.h_pixels = panel_h;
		sh->base.hmd->views[eye].viewport.y_pixels = 0;
		sh->base.hmd->views[eye].viewport.w_pixels = panel_w;
		sh->base.hmd->views[eye].viewport.h_pixels = panel_h;
		// if rotation is not identity, the dimensions can get more complex.
		sh->base.hmd->views[eye].rot = u_device_rotation_ident;
	}
	// left eye starts at x=0, right eye starts at x=panel_width
	sh->base.hmd->views[0].viewport.x_pixels = 0;
	sh->base.hmd->views[1].viewport.x_pixels = panel_w;

	// Distortion information, fills in xdev->compute_distortion().
	u_distortion_mesh_set_none(&sh->base);

	// Init pose queue
	ovr_state_queue_init(&sh->state_queue);

	// Init frame id queue
	ovr_frame_id_queue_init(&sh->frame_id_queue);

	// Setup variable tracker: Optional but useful for debugging
	u_var_add_root(sh, "Ovr HMD", true);
	u_var_add_pose(sh, &sh->center_pose, "center_pose");
	u_var_add_log_level(sh, &sh->log_level, "log_level");	
	g_hmd = sh;
	return &sh->base;
}
