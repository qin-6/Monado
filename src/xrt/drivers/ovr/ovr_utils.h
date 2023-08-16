#pragma once
#include "os/os_threading.h"
#include "cloudxr_streaming_common.h"
#include "xrt/xrt_device.h"
#include "util/u_json.h"


#define ovr_json_bool(obj, name, data) 		u_json_get_bool(u_json_get(obj, name), data)
#define ovr_json_float(obj, name, data)		u_json_get_float(u_json_get(obj, name), data)
#define ovr_json_int(obj, name, data)		u_json_get_int(u_json_get(obj, name), data)
#define ovr_json_string(obj, name, data)	u_json_get_string_into_array(u_json_get(obj, name), data, sizeof(data))

struct paas_service
{
	bool enable;
	char signaling_server_ip[32];
	uint32_t signaling_server_port;
};

struct video_params
{
	uint32_t width;
	uint32_t height;
	uint32_t fps;
	uint32_t bitrate;
};

struct fix_foveation_render{
	bool enable;
	bool supported;
	
	char frag_shader_path[128];
	char vert_shader_path[128];
	
	float center_size_x;
	float center_size_y;

	float center_shift_x;
	float center_shift_y;

	float edge_ratio_x;
	float edge_ratio_y;	
};

struct ovr_settings
{
	float    ipd;	
	float 	 projection[8]; 
	char version[32];
	struct os_mutex 	lock;
	struct paas_service *pass;
	struct video_params *video;
	struct fix_foveation_render *ffr;
};

struct ovr_frame_id_queue
{
	#define MAX_SIZE 100
	uint32_t front;
	uint32_t rear;
	uint32_t size;
	struct os_mutex lock;
	struct
	{
		uint32_t frame_id;
		uint64_t timestamp;
	}frame_id_with_timestamp[MAX_SIZE];
};

void ovr_frame_id_queue_init(struct ovr_frame_id_queue *queue);


void ovr_frame_id_queue_push(struct ovr_frame_id_queue *queue, uint32_t frame_id, uint64_t at_timestamp_ns);

void ovr_frame_id_queue_pop(struct ovr_frame_id_queue *queue, uint32_t *frame_id, uint64_t *at_timestamp_ns);

struct ovr_state_queue
{
	uint32_t size;
	uint32_t front;
	uint32_t rear;
	struct os_mutex lock;
	struct 
	{
		struct xrt_pose pose;
		uint32_t frame_id;
	}state[MAX_SIZE];
	
}ovr_state_queue;

void ovr_state_queue_init(struct ovr_state_queue * queue);

void ovr_state_queue_push(struct ovr_state_queue *queue, scxrXRTrackingState *state);

void ovr_state_queue_pop(struct ovr_state_queue *queue, struct xrt_pose *pose, uint32_t *frame_id);

void ovr_fov_transition(float aspect, struct xrt_fov *in_fov, struct xrt_fov *out_fov);


void ovr_settings_resolution_set(struct ovr_settings *settings, uint32_t width, uint32_t height);

bool ovr_settings_resolution_get(struct ovr_settings *settings, uint32_t *width, uint32_t *height);


bool ovr_settings_form_json(const char *path, struct ovr_settings *settings);


