#include <stdio.h>
#include "ovr_utils.h"
#include "math/m_mathinclude.h"


const char *session_path = "E:\\Monado\\monado\\src\\xrt\\drivers\\ovr\\resources\\session.json";


inline float SIGN(float x) 
{
	return (x >= 0.0f) ? +1.0f : -1.0f;
}

inline float NORM(float a, float b, float c, float d)
{
	return sqrt(a * a + b * b + c * c + d * d);
}

inline void Matrix_MatToQuat(const scxrMatrix34* m, struct xrt_quat* q) 
{
	float r11 = m->m[0][0];
	float r12 = m->m[0][1];
	float r13 = m->m[0][2];
	float r21 = m->m[1][0];
	float r22 = m->m[1][1];
	float r23 = m->m[1][2];
	float r31 = m->m[2][0];
	float r32 = m->m[2][1];
	float r33 = m->m[2][2];
	float q0 = (r11 + r22 + r33 + 1.0f) / 4.0f;
	float q1 = (r11 - r22 - r33 + 1.0f) / 4.0f;
	float q2 = (-r11 + r22 - r33 + 1.0f) / 4.0f;
	float q3 = (-r11 - r22 + r33 + 1.0f) / 4.0f;
	if (q0 < 0.0f) {
		q0 = 0.0f;
	}
	if (q1 < 0.0f) {
		q1 = 0.0f;
	}
	if (q2 < 0.0f) {
		q2 = 0.0f;
	}
	if (q3 < 0.0f) {
		q3 = 0.0f;
	}
	q0 = sqrt(q0);
	q1 = sqrt(q1);
	q2 = sqrt(q2);
	q3 = sqrt(q3);
	if (q0 >= q1 && q0 >= q2 && q0 >= q3) {
		q0 *= +1.0f;
		q1 *= SIGN(r32 - r23);
		q2 *= SIGN(r13 - r31);
		q3 *= SIGN(r21 - r12);
	}
	else if (q1 >= q0 && q1 >= q2 && q1 >= q3) {
		q0 *= SIGN(r32 - r23);
		q1 *= +1.0f;
		q2 *= SIGN(r21 + r12);
		q3 *= SIGN(r13 + r31);
	}
	else if (q2 >= q0 && q2 >= q1 && q2 >= q3) {
		q0 *= SIGN(r13 - r31);
		q1 *= SIGN(r21 + r12);
		q2 *= +1.0f;
		q3 *= SIGN(r32 + r23);
	}
	else if (q3 >= q0 && q3 >= q1 && q3 >= q2) {
		q0 *= SIGN(r21 - r12);
		q1 *= SIGN(r31 + r13);
		q2 *= SIGN(r32 + r23);
		q3 *= +1.0f;
	}
	else {
		//printf("coding error\n");
	}
	float r = NORM(q0, q1, q2, q3);
	q0 /= r;
	q1 /= r;
	q2 /= r;
	q3 /= r;
	q->w = q0; q->x = q1; q->y = q2; q->z = q3;
}

void ovr_fov_transition(float aspect, struct xrt_fov *in_fov, struct xrt_fov *out_fov)
{

	out_fov->angle_left	  = -aspect * tanf(in_fov->angle_left / 180.0 * M_PI);
	out_fov->angle_right  =  aspect * tanf(in_fov->angle_right / 180.0 * M_PI);
	out_fov->angle_up     =  tanf(in_fov->angle_up / 180.0 * M_PI);
	out_fov->angle_down   = -tanf(in_fov->angle_down / 180.0 * M_PI);
}

void ovr_state_queue_init(struct ovr_state_queue * queue)
{
	queue->front = queue->rear  = 0;
	queue->size = 0;
	memset(queue->state, 0, sizeof(queue->state));
	os_mutex_init(&queue->lock);
	queue->state[queue->rear].pose = (struct xrt_pose){XRT_QUAT_IDENTITY, {0.0f, 1.6f, 0.0f}};
	queue->state[queue->rear].frame_id = 0;

}
void ovr_state_queue_push(struct ovr_state_queue * queue, scxrXRTrackingState * state)
{
	scxrMatrix34 matrix;
	memset(matrix.m, 0, sizeof(float) * 12);
	memcpy(matrix.m, state->hmd.pose.deviceToAbsoluteTracking.m, sizeof(float) * 12);
	
	queue->state[queue->rear].frame_id = state->hmd.pose.id;
	queue->state[queue->rear].pose.position.x = matrix.m[0][3];
	queue->state[queue->rear].pose.position.y = matrix.m[1][3];
	queue->state[queue->rear].pose.position.z = matrix.m[2][3];
	
	Matrix_MatToQuat(&matrix, &queue->state[queue->rear].pose.orientation);
	queue->rear = (queue->rear + 1) % MAX_SIZE;
	os_mutex_lock(&queue->lock);
	if(queue->size == MAX_SIZE) 						//队满
	{
		queue->front = (queue->front + 1) % MAX_SIZE; 	//丢弃最老的
	}
	else
	{
		queue->size ++;
	}
	os_mutex_unlock(&queue->lock);
	
}
void ovr_state_queue_pop(struct ovr_state_queue * queue, struct xrt_pose *pose, uint32_t *frame_id)
{	
	os_mutex_lock(&queue->lock);
	memcpy(pose, &queue->state[queue->front].pose, sizeof(struct xrt_pose));
	*frame_id = queue->state[queue->front].frame_id;	
	if(queue->size == 0)
	{
		os_mutex_unlock(&queue->lock);
		return;
	}
	queue->front = (queue->front + 1) % MAX_SIZE;
	queue->size --;
	os_mutex_unlock(&queue->lock);
}


void ovr_frame_id_queue_init(struct ovr_frame_id_queue *queue)
{
	queue->front = queue->rear  = 0;
	queue->size = 0;
	memset(queue->frame_id_with_timestamp, 0, sizeof(queue->frame_id_with_timestamp));
	os_mutex_init(&queue->lock);
}

void ovr_frame_id_queue_push(struct ovr_frame_id_queue *queue, uint32_t frame_id, uint64_t at_timestamp_ns)
{
	queue->frame_id_with_timestamp[queue->rear].frame_id = frame_id;
	queue->frame_id_with_timestamp[queue->rear].timestamp = at_timestamp_ns;

	queue->rear = (queue->rear + 1) % MAX_SIZE;
	os_mutex_lock(&queue->lock);
	if(queue->size == MAX_SIZE) 						//队满
	{
		queue->front = (queue->front + 1) % MAX_SIZE; 	//丢弃最老的
	}
	else
	{
		queue->size ++;
	}
	os_mutex_unlock(&queue->lock);
}

void ovr_frame_id_queue_pop(struct ovr_frame_id_queue *queue, uint32_t *frame_id, uint64_t *at_timestamp_ns)
{
	if(queue->size == 0)
	{
		*frame_id = 0;
		*at_timestamp_ns = 0;
		return;
	}
	os_mutex_lock(&queue->lock);
	*frame_id = queue->frame_id_with_timestamp[queue->front].frame_id;
	*at_timestamp_ns = queue->frame_id_with_timestamp[queue->front].timestamp;

	queue->front = (queue->front + 1) % MAX_SIZE;
	queue->size --;
	os_mutex_unlock(&queue->lock);
}

void ovr_settings_resolution_set(struct ovr_settings *settings, uint32_t width, uint32_t height)
{
	os_mutex_lock(&settings->lock);
	settings->video->width = width;
	settings->video->height = height;
	os_mutex_unlock(&settings->lock);
}

void ovr_settings_ffr_set(struct ovr_settings *settings, bool ffrEnable)
{
	settings->ffr->enable = (settings->ffr->supported) ? ffrEnable : false;
}
bool ovr_settings_resolution_get(struct ovr_settings *settings, uint32_t *width, uint32_t *height)
{
	if(*width == settings->video->width &&
	   *height == settings->video->height)
	{
		return false;
	}
	os_mutex_lock(&settings->lock);
	*width = settings->video->width;
	*height = settings->video->height;
	os_mutex_unlock(&settings->lock);
	return true;
}

void ovr_settings_destroy(struct ovr_settings *settings)
{
	if(settings->pass)
	{
		free(settings->pass);
		settings->pass = NULL;
	}
	if(settings->video)
	{
		free(settings->video);
		settings->video = NULL;
	}
	if(settings->ffr)
	{
		free(settings->ffr);
		settings->ffr = NULL;
	}
}
bool ovr_settings_form_json(const char *path, struct ovr_settings *settings)
{

	const char *session_path = "E:\\Monado\\monado\\src\\xrt\\drivers\\ovr\\resources\\session.json";
	
	FILE *fd;
	uint32_t file_size = 0;
	
	fd = fopen(path, "r");
	assert(fd != NULL);
	fseek(fd, 0, SEEK_END);
	file_size = ftell(fd);
	fseek(fd, 0, SEEK_SET);
	
	char *json_string = U_TYPED_ARRAY_CALLOC(char, file_size);
	
	memset(json_string, 0, file_size);
	uint32_t len = fread(json_string, 1, file_size, fd);
	fclose(fd);
	if(len == 0)
	{
		return false;
	}

	cJSON *json = cJSON_Parse(json_string);

	if (!cJSON_IsObject(json)) 
	{
		return false;
	}
	/*init mutex*/
	os_mutex_init(&settings->lock);
	
	/*get version*/
	ovr_json_string(json, "version", settings->version);

	/*get paas service params*/
	struct paas_service *paas = U_TYPED_CALLOC(struct paas_service);
	if(paas == NULL)
	{
		return false;
	}
	U_ZERO(paas);
	cJSON *paas_json = u_json_get(json, "paas_service");
	if(paas_json == NULL)
	{
		return false;
	}
	ovr_json_bool(paas_json, "enable", &paas->enable);
	ovr_json_int(paas_json, "signaling_server_port", &paas->signaling_server_port);
	ovr_json_string(paas_json, "signaling_server_ipaddr", paas->signaling_server_ip);

	settings->pass = paas;

	/*get video params*/
	struct video_params *video = U_TYPED_CALLOC(struct video_params);
	if(video == NULL)
	{
		return false;
	}
	U_ZERO(video);
	cJSON *video_json = u_json_get(json, "video_params");
	if(paas_json == NULL)
	{
		return false;
	}
	ovr_json_int(video_json, "width", &video->width);
	ovr_json_int(video_json, "height", &video->height);
	ovr_json_int(video_json, "fps", &video->fps);
	ovr_json_int(video_json, "bitrate", &video->bitrate);

	settings->video = video;
	
	/*get fix foveation render params*/
	struct fix_foveation_render *ffr = U_TYPED_CALLOC(struct fix_foveation_render);
	if(ffr == NULL)
	{
		return false;
	}
	U_ZERO(ffr);
	cJSON *ffr_json = u_json_get(json, "fix_foveation_render");
	if(ffr_json == NULL)
	{
		return false;
	}
	ovr_json_float(ffr_json, "center_size_x", &ffr->center_size_x);
	ovr_json_float(ffr_json, "center_size_y", &ffr->center_size_y);
	ovr_json_float(ffr_json, "center_shift_x", &ffr->center_shift_x);
	ovr_json_float(ffr_json, "center_shift_y", &ffr->center_shift_y);
	ovr_json_float(ffr_json, "edge_ratio_x", &ffr->edge_ratio_x);
	ovr_json_float(ffr_json, "edge_ratio_y", &ffr->edge_ratio_y);
	
	ovr_json_string(ffr_json, "frag_shader_path", ffr->frag_shader_path);
	ovr_json_string(ffr_json, "vert_shader_path", ffr->vert_shader_path);

	ovr_json_bool(ffr_json, "supported", &ffr->supported);
	ovr_json_bool(ffr_json, "enable", &ffr->enable);
	
	settings->ffr = ffr;
	free(json_string);
	return true;
}


