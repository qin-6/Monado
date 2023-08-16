#include <stdlib.h>
#include <string.h>
#include "xrt/xrt_compiler.h"
#include "main/comp_window.h"
#include "main/comp_target.h"
#include "util/u_misc.h"
#include "os/os_threading.h"

#include "util/u_misc.h"
#include "util/u_pacing.h"
#include "util/u_pretty_print.h"

const float vertices[] = {
	 1.0,  1.0, 0.0, 1.0, 1.0,
	 1.0, -1.0, 0.0, 1.0, 0.0,
	-1.0, -1.0, 0.0, 0.0, 0.0,
	-1.0,  1.0, 0.0, 0.0, 1.0
};
	
const uint32_t indices[] = {
	 0, 1, 2, 0, 2, 3
};

struct foveation_vars
{
	uint32_t target_eye_width;
	uint32_t target_eye_height;
	uint32_t optimized_eye_width;
	uint32_t optimized_eye_height;

	float eye_width_ratio;
	float eye_height_ratio;

	float center_size_x;
	float center_size_y;
	float center_shift_x;
	float center_shift_y;
	float edge_ratio_x;
	float edge_ratio_y;
};

struct fix_foveated_render
{
	bool 		initialized;
	uint32_t 	width;
	uint32_t 	height;
	VkFormat 	format;
	struct foveation_vars foveation_vars_ubo;
	struct xrt_ffr_bundle *ffr_bundle;
	struct
	{	
		uint32_t 		count;
		uint32_t		index;
		VkImage 		*image;
		VkDeviceMemory 	*memory;
		VkImageView 	*view;	
		VkFramebuffer   *framebuffer;
	}target;

	struct{
		VkShaderModule mesh_vert;
        VkShaderModule mesh_frag;
	}shaders;
	
	struct
	{
		uint32_t src_binding;

		uint32_t ubo_binding;

		VkPipelineLayout pipeline_layout;

		VkCommandPool 	commandpool;
		
		VkCommandBuffer commandbuffer;
		
		VkRenderPass 	render_pass;
				
		VkPipeline 		pipeline;

		VkDescriptorPool descriptor_pool;
		
		VkDescriptorSet descriptor_set;

		VkDescriptorSetLayout descriptor_set_layout;
				
		VkSampler 		sampler;

		VkFence			renderfence;

		VkPipelineCache pipeline_cache;

		struct vk_buffer vbo;
		struct vk_buffer ibo;
		struct vk_buffer ubo;

		uint32_t stride;
		uint32_t index_count_total;
	} mesh;
};

struct streaming_swaipchain
{
	struct os_thread_helper oth;

};

struct comp_window_offscreen
{
	struct comp_target_swapchain base;
	uint32_t image_count;
	uint32_t image_index;
	VkDeviceMemory* image_memory;
	struct fix_foveated_render 	ffr;
	struct streaming_swaipchain swaipchain;
};

static inline struct vk_bundle *
get_vk(struct comp_window_offscreen *cwo)
{
	return &cwo->base.base.c->base.vk;
}

static inline int
find_memory_type(struct vk_bundle *vk, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
	VkPhysicalDeviceMemoryProperties memProperties;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
		if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}
	return -1;
}

static inline VkFormat
find_depth_format(struct vk_bundle *vk)
{
	VkFormat candidates[3] = {
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D24_UNORM_S8_UINT
	};
	VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
	VkFormatFeatureFlags features = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
	for(int i = 0; i < 3; i ++)
	{
		VkFormatProperties props;		
		vkGetPhysicalDeviceFormatProperties(vk->physical_device, candidates[i], &props);
		if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) 
		{
				return candidates[i];
		}
		else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) 
		{
				return candidates[i];
		}
	}
	return VK_FORMAT_UNDEFINED;
}

static void
target_fini_semaphores(struct comp_target_swapchain *cts)
{
	struct vk_bundle *vk = get_vk(cts);

	if (cts->base.semaphores.present_complete != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, cts->base.semaphores.present_complete, NULL);
		cts->base.semaphores.present_complete = VK_NULL_HANDLE;
	}

	if (cts->base.semaphores.render_complete != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, cts->base.semaphores.render_complete, NULL);
		cts->base.semaphores.render_complete = VK_NULL_HANDLE;
	}
}

static void
target_init_semaphores(struct comp_target_swapchain *cts)
{
	struct vk_bundle *vk = get_vk(cts);
	VkResult ret;

	target_fini_semaphores(cts);

	VkSemaphoreCreateInfo info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};

	ret = vk->vkCreateSemaphore(vk->device, &info, NULL, &cts->base.semaphores.present_complete);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(cts->base.c, "vkCreateSemaphore: %s", vk_result_string(ret));
	}

	cts->base.semaphores.render_complete_is_timeline = false;
	ret = vk->vkCreateSemaphore(vk->device, &info, NULL, &cts->base.semaphores.render_complete);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(cts->base.c, "vkCreateSemaphore: %s", vk_result_string(ret));
	}
}

static void
destroy_image_views(struct comp_target_swapchain *cts)
{
	if (cts->base.images == NULL) {
		return;
	}

	struct vk_bundle *vk = get_vk(cts);
	
	for (uint32_t i = 0; i < cts->base.image_count; i++) {
		if (cts->base.images[i].view == VK_NULL_HANDLE) {
			continue;
		}

		vk->vkDestroyImageView(vk->device, cts->base.images[i].view, NULL);
		cts->base.images[i].view = VK_NULL_HANDLE;
	}

	for	(uint32_t i = 0; i < cts->base.image_count; i ++){
		if(cts->base.images[i].handle == VK_NULL_HANDLE)
		{
			continue;
		}
		vk->vkDestroyImage(vk->device, cts->base.images[i].handle, NULL);
		cts->base.images[i].handle = VK_NULL_HANDLE;
	}

	free(cts->base.images);
	cts->base.images = NULL;
}

static void 
create_image_view(struct comp_target_swapchain *cts)
{
	struct vk_bundle *vk = get_vk(cts);
	
	VkImageSubresourceRange subresource_range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = 1,
	    .baseArrayLayer = 0,
	    .layerCount = 1,
	};
	for (uint32_t i = 0; i < cts->base.image_count; i++) {
		vk_create_view(                 //
		    vk,                         // vk_bundle
		    cts->base.images[i].handle, // image
		    VK_IMAGE_VIEW_TYPE_2D,      // type
		    cts->base.format, 			// format
		    subresource_range,          // subresource_range
		    &cts->base.images[i].view); // out_view
	}
}

static void 
create_image_memory(struct comp_target_swapchain	*cts)
{
	struct vk_bundle *vk = get_vk(cts);
	struct comp_window_offscreen *cwo = (struct comp_window_offscreen *)cts;
		
	cwo->image_memory  = U_TYPED_ARRAY_CALLOC(VkDeviceMemory, cwo->image_count);
	assert(cwo->image_memory != NULL);
	for(uint32_t i = 0; i < cwo->image_count; i ++)
	{
		VkMemoryRequirements memRequirements;
		vk->vkGetImageMemoryRequirements(vk->device, cts->base.images[i].handle, &memRequirements);
		int memory_type_index = find_memory_type(vk, memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if(memory_type_index < 0)
		{
			COMP_ERROR(cts->base.c, "Can't find required memory type");
			return;
		}
		COMP_INFO(cts->base.c, "Find required memory type index : %d", memory_type_index);
		VkMemoryAllocateInfo allocInfo = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = memRequirements.size,
			.memoryTypeIndex = memory_type_index,
		};
		vk->vkAllocateMemory(vk->device, &allocInfo, NULL, &cwo->image_memory[i]);
		vk->vkBindImageMemory(vk->device, cts->base.images[i].handle, cwo->image_memory[i], 0);
	}
}

static void 
destroy_image_memory(struct comp_target_swapchain	*cts)
{
	struct comp_window_offscreen *cwo = (struct comp_window_offscreen *)cts;
	struct vk_bundle *vk = get_vk(cts);

	if(cwo->image_memory == NULL){
		return;
	}
	for(uint32_t i = 0; i < cwo->image_count; i ++){
		if(cwo->image_memory[i] == VK_NULL_HANDLE){
			continue;
		}
		vk->vkFreeMemory(vk->device, cwo->image_memory[i], NULL);
		cwo->image_memory[i] = VK_NULL_HANDLE;
	}

	free(cwo->image_memory);
	cwo->image_memory = NULL;
}

static void
do_update_timings_google_display_timing(struct comp_target_swapchain *cts)
{
	struct vk_bundle *vk = get_vk(cts);
	//COMP_INFO(cts->base.c, "has GOOGLE display timing %d", vk->has_GOOGLE_display_timing);
	
	if (!vk->has_GOOGLE_display_timing) {
		
		return;
	}

	if (cts->swapchain.handle == VK_NULL_HANDLE) {
		return;
	}

	uint32_t count = 0;
	vk->vkGetPastPresentationTimingGOOGLE( //
	    vk->device,                        //
	    cts->swapchain.handle,             //
	    &count,                            //
	    NULL);                             //
	if (count <= 0) {
		return;
	}

	VkPastPresentationTimingGOOGLE *timings = U_TYPED_ARRAY_CALLOC(VkPastPresentationTimingGOOGLE, count);
	vk->vkGetPastPresentationTimingGOOGLE( //
	    vk->device,                        //
	    cts->swapchain.handle,             //
	    &count,                            //
	    timings);                          //
	uint64_t now_ns = os_monotonic_get_ns();
	for (uint32_t i = 0; i < count; i++) {
		u_pc_info(cts->upc,                       //
		          timings[i].presentID,           //
		          timings[i].desiredPresentTime,  //
		          timings[i].actualPresentTime,   //
		          timings[i].earliestPresentTime, //
		          timings[i].presentMargin,       //
		          now_ns);                        //
	}

	free(timings);
}

static void
do_update_timings_vblank_thread(struct comp_target_swapchain *cts)
{
	if (!cts->vblank.has_started) {
		return;
	}

	uint64_t last_vblank_ns;

	os_thread_helper_lock(&cts->vblank.event_thread);
	last_vblank_ns = cts->vblank.last_vblank_ns;
	cts->vblank.last_vblank_ns = 0;
	os_thread_helper_unlock(&cts->vblank.event_thread);

	if (last_vblank_ns) {
		u_pc_update_vblank_from_display_control(cts->upc, last_vblank_ns);
	}
}

static VkShaderModule
ffr_create_shader_module(struct vk_bundle *vk, char *shader_path)
{
	FILE *fd;
	uint32_t code_size = 0;
	char code_addr[1024 * 10];
	VkShaderModule shaderModule = VK_NULL_HANDLE;
	fd = fopen(shader_path, "rb");
	if(fd == NULL)
	{
		VK_ERROR(vk, "open [%s] failed", shader_path);
	}
	memset(code_addr, 0, 1024 * 10);

	code_size = fread(code_addr, 1, 1024 * 10, fd);
	fclose(fd);
	VK_INFO(vk, "ffr get shader size = %d", code_size);
	VkShaderModuleCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = code_size,
		.pCode = (uint32_t *)code_addr,
	};
	
	if (vk->vkCreateShaderModule(vk->device, &createInfo, NULL, &shaderModule) != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreateCommandPool failed");
	}

	return shaderModule;
}

static void 
ffr_create_commandpool(struct vk_bundle *vk, struct fix_foveated_render *ffr)
{
	VkCommandPoolCreateInfo poolInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = vk->queue_family_index,
		.flags = 0,
	};

	if (vk->vkCreateCommandPool(vk->device, &poolInfo, VK_NULL_HANDLE, &ffr->mesh.commandpool) != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreateCommandPool failed");
	}

}
static void 
ffr_create_images(struct vk_bundle *vk, struct fix_foveated_render *ffr)
{

	ffr->target.image = U_TYPED_ARRAY_CALLOC(VkImage, ffr->target.count);
	ffr->target.memory = U_TYPED_ARRAY_CALLOC(VkDeviceMemory, ffr->target.count);
	ffr->target.view = U_TYPED_ARRAY_CALLOC(VkImageView, ffr->target.count);
	
	for(int i = 0; i < ffr->target.count; i ++)
	{
		VkImageCreateInfo imageInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.extent.width = ffr->width,
			.extent.height = ffr->height,
			.extent.depth = 1,
			.mipLevels = 1,
			.arrayLayers = 1,
			.format = ffr->format,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		if (vk->vkCreateImage(vk->device, &imageInfo, NULL, &ffr->target.image[i]) != VK_SUCCESS){
			VK_ERROR(vk, "vkCreateImage failed");
		}

		VkMemoryRequirements memRequirements;
		vk->vkGetImageMemoryRequirements(vk->device, ffr->target.image[i], &memRequirements);

		VkMemoryAllocateInfo allocInfo = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = memRequirements.size,
			.memoryTypeIndex = find_memory_type(vk, memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
		};

		vk->vkAllocateMemory(vk->device, &allocInfo, NULL, &ffr->target.memory[i]);
		vk->vkBindImageMemory(vk->device, ffr->target.image[i], ffr->target.memory[i], 0);

		VkImageSubresourceRange subresource_range = {
		    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		    .baseMipLevel = 0,
		    .levelCount = 1,
		    .baseArrayLayer = 0,
		    .layerCount = 1,
		};

		VkImageViewCreateInfo viewInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = ffr->target.image[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = ffr->format,
			.subresourceRange = subresource_range,
		};
			
		if (vk->vkCreateImageView(vk->device, &viewInfo, NULL, &ffr->target.view[i]) != VK_SUCCESS) 
		{
			VK_ERROR(vk, "vkCreateImageView failed");
		}
	}
}


static bool 
ffr_create_buffer(struct vk_bundle *vk, 						
					    struct vk_buffer *buffer,
						VkBufferUsageFlags usage)
{
	VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	if (!vk_buffer_init(vk, buffer->size, usage, properties, &buffer->handle, &buffer->memory))
	{
		return false;
	}
	return vk_update_buffer(vk, buffer->data, buffer->size, buffer->memory);
}

static void 
ffr_create_commandbuffer(struct vk_bundle *vk, VkCommandPool *commandpool, VkCommandBuffer *commandbuffer)
{
	VkResult ret = VK_SUCCESS;
	VkCommandPoolCreateInfo poolInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = vk->queue_family_index,
		.flags = 0,//VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};

	ret = vk->vkCreateCommandPool(vk->device, &poolInfo, VK_NULL_HANDLE, commandpool); 
	if(ret != VK_SUCCESS)
	{
		VK_ERROR(vk, "vkCreateCommandPool failed");
	}

	VkCommandBufferAllocateInfo allocInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = *commandpool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};

	ret = vk->vkAllocateCommandBuffers(vk->device, &allocInfo, commandbuffer);
	if(ret != VK_SUCCESS)
	{
		VK_ERROR(vk, "vkAllocateCommandBuffers failed: %s", vk_result_string(ret));
	}

}

static void 
ffr_record_command(struct vk_bundle *vk, struct fix_foveated_render *ffr)
{
	VkResult ret = VK_SUCCESS;
	VkCommandBufferBeginInfo beginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	ret = vk->vkBeginCommandBuffer(ffr->mesh.commandbuffer, &beginInfo);
	if(ret != VK_SUCCESS)
	{
		VK_ERROR(vk, "vkBeginCommandBuffer failed: %s", vk_result_string(ret));
	}

	VkClearValue clear_color[1] = {{
	    .color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}},
	}};
		
	VkRenderPassBeginInfo renderPassInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = ffr->mesh.render_pass,
		.framebuffer = ffr->target.framebuffer[ffr->target.index],
		.renderArea = 
			{
				.offset = 
					{ 
						.x = 0,
						.y = 0,
					},
				.extent =
	                {
	                    .width = ffr->width,
	                    .height = ffr->height,
	                },
		 	},
		 .clearValueCount = ARRAY_SIZE(clear_color),
	     .pClearValues = clear_color,
	};

	
	vk->vkCmdBeginRenderPass(ffr->mesh.commandbuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
	VkViewport viewport = {
	    .x = (float)0,
	    .y = (float)0,
	    .width = (float)ffr->width,
	    .height = (float)ffr->height,
	    .minDepth = 0.0f,
	    .maxDepth = 1.0f,
	};

	vk->vkCmdSetViewport(ffr->mesh.commandbuffer, // commandBuffer
		                     0,          // firstViewport
		                     1,          // viewportCount
		                     &viewport); // pViewports

	/*
	 * Scissor
	 */

	VkRect2D scissor = {
	    .offset =
	        {
	            .x = 0,
	            .y = 0,
	        },
	    .extent =
	        {
	            .width = ffr->width,
	            .height = ffr->height,
	        },
	};

	vk->vkCmdSetScissor(ffr->mesh.commandbuffer, // commandBuffer
	                    0,          // firstScissor
	                    1,          // scissorCount
	                    &scissor);  // pScissors
	vk->vkCmdBindPipeline(ffr->mesh.commandbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ffr->mesh.pipeline);

	VkBuffer vertexBuffers[] = { ffr->mesh.vbo.handle };
	VkDeviceSize offsets[] = { 0 };
	vk->vkCmdBindVertexBuffers(ffr->mesh.commandbuffer, 0, 1, vertexBuffers, offsets);
	vk->vkCmdBindIndexBuffer(ffr->mesh.commandbuffer, ffr->mesh.ibo.handle, 0, VK_INDEX_TYPE_UINT32);
	vk->vkCmdBindDescriptorSets(ffr->mesh.commandbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ffr->mesh.pipeline_layout, 0, 1, &ffr->mesh.descriptor_set, 0, NULL);
	vk->vkCmdDrawIndexed(ffr->mesh.commandbuffer, 6, 1, 0, 0, 0);
	vk->vkCmdEndRenderPass(ffr->mesh.commandbuffer);
	ret = vk->vkEndCommandBuffer(ffr->mesh.commandbuffer);
	if(ret != VK_SUCCESS)
	{
		VK_ERROR(vk, "vkEndCommandBuffer failed: %s", vk_result_string(ret));
	}
}

static void 
ffr_create_descriptor_set_layout(struct vk_bundle *vk,
                                  uint32_t src_binding,
                                  uint32_t ubo_binding,
                                  VkDescriptorSetLayout *out_descriptor_set_layout)
{
	VkResult ret;

	VkDescriptorSetLayoutBinding set_layout_bindings[2] = {
	    {
	        .binding = src_binding,
	        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .descriptorCount = 1,
	        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	    },
	    {
	        .binding = ubo_binding,
	        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	        .descriptorCount = 1,
	        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	    },
	};

	VkDescriptorSetLayoutCreateInfo set_layout_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = ARRAY_SIZE(set_layout_bindings),
	    .pBindings = set_layout_bindings,
	};

	VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
	ret = vk->vkCreateDescriptorSetLayout(vk->device,              //
	                                      &set_layout_info,        //
	                                      NULL,                    //
	                                      &descriptor_set_layout); //
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreateDescriptorSetLayout failed: %s", vk_result_string(ret));
	}

	*out_descriptor_set_layout = descriptor_set_layout;
}

static void 
ffr_create_fance(struct vk_bundle *vk, VkFence *out_render_fence)
{
	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = 0,
	};

	if (vk->vkCreateFence(vk->device, &fenceInfo, NULL, out_render_fence) != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreateFence failed");
	}	
}
                               
static void 
ffr_create_render_pass(struct vk_bundle *vk, 
								 VkFormat format, 
								 VkRenderPass *out_render_pass)
{
	VkResult ret;

	VkAttachmentDescription attachments[1] = {
	    {
	        .format = format,
	        .samples = VK_SAMPLE_COUNT_1_BIT,
	        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
	        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	    },
	};

	VkAttachmentReference color_reference = {
	    .attachment = 0,
	    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	VkSubpassDescription subpasses[1] = {
	    {
	        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	        .inputAttachmentCount = 0,
	        .pInputAttachments = NULL,
	        .colorAttachmentCount = 1,
	        .pColorAttachments = &color_reference,
	        .pResolveAttachments = NULL,
	        .pDepthStencilAttachment = NULL,
	        .preserveAttachmentCount = 0,
	        .pPreserveAttachments = NULL,
	    },
	};

	VkSubpassDependency dependencies[1] = {
	    {
	        .srcSubpass = VK_SUBPASS_EXTERNAL,
	        .dstSubpass = 0,
	        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	        .srcAccessMask = 0,
	        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    },
	};

	VkRenderPassCreateInfo render_pass_info = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = ARRAY_SIZE(attachments),
	    .pAttachments = attachments,
	    .subpassCount = ARRAY_SIZE(subpasses),
	    .pSubpasses = subpasses,
	    .dependencyCount = ARRAY_SIZE(dependencies),
	    .pDependencies = dependencies,
	};

	VkRenderPass render_pass = VK_NULL_HANDLE;
	ret = vk->vkCreateRenderPass(vk->device,        //
	                             &render_pass_info, //
	                             NULL,              //
	                             &render_pass);     //
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreateRenderPass failed: %s", vk_result_string(ret));
	}

	*out_render_pass = render_pass;
}

static void 
ffr_create_pipeline(struct vk_bundle *vk,
                     VkRenderPass render_pass,
                     VkDescriptorSetLayout descriptor_set_layout,
                     uint32_t src_binding,
                     uint32_t mesh_index_count_total,
                     uint32_t mesh_stride,
                     VkShaderModule mesh_vert,
                     VkShaderModule mesh_frag,
                     VkPipelineCache *out_pipeline_cache,
                     VkPipelineLayout *out_pipeline_layout,
                     VkPipeline *out_mesh_pipeline)
{
	VkResult ret;

	/*create piprline layout*/
	VkPipelineLayoutCreateInfo pipeline_layout_info = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &descriptor_set_layout,
	};

	ret = vk->vkCreatePipelineLayout( 	//
	    vk->device,                   	// device
	    &pipeline_layout_info,        	// pCreateInfo
	    NULL,                         	// pAllocator
	    out_pipeline_layout);          // pPipelineLayout
	    
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreatePipelineLayout failed: %s", vk_result_string(ret));
		return;
	}

	/*create pipeline cache*/
	VkPipelineCacheCreateInfo pipeline_cache_info = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
	};

	ret = vk->vkCreatePipelineCache( 	//
	    vk->device,                  	// device
	    &pipeline_cache_info,        	// pCreateInfo
	    NULL,                        	// pAllocator
	    out_pipeline_cache);           // pPipelineCache
	    
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreatePipelineCache failed: %s", vk_result_string(ret));
		return;
	}

	/*create pipeline*/
	// Might be changed to line for debugging.
	VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;

	// Do we use triangle strips or triangles with indices.
	VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	if (mesh_index_count_total > 0) {
		topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	}

	VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = topology,
	    .primitiveRestartEnable = VK_FALSE,
	};

	VkPipelineRasterizationStateCreateInfo rasterization_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .depthClampEnable = VK_FALSE,
	    .rasterizerDiscardEnable = VK_FALSE,
	    .polygonMode = polygonMode,
	    .cullMode = VK_CULL_MODE_BACK_BIT,
	    .frontFace = VK_FRONT_FACE_CLOCKWISE,
	    .lineWidth = 1.0f,
	};

	VkPipelineColorBlendAttachmentState blend_attachment_state = {
	    .blendEnable = VK_FALSE,
	    .colorWriteMask = 0xf,
	};

	VkPipelineColorBlendStateCreateInfo color_blend_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &blend_attachment_state,
	};

	VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
	    .depthTestEnable = VK_TRUE,
	    .depthWriteEnable = VK_TRUE,
	    .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
	    .front =
	        {
	            .compareOp = VK_COMPARE_OP_ALWAYS,
	        },
	    .back =
	        {
	            .compareOp = VK_COMPARE_OP_ALWAYS,
	        },
	};

	VkPipelineViewportStateCreateInfo viewport_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1,
	    .scissorCount = 1,
	};

	VkPipelineMultisampleStateCreateInfo multisample_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	VkDynamicState dynamic_states[] = {
	    VK_DYNAMIC_STATE_VIEWPORT,
	    VK_DYNAMIC_STATE_SCISSOR,
	};

	VkPipelineDynamicStateCreateInfo dynamic_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = ARRAY_SIZE(dynamic_states),
	    .pDynamicStates = dynamic_states,
	};

	// clang-format off
	VkVertexInputAttributeDescription vertex_input_attribute_descriptions[2] = {
	    {
	        .binding = src_binding,
	        .location = 0,
	        .format = VK_FORMAT_R32G32B32A32_SFLOAT,
	        .offset = 0,
	    },
	    {
	        .binding = src_binding,
	        .location = 1,
	        .format = VK_FORMAT_R32G32B32A32_SFLOAT,
	        .offset = sizeof(float) * 3,
	    },
	};

	VkVertexInputBindingDescription vertex_input_binding_description[1] = {
	    {
	        .binding = src_binding,
	        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	        .stride = mesh_stride,
	    },
	};

	VkPipelineVertexInputStateCreateInfo vertex_input_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	    .vertexAttributeDescriptionCount = ARRAY_SIZE(vertex_input_attribute_descriptions),
	    .pVertexAttributeDescriptions = vertex_input_attribute_descriptions,
	    .vertexBindingDescriptionCount = ARRAY_SIZE(vertex_input_binding_description),
	    .pVertexBindingDescriptions = vertex_input_binding_description,
	};
	// clang-format on

	VkPipelineShaderStageCreateInfo shader_stages[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_VERTEX_BIT,
	        .module = mesh_vert,
	        .pName = "main",
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	        .module = mesh_frag,
	        .pName = "main",
	    },
	};

	VkGraphicsPipelineCreateInfo pipeline_info = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = ARRAY_SIZE(shader_stages),
	    .pStages = shader_stages,
	    .pVertexInputState = &vertex_input_state,
	    .pInputAssemblyState = &input_assembly_state,
	    .pViewportState = &viewport_state,
	    .pRasterizationState = &rasterization_state,
	    .pMultisampleState = &multisample_state,
	    .pDepthStencilState = &depth_stencil_state,
	    .pColorBlendState = &color_blend_state,
	    .pDynamicState = &dynamic_state,
	    .layout = *out_pipeline_layout,
	    .renderPass = render_pass,
	    .basePipelineHandle = VK_NULL_HANDLE,
	    .basePipelineIndex = -1,
	};

	ret = vk->vkCreateGraphicsPipelines(vk->device,     //
	                                    *out_pipeline_cache, //
	                                    1,              //
	                                    &pipeline_info, //
	                                    NULL,           //
	                                    out_mesh_pipeline);     //
	if (ret != VK_SUCCESS) {
		VK_DEBUG(vk, "vkCreateGraphicsPipelines failed: %s", vk_result_string(ret));
	}
}

static void 
ffr_create_framebuffer(struct vk_bundle *vk, struct fix_foveated_render *ffr)
{
	 VkResult ret;
	 ffr->target.framebuffer = U_TYPED_ARRAY_CALLOC(VkFramebuffer, ffr->target.count);
	 for(int i = 0; i < ffr->target.count; i ++)
	 {

		 VkImageView attachments[1] = {ffr->target.view[i]};

		 VkFramebufferCreateInfo frame_buffer_info = {
			 .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			 .renderPass = ffr->mesh.render_pass,
			 .attachmentCount = ARRAY_SIZE(attachments),
			 .pAttachments = attachments,
			 .width = ffr->width,
			 .height = ffr->height,
			 .layers = 1,
		 };

		 ret = vk->vkCreateFramebuffer(vk->device,		   //
									   &frame_buffer_info, //
									   NULL,			   //
									   &ffr->target.framebuffer[i]);	   //
		 if (ret != VK_SUCCESS) {
			 VK_ERROR(vk, "vkCreateFramebuffer failed: %s", vk_result_string(ret));
			 return;
		 }
	 }
	 VK_INFO(vk, "create framebuffer success");
}

static void 
ffr_submit_queue(struct vk_bundle *vk,
						 VkSemaphore *render_complete_semaphore,
						 VkCommandBuffer *commandbuffer)
{
	VkResult ret = VK_SUCCESS;
	VkFence fence;
	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = 0,
	};

	if (vk->vkCreateFence(vk->device, &fenceInfo, NULL, &fence) != VK_SUCCESS) 
	{
		VK_ERROR(vk, "vkCreateFence failed");
	}	
	
	VkSubmitInfo submitInfo = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = render_complete_semaphore,
			.pWaitDstStageMask = NULL,
			.commandBufferCount = 1,
			.pCommandBuffers = commandbuffer,
			.signalSemaphoreCount = 0,
	};
	ret = vk->vkQueueSubmit(vk->queue, 1, &submitInfo, fence);
	if (ret != VK_SUCCESS) 
	{
		VK_ERROR(vk, "vkQueueSubmit: %s", vk_result_string(ret));
	}

	ret = vk->vkWaitForFences(vk->device, 1, &fence, VK_TRUE, UINT64_MAX);
	if(ret != VK_SUCCESS)
	{
		VK_ERROR(vk, "vkWaitForFences: %s", vk_result_string(ret));
	}

	vk->vkDestroyFence(vk->device, fence, NULL);
}
static void 
ffr_calculate_foveation_vars(struct comp_target *ct, struct fix_foveated_render *ffr)
{
	
	float target_eye_width = ct->width / 2;
	float target_eye_height = ct->height;

	float center_size_x = ffr->ffr_bundle->center_size_x;
	float center_size_y = ffr->ffr_bundle->center_size_y;

	float center_shift_x = ffr->ffr_bundle->center_shift_x;
	float center_shift_y = ffr->ffr_bundle->center_shift_y;

	float edge_ratio_x = ffr->ffr_bundle->edge_ratio_x;
	float edge_ratio_y = ffr->ffr_bundle->edge_ratio_y;

	float edge_size_x = target_eye_width - center_size_x * target_eye_width;
	float edge_size_y = target_eye_height - center_size_y * target_eye_height;

	float center_size_x_aligned = 1.0 - ceil(edge_size_x / (edge_ratio_x * 2.0)) * (edge_ratio_x * 2.0) / target_eye_width;
	float center_size_y_aligned = 1.0 - ceil(edge_size_y / (edge_ratio_y * 2.0)) * (edge_ratio_y * 2.0) / target_eye_height;

	float edge_size_x_aligned = target_eye_width  - center_size_x_aligned * target_eye_width;
	float edge_size_y_aligned = target_eye_height - center_size_y_aligned * target_eye_height;

	float center_shift_x_aligned = ceil(center_shift_x * edge_size_x_aligned / (edge_ratio_x * 2.0)) * (edge_ratio_x * 2.0) / edge_size_x_aligned;
	float center_shift_y_aligned = ceil(center_shift_y * edge_size_y_aligned / (edge_ratio_y * 2.0)) * (edge_ratio_y * 2.0) / edge_size_y_aligned;

	float foveation_scale_x = (center_size_x_aligned + (1.0 - center_size_x_aligned) / edge_ratio_x);
	float foveation_scale_y = (center_size_y_aligned + (1.0 - center_size_y_aligned) / edge_ratio_y);

	float optimized_eye_width = foveation_scale_x * target_eye_width;
	float optimized_eye_height = foveation_scale_y * target_eye_height;

	uint32_t optimized_eye_width_aligned = ceil(optimized_eye_width / 32.0) * 32;
	uint32_t optimized_eye_height_aligned = ceil(optimized_eye_height / 32.0) * 32;

	float eye_width_ratio_aligned = optimized_eye_width / optimized_eye_width_aligned;
	float eye_height_ratio_aligned = optimized_eye_height / optimized_eye_height_aligned;

	ffr->foveation_vars_ubo.target_eye_width = target_eye_width;
	ffr->foveation_vars_ubo.target_eye_height = target_eye_height;

	ffr->foveation_vars_ubo.optimized_eye_width = optimized_eye_width_aligned;
	ffr->foveation_vars_ubo.optimized_eye_height = optimized_eye_height_aligned;

	ffr->foveation_vars_ubo.eye_width_ratio = eye_width_ratio_aligned;
	ffr->foveation_vars_ubo.eye_height_ratio = eye_height_ratio_aligned;

	ffr->foveation_vars_ubo.center_size_x = center_size_x_aligned;
	ffr->foveation_vars_ubo.center_size_y = center_size_y_aligned;

	ffr->foveation_vars_ubo.center_shift_x = center_shift_x_aligned;
	ffr->foveation_vars_ubo.center_shift_y = center_shift_y_aligned;

	ffr->foveation_vars_ubo.edge_ratio_x = edge_ratio_x;
	ffr->foveation_vars_ubo.edge_ratio_y = edge_ratio_y;

	ffr->width = ffr->foveation_vars_ubo.optimized_eye_width * 2;
	ffr->height = ffr->foveation_vars_ubo.optimized_eye_height;

	COMP_INFO(ct->c, "original width = %d, height = %d", ct->width, ct->height);
	COMP_INFO(ct->c, "fix foveation width = %d, height = %d", ffr->width, ffr->height);
}

static void 
ffr_update_descriptor_ubo(struct vk_bundle *vk, 
									  uint32_t ubo_binding, 
									  VkDescriptorSet descriptor_set, 
									  VkBuffer handle)
{
	VkDescriptorBufferInfo buffer_info = {
	    .buffer = handle,
	    .offset = 0,
	    .range = VK_WHOLE_SIZE,
	};

	VkWriteDescriptorSet write_descriptor_sets[1] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = descriptor_set,
	        .dstBinding = ubo_binding,
	        .descriptorCount = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	        .pBufferInfo = &buffer_info,
	    },
	};

	vk->vkUpdateDescriptorSets(vk->device,                        //
	                           ARRAY_SIZE(write_descriptor_sets), // descriptorWriteCount
	                           write_descriptor_sets,             // pDescriptorWrites
	                           0,                                 // descriptorCopyCount
	                           NULL);                             // pDescriptorCopies
}

static void 
ffr_update_descriptor_image(struct vk_bundle *vk, 
										uint32_t src_binding, 
										VkSampler sampler, 
										VkDescriptorSet descriptor_set, 
										VkImageView image_view)
{
	VkResult ret = VK_SUCCESS;
	VkDescriptorImageInfo image_info = {
	    .sampler = sampler,
	    .imageView = image_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	
	VkWriteDescriptorSet write_descriptor_sets[1] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = src_binding,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_info,
			},
		};
	vk->vkUpdateDescriptorSets(vk->device,                        //
	                           ARRAY_SIZE(write_descriptor_sets), // descriptorWriteCount
	                           write_descriptor_sets,             // pDescriptorWrites
	                           0,                                 // descriptorCopyCount
	                           NULL);                             // pDescriptorCopies
}
static void 
ffr_update_discriptor_set(struct vk_bundle *vk, struct fix_foveated_render *ffr, VkImageView image_view)
{
	VkResult ret = VK_SUCCESS;
	VkDescriptorImageInfo image_info = {
	    .sampler = ffr->mesh.sampler,
	    .imageView = image_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorBufferInfo buffer_info = {
	    .buffer = ffr->mesh.ubo.handle,
	    .offset = 0,
	    .range = VK_WHOLE_SIZE,
	};

	VkWriteDescriptorSet write_descriptor_sets[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = ffr->mesh.descriptor_set,
	        .dstBinding = ffr->mesh.src_binding,
	        .descriptorCount = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .pImageInfo = &image_info,
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = ffr->mesh.descriptor_set,
	        .dstBinding = ffr->mesh.ubo_binding,
	        .descriptorCount = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	        .pBufferInfo = &buffer_info,
	    },
	};

	vk->vkUpdateDescriptorSets(vk->device,                        //
	                           ARRAY_SIZE(write_descriptor_sets), // descriptorWriteCount
	                           write_descriptor_sets,             // pDescriptorWrites
	                           0,                                 // descriptorCopyCount
	                           NULL);                             // pDescriptorCopies
}

static bool
ffr_enable(struct comp_target *ct)
{
	struct xrt_device *xdev = ct->c->xdev;
	if(xdev->ffr_bundle.ffr_enable)
	{
		return xdev->ffr_bundle.ffr_enable(xdev);
	}
	return false;
}

static 	void 
ffr_create_descriptor_set(struct vk_bundle *vk,
                                  uint32_t src_binding,
                                  uint32_t ubo_binding,
                                  VkDescriptorPool *out_descriptor_pool,
                                  VkDescriptorSetLayout *out_descriptor_set_layout,
                                  VkDescriptorSet *out_descriptor_set
                                  )
{
	VkResult ret;
	/*create descriptor tool*/
	struct vk_descriptor_pool_info mesh_pool_info = {
	    .uniform_per_descriptor_count = 1,
	    .sampler_per_descriptor_count = 1,
	    .storage_image_per_descriptor_count = 0,
	    .storage_buffer_per_descriptor_count = 0,
	    .descriptor_count = 1,
	    .freeable = false,
	};
	ret = vk_create_descriptor_pool(    			     //
							     vk,                     // vk_bundle
							     &mesh_pool_info,        // info
							     out_descriptor_pool); 	 // out_descriptor_pool
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreateDescriptorPool failed: %s", vk_result_string(ret));
	}
	/*create descriptor layout*/				     
	VkDescriptorSetLayoutBinding set_layout_bindings[2] = {
	    {
	        .binding = src_binding,
	        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .descriptorCount = 1,
	        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	    },
	    {
	        .binding = ubo_binding,
	        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	        .descriptorCount = 1,
	        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	    },
	};

	VkDescriptorSetLayoutCreateInfo set_layout_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = ARRAY_SIZE(set_layout_bindings),
	    .pBindings = set_layout_bindings,
	};

	VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
	ret = vk->vkCreateDescriptorSetLayout(vk->device,              //
	                                      &set_layout_info,        //
	                                      NULL,                    //
	                                      out_descriptor_set_layout); //
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreateDescriptorSetLayout failed: %s", vk_result_string(ret));
	}

	/*create descriptor set*/
	ret = vk_create_descriptor_set(vk,                             // vk_bundle
						     	*out_descriptor_pool,            // descriptor_pool
						     	*out_descriptor_set_layout,      // descriptor_set_layout
						     	out_descriptor_set);  			 // descriptor_set	
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreateDescriptorSet failed: %s", vk_result_string(ret));
	}

}
static void 
comp_target_save_image(struct comp_target *ct)
{
	struct comp_target_swapchain *cts = (struct comp_target_swapchain *)ct;
	struct comp_window_offscreen *cwo = (struct comp_window_offscreen*)ct;
	struct fix_foveated_render *ffr = &cwo->ffr;
	struct vk_bundle *vk = get_vk(cts);

	static uint32_t num = 0;
	uint32_t imagebytes = ffr->width * ffr->height * 4;
	char *imagedata = NULL;
	char chach_path[64];
	FILE *fd = NULL;
	sprintf(chach_path, "%s%d%s", "D:\\video\\chache", num, ".rgba");
	
	fd = fopen(chach_path, "wb+");
	if(fd == NULL)
	{
		COMP_INFO(ct->c, "open file failed when save image");
	}
	
	VkImageSubresource subResource = { 
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	};
	VkSubresourceLayout subResourceLayout;
		
	vk->vkGetImageSubresourceLayout(vk->device, ffr->target.image, &subResource, &subResourceLayout);
	vk->vkMapMemory(vk->device, ffr->target.memory, 0, VK_WHOLE_SIZE, 0, (void **)&imagedata);
	imagedata += subResourceLayout.offset;

	COMP_INFO(ct->c, "addr = %p, offset = %d", imagedata, subResourceLayout.offset);

	//if(subResourceLayout.rowPitch == ffr->width * 4)
	{		
		fwrite(imagedata, 1, imagebytes, fd);	
	}
	//else
	{
	//	COMP_INFO(ct->c, "shit");
	}
	vk->vkUnmapMemory(vk->device, ffr->target.memory);
	fclose(fd);
	num ++;
}

static bool
comp_target_swapchain_check_ready(struct comp_target *ct)
{
	struct comp_target_swapchain *cts = (struct comp_target_swapchain*)ct;

	for(int i = 0; i < cts->base.image_count; i ++)
	{
		if(cts->base.images[i].handle == VK_NULL_HANDLE || cts->base.images[i].view == VK_NULL_HANDLE)
		{
			return false;
		}
	}
	//COMP_INFO(ct->c, "images ready");
	return true;
}

static bool
comp_target_swapchain_has_images(struct comp_target *ct)
{
	struct comp_target_swapchain *cts = (struct comp_target_swapchain *)ct;
	for(int i = 0; i < cts->base.image_count; i ++)
	{
		if(cts->base.images[i].handle == VK_NULL_HANDLE || cts->base.images[i].view == VK_NULL_HANDLE)
		{
			return false;
		}
	}
	//COMP_INFO(ct->c, "swapchain has images");
	return true;
}

uint64_t timestamp[2];
static VkResult
comp_target_swapchain_acquire_next_image(struct comp_target *ct, uint32_t *out_index)
{
	VkResult ret = VK_SUCCESS;
	struct comp_window_offscreen *w = (struct comp_window_offscreen *)ct;
	struct comp_target_swapchain *cts = (struct comp_target_swapchain *)ct;
	struct vk_bundle *vk = get_vk(cts);
	VkImage image;
	uint64_t time_stamp;
	if (!comp_target_swapchain_has_images(ct)) {
		//! @todo what error to return here?
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	//TODO add semaphonres
	*out_index = w->image_index;
	
#if 0
	VkSemaphoreSignalInfo signalInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
		.pNext = NULL,
		.semaphore = cts->base.semaphores.present_complete,
		.value = cts->current_frame_id,
	};

	ret = vk->vkSignalSemaphore(vk->device, &signalInfo);
	if(ret != VK_SUCCESS){
		COMP_INFO(ct->c, "vkSignalSemaphore failed");
	}
#endif
	return ret;
}

static VkResult
comp_target_swapchain_present(struct comp_target *ct,
                              VkQueue queue,
                              uint32_t index,
                              uint64_t timeline_semaphore_value,
                              uint64_t desired_present_time_ns,
                              uint64_t present_slop_ns)
{
	VkResult ret = VK_SUCCESS;
	struct comp_window_offscreen *cwo = (struct comp_window_offscreen *)ct;
	struct comp_target_swapchain *cts = (struct comp_target_swapchain *)ct;
	struct fix_foveated_render	*ffr = (struct fix_foveated_render *)&cwo->ffr;
	struct vk_bundle *vk = get_vk(cts);
	VkImage image = cts->base.images[index].handle;
	if(ffr->initialized)
	{	
		ffr_update_descriptor_image(vk, 
									ffr->mesh.src_binding, 
									ffr->mesh.sampler, 
									ffr->mesh.descriptor_set, 
									cts->base.images[index].view);
		ffr_record_command(vk, ffr);
		ffr_submit_queue(vk, 
						 &ct->semaphores.render_complete, 
						 &ffr->mesh.commandbuffer);	
		image = ffr->target.image[ffr->target.index];
		ffr->target.index = (ffr->target.index + 1) % ffr->target.count;
	}
	
	cwo->image_index = (cwo->image_index + 1) % cwo->image_count;
	
	struct xrt_device *xdev = ct->c->xdev;
	if(xdev->streaming_check_resolution)
	{		
		uint32_t *width = &ct->c->settings.preferred.width;
		uint32_t *height = &ct->c->settings.preferred.height;
		ret = (xdev->streaming_check_resolution(xdev, width, height)) ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUCCESS;
		if(ret != VK_SUCCESS)
		{
			return ret;
		}
	}	
	if(xdev->streaming_onframe)
	{
		xdev->streaming_onframe(xdev, image, desired_present_time_ns);
	}
	return ret;
}

static void
comp_target_swapchain_calc_frame_pacing(struct comp_target *ct,
                                        int64_t *out_frame_id,
                                        uint64_t *out_wake_up_time_ns,
                                        uint64_t *out_desired_present_time_ns,
                                        uint64_t *out_present_slop_ns,
                                        uint64_t *out_predicted_display_time_ns)
{
	struct comp_target_swapchain *cts = (struct comp_target_swapchain *)ct;

	int64_t frame_id = -1;
	uint64_t wake_up_time_ns = 0;
	uint64_t desired_present_time_ns = 0;
	uint64_t present_slop_ns = 0;
	uint64_t predicted_display_time_ns = 0;
	uint64_t predicted_display_period_ns = 0;
	uint64_t min_display_period_ns = 0;
	uint64_t now_ns = os_monotonic_get_ns();

	u_pc_predict(cts->upc,                     //
	             now_ns,                       //
	             &frame_id,                    //
	             &wake_up_time_ns,             //
	             &desired_present_time_ns,     //
	             &present_slop_ns,             //
	             &predicted_display_time_ns,   //
	             &predicted_display_period_ns, //
	             &min_display_period_ns);      //

	cts->current_frame_id = frame_id;

	*out_frame_id = frame_id;
	*out_wake_up_time_ns = wake_up_time_ns;
	*out_desired_present_time_ns = desired_present_time_ns;
	*out_predicted_display_time_ns = predicted_display_time_ns;
	*out_present_slop_ns = present_slop_ns;
}

static void
comp_target_swapchain_create_images(struct comp_target *ct,
									uint32_t preferred_width,
									uint32_t preferred_height,
									VkFormat color_format,
									VkColorSpaceKHR color_space,
									VkImageUsageFlags image_usage,
									VkPresentModeKHR present_mode)

{
	VkResult ret;
	struct comp_target_swapchain *cts = (struct comp_target_swapchain *)ct;
	struct comp_window_offscreen *w = (struct comp_window_offscreen*)ct;
	struct vk_bundle *vk = get_vk(cts);

	uint64_t now_ns = os_monotonic_get_ns();
	// Some platforms really don't like the pacing_compositor code.
	bool use_display_timing_if_available = cts->timing_usage == COMP_TARGET_USE_DISPLAY_IF_AVAILABLE;
	if (cts->upc == NULL && use_display_timing_if_available && vk->has_GOOGLE_display_timing) {
		u_pc_display_timing_create(ct->c->settings.nominal_frame_interval_ns,
		                           &U_PC_DISPLAY_TIMING_CONFIG_DEFAULT, &cts->upc);
		COMP_INFO(ct->c, "create display-timing upc");
	} else if (cts->upc == NULL) {
		u_pc_fake_create(ct->c->settings.nominal_frame_interval_ns, now_ns, &cts->upc);
		COMP_INFO(ct->c, "create fake upc");
	}
	
	destroy_image_views(cts);
	destroy_image_memory(cts);
	target_init_semaphores(cts);
	
	cts->base.image_count = w->image_count;
	cts->base.images = U_TYPED_ARRAY_CALLOC(struct comp_target_image, cts->base.image_count);
	assert(cts->base.images != NULL);

	for(int i = 0; i < cts->base.image_count; i ++)
	{
		VkImageCreateInfo imageInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.extent.width = preferred_width,
			.extent.height = preferred_height,
			.extent.depth = 1,
			.mipLevels = 1,
			.arrayLayers = 1,
			.format = color_format,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,	//image_usage,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		COMP_INFO(cts->base.c, "Image usage: local = 0x%x, required = 0x%x", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, image_usage);
		COMP_INFO(cts->base.c, "Color format:local = %d,   required   = %d", VK_FORMAT_R8G8B8A8_SRGB, color_format);
		ret = vk->vkCreateImage(vk->device, &imageInfo, NULL, &cts->base.images[i].handle);
		if(ret != VK_SUCCESS)
		{
			COMP_ERROR(cts->base.c, "vkCreateImage : %s", vk_result_string(ret));
			return;
		}
	}
	cts->base.width = preferred_width;
	cts->base.height = preferred_height;
	cts->base.format = color_format;

	create_image_memory(cts);
	create_image_view(cts);

	if(!ffr_enable(ct))
	{
		//TODO
		return;	
	}
	w->ffr.format = color_format;
	ffr_calculate_foveation_vars(ct, &w->ffr);
	ffr_create_images(vk, &w->ffr);
	if(ct->c->xdev->streaming_update_resolution)
	{
		ct->c->xdev->streaming_update_resolution(ct->c->xdev, w->ffr.width, w->ffr.height);
	}

	ffr_create_framebuffer(vk, &w->ffr);
	ffr_create_buffer(vk, &w->ffr.mesh.ubo, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

	ffr_update_descriptor_ubo(vk, 
							  w->ffr.mesh.ubo_binding, 
							  w->ffr.mesh.descriptor_set, 
							  w->ffr.mesh.ubo.handle);
	ffr_update_descriptor_image(vk, 
								w->ffr.mesh.src_binding, 
								w->ffr.mesh.sampler, 
								w->ffr.mesh.descriptor_set, 
								cts->base.images[0].view);
	
	ffr_record_command(vk, &w->ffr);	
	w->ffr.initialized = true;
}
static void
comp_target_swapchain_mark_timing_point(struct comp_target *ct,
									 enum comp_target_timing_point point,
									 int64_t frame_id,
									 uint64_t when_ns)
{

   struct comp_target_swapchain *cts = (struct comp_target_swapchain *)ct;
   assert(frame_id == cts->current_frame_id);
   switch (point) {
   case COMP_TARGET_TIMING_POINT_WAKE_UP:
	   u_pc_mark_point(cts->upc, U_TIMING_POINT_WAKE_UP, cts->current_frame_id, when_ns);
	   break;
   case COMP_TARGET_TIMING_POINT_BEGIN:
	   u_pc_mark_point(cts->upc, U_TIMING_POINT_BEGIN, cts->current_frame_id, when_ns);
	   break;
   case COMP_TARGET_TIMING_POINT_SUBMIT:
	   u_pc_mark_point(cts->upc, U_TIMING_POINT_SUBMIT, cts->current_frame_id, when_ns);
	   break;
   default: assert(false);
   }
}

static VkResult
comp_target_swapchain_update_timings(struct comp_target *ct)
{
	 COMP_TRACE_MARKER();

	 struct comp_target_swapchain *cts = (struct comp_target_swapchain *)ct;
	 do_update_timings_google_display_timing(cts);
	 do_update_timings_vblank_thread(cts);
	 return VK_SUCCESS;
}

static void
comp_target_swapchain_info_gpu(
    struct comp_target *ct, int64_t frame_id, uint64_t gpu_start_ns, uint64_t gpu_end_ns, uint64_t when_ns)
{
	COMP_TRACE_MARKER();

	struct comp_target_swapchain *cts = (struct comp_target_swapchain *)ct;

	u_pc_info_gpu(cts->upc, frame_id, gpu_start_ns, gpu_end_ns, when_ns);
}

static void
comp_target_swapchain_cleanup(struct comp_target_swapchain *cts)
{
	struct vk_bundle *vk = get_vk(cts);

	// Thread if it has been started must be stopped first.
	if (cts->vblank.has_started) {
		// Destroy also stops the thread.
		os_thread_helper_destroy(&cts->vblank.event_thread);
		cts->vblank.has_started = false;
	}

	destroy_image_views(cts);

	destroy_image_memory(cts);

	target_fini_semaphores(cts);

	u_pc_destroy(&cts->upc);
}


static void
comp_target_swapchain_init_and_set_fnptrs(struct comp_target_swapchain *cts,
										  enum comp_target_display_timing_usage timing_usage)
{
	cts->timing_usage = timing_usage;
	cts->base.check_ready 		= comp_target_swapchain_check_ready;
	cts->base.create_images 	= comp_target_swapchain_create_images;
	cts->base.has_images 		= comp_target_swapchain_has_images;
	cts->base.acquire 			= comp_target_swapchain_acquire_next_image;
	cts->base.calc_frame_pacing = comp_target_swapchain_calc_frame_pacing;
	cts->base.mark_timing_point = comp_target_swapchain_mark_timing_point;
	cts->base.update_timings 	= comp_target_swapchain_update_timings;
	cts->base.info_gpu 			= comp_target_swapchain_info_gpu;
	cts->base.present			= comp_target_swapchain_present;

	os_thread_helper_init(&cts->vblank.event_thread);
}

static void 
comp_window_frame_deliver_loop(void *ptr)
{
	struct comp_window_offscreen *cwo = (struct comp_window_offscreen*)ptr;
	struct vk_bundle *vk = get_vk(cwo);
	while(os_thread_helper_is_running(&cwo->swaipchain.oth))
	{
		COMP_INFO(cwo->base.base.c, "deliver loop");
	}

}

static void
comp_window_offscreen_destroy(struct comp_target *ct)
{
	struct comp_window_offscreen *cwo = (struct comp_window_offscreen *)ct;

	// Stop the Windows thread first, destroy also stops the thread.

	comp_target_swapchain_cleanup(&cwo->base);

	//! @todo

	free(ct);
}

static void
comp_window_offscreen_flush(struct comp_target *ct)
{
	//TODO
	//COMP_INFO(ct->c, "do nothing");
}

static bool
comp_window_offscreen_init_pre_vulkan(struct comp_target *ct)
{
	struct comp_window_offscreen *cwo = (struct comp_window_offscreen*)ct;
	struct fix_foveated_render *ffr = &cwo->ffr; 
	
	ffr->initialized = false;
	ffr->width  = ct->c->settings.preferred.width;	
	ffr->height = ct->c->settings.preferred.height;
	ffr->format = ct->c->settings.color_format;

	ffr->target.count = 2;
	ffr->target.index = 0;
	
	ffr->mesh.src_binding = 0;
	ffr->mesh.ubo_binding = 1;
	ffr->mesh.index_count_total = ARRAY_SIZE(indices);
	ffr->mesh.stride = sizeof(float) * 5;

	ffr->mesh.vbo.size =sizeof(vertices);
	ffr->mesh.vbo.data = (void *)vertices;

	ffr->mesh.ibo.size = sizeof(indices);
	ffr->mesh.ibo.data = (void *)indices;

	ffr->mesh.ubo.size = sizeof(struct foveation_vars);
	ffr->mesh.ubo.data = (void *)&ffr->foveation_vars_ubo;

	ffr->ffr_bundle = &ct->c->xdev->ffr_bundle;

	/*init custom defined swaipchain*/
	os_thread_helper_init(&cwo->swaipchain.oth);
	return true;
}

static bool
comp_window_offscreen_init_post_vulkan(struct comp_target *ct, uint32_t prefered_width, uint32_t prefered_height)
{
	//TODO
	
	struct comp_target_swapchain *cts = (struct comp_target_swapchain *)ct;
	struct comp_window_offscreen *cwo = (struct comp_window_offscreen*)ct;
	struct vk_bundle *vk = get_vk(cts);
	struct fix_foveated_render *ffr = &cwo->ffr;
		
	ffr->shaders.mesh_vert = ffr_create_shader_module(vk, ffr->ffr_bundle->vert_shader);
	ffr->shaders.mesh_frag = ffr_create_shader_module(vk, ffr->ffr_bundle->frag_shader);


	ffr_create_buffer(vk, &ffr->mesh.vbo, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	ffr_create_buffer(vk, &ffr->mesh.ibo, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	
	COMP_INFO(ct->c, "ffr create command buffer.");

	ffr_create_commandbuffer(vk, &ffr->mesh.commandpool, &ffr->mesh.commandbuffer);
	vk_create_sampler(vk,                                       // vk_bundle
				      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,  // clamp_mode
				      &ffr->mesh.sampler);     					// out_sampler
				      
	COMP_INFO(ct->c, "ffr create render pass.");	
	ffr_create_render_pass(vk, 
						   ffr->format, 
						   &ffr->mesh.render_pass);
	ffr_create_descriptor_set(vk, 
							  ffr->mesh.src_binding, 
							  ffr->mesh.ubo_binding, 
							  &ffr->mesh.descriptor_pool, 
							  &ffr->mesh.descriptor_set_layout, 
							  &ffr->mesh.descriptor_set);
							  
	COMP_INFO(ct->c, "ffr create pipeline.");
	ffr_create_pipeline(vk, 
						 ffr->mesh.render_pass, 
						 ffr->mesh.descriptor_set_layout, 
						 ffr->mesh.src_binding, 
						 ffr->mesh.index_count_total,
						 ffr->mesh.stride,
						 ffr->shaders.mesh_vert,
						 ffr->shaders.mesh_frag,
						 &ffr->mesh.pipeline_cache,
						 &ffr->mesh.pipeline_layout,
						 &ffr->mesh.pipeline);
	
	ffr_create_fance(vk, &ffr->mesh.renderfence);

	/*start frame deliver thread*/
	//int ret = os_thread_helper_start(&cwo->swaipchain.oth, comp_window_frame_deliver_loop, cwo);
	//COMP_INFO(ct->c, "os thread helper start ret = %d", ret);
	return true;
}

static void
comp_window_offscreen_update_window_title(struct comp_target *ct, const char *title)
{


}

static bool
comp_window_offscreen_configure_check_ready(struct comp_target *ct)
{
	return true;
}


struct comp_target*
comp_window_offscreen_create(struct comp_compositor *c)
{
	struct comp_window_offscreen *w = U_TYPED_CALLOC(struct comp_window_offscreen);
	comp_target_swapchain_init_and_set_fnptrs(&w->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);
	w->image_index = 0;
	w->image_count = 2;
	
	w->base.base.name = "Offline";
	w->base.display = VK_NULL_HANDLE;
	w->base.base.destroy = comp_window_offscreen_destroy;
	w->base.base.flush = comp_window_offscreen_flush;
	w->base.base.init_pre_vulkan = comp_window_offscreen_init_pre_vulkan;
	w->base.base.init_post_vulkan = comp_window_offscreen_init_post_vulkan;
	w->base.base.set_title = comp_window_offscreen_update_window_title;
#ifdef ALLOW_CLOSING_WINDOW
	w->base.base.check_ready = comp_window_mswin_configure_check_ready;
#endif
	w->base.base.c = c;
	COMP_INFO(c, "comp window offscreen create");
	
	return &w->base.base;
}

static bool
detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	return true;
}

static bool
create_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	struct comp_target *ct = comp_window_offscreen_create(c);
	if(ct == NULL)
	{
		return false;
	}
	*out_ct = ct;
	return true;
}


const struct comp_target_factory comp_target_factory_offscreen = {
    .name = "OffScreen",
    .identifier = "mswin",
    .requires_vulkan_for_create = false,
    .is_deferred = true,
    .required_instance_extensions = NULL,
    .required_instance_extension_count = 0,
    .detect = detect,
    .create_target = create_target,
};

