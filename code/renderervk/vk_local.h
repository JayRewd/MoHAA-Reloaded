/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// vk_local.h -- definitions shared by the Vulkan backend files (vk_*.c)
//
// The backend implements the interface in vk.h:
//  - vk_instance.c: loader, instance, device, swapchain
//  - vk.c: frames, command buffers, render state, drawing, clears, readback
//  - vk_image.c: textures, samplers, uploads
//  - vk_pipeline.c: pipeline cache
//  - vk_shaders.c: SPIR-V generated from shaders/ by shaders/compile_shaders.sh
//
// Vulkan is loaded through SDL (SDL_Vulkan_GetVkGetInstanceProcAddr), so the
// renderer does not link against the Vulkan loader; every Vulkan function is
// called through the qvk* pointers declared here.

#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "tr_local.h"

#define VK_MAX_FRAMES				2		// frames in flight
#define VK_MAX_SWAPCHAIN_IMAGES		16
#define VK_MAX_TEXTURES				4096	// live texture handles

/*
====================================================================

FUNCTION POINTERS

====================================================================
*/

// loaded with vkGetInstanceProcAddr( NULL, ... )
#define VK_GLOBAL_FUNCTIONS( X ) \
	X( vkCreateInstance ) \
	X( vkEnumerateInstanceExtensionProperties ) \
	X( vkEnumerateInstanceLayerProperties )

// loaded with vkGetInstanceProcAddr( instance, ... )
#define VK_INSTANCE_FUNCTIONS( X ) \
	X( vkCreateDevice ) \
	X( vkDestroyInstance ) \
	X( vkDestroySurfaceKHR ) \
	X( vkEnumerateDeviceExtensionProperties ) \
	X( vkEnumeratePhysicalDevices ) \
	X( vkGetDeviceProcAddr ) \
	X( vkGetPhysicalDeviceFeatures ) \
	X( vkGetPhysicalDeviceFormatProperties ) \
	X( vkGetPhysicalDeviceMemoryProperties ) \
	X( vkGetPhysicalDeviceProperties ) \
	X( vkGetPhysicalDeviceQueueFamilyProperties ) \
	X( vkGetPhysicalDeviceSurfaceCapabilitiesKHR ) \
	X( vkGetPhysicalDeviceSurfaceFormatsKHR ) \
	X( vkGetPhysicalDeviceSurfacePresentModesKHR ) \
	X( vkGetPhysicalDeviceSurfaceSupportKHR )

// optional instance functions, NULL when the extension is missing
#define VK_INSTANCE_FUNCTIONS_OPTIONAL( X ) \
	X( vkCreateDebugUtilsMessengerEXT ) \
	X( vkDestroyDebugUtilsMessengerEXT ) \
	X( vkGetPhysicalDeviceFeatures2 ) \
	X( vkGetPhysicalDeviceProperties2 )

// loaded with vkGetDeviceProcAddr
#define VK_DEVICE_FUNCTIONS( X ) \
	X( vkAcquireNextImageKHR ) \
	X( vkAllocateCommandBuffers ) \
	X( vkAllocateDescriptorSets ) \
	X( vkAllocateMemory ) \
	X( vkBeginCommandBuffer ) \
	X( vkBindBufferMemory ) \
	X( vkBindImageMemory ) \
	X( vkCmdBeginRenderPass ) \
	X( vkCmdBindDescriptorSets ) \
	X( vkCmdBindIndexBuffer ) \
	X( vkCmdBindPipeline ) \
	X( vkCmdBindVertexBuffers ) \
	X( vkCmdBlitImage ) \
	X( vkCmdClearAttachments ) \
	X( vkCmdClearColorImage ) \
	X( vkCmdClearDepthStencilImage ) \
	X( vkCmdCopyBufferToImage ) \
	X( vkCmdCopyImageToBuffer ) \
	X( vkCmdDrawIndexed ) \
	X( vkCmdEndRenderPass ) \
	X( vkCmdPipelineBarrier ) \
	X( vkCmdPushConstants ) \
	X( vkCmdSetDepthBias ) \
	X( vkCmdSetLineWidth ) \
	X( vkCmdSetScissor ) \
	X( vkCmdSetStencilCompareMask ) \
	X( vkCmdSetStencilReference ) \
	X( vkCmdSetStencilWriteMask ) \
	X( vkCmdSetViewport ) \
	X( vkCreateBuffer ) \
	X( vkCreateCommandPool ) \
	X( vkCreateDescriptorPool ) \
	X( vkCreateDescriptorSetLayout ) \
	X( vkCreateFence ) \
	X( vkCreateFramebuffer ) \
	X( vkCreateGraphicsPipelines ) \
	X( vkCreateImage ) \
	X( vkCreateImageView ) \
	X( vkCreatePipelineCache ) \
	X( vkCreatePipelineLayout ) \
	X( vkCreateRenderPass ) \
	X( vkCreateSampler ) \
	X( vkCreateSemaphore ) \
	X( vkCreateShaderModule ) \
	X( vkCreateSwapchainKHR ) \
	X( vkDestroyBuffer ) \
	X( vkDestroyCommandPool ) \
	X( vkDestroyDescriptorPool ) \
	X( vkDestroyDescriptorSetLayout ) \
	X( vkDestroyDevice ) \
	X( vkDestroyFence ) \
	X( vkDestroyFramebuffer ) \
	X( vkDestroyImage ) \
	X( vkDestroyImageView ) \
	X( vkDestroyPipeline ) \
	X( vkDestroyPipelineCache ) \
	X( vkDestroyPipelineLayout ) \
	X( vkDestroyRenderPass ) \
	X( vkDestroySampler ) \
	X( vkDestroySemaphore ) \
	X( vkDestroyShaderModule ) \
	X( vkDestroySwapchainKHR ) \
	X( vkDeviceWaitIdle ) \
	X( vkEndCommandBuffer ) \
	X( vkFreeDescriptorSets ) \
	X( vkFreeMemory ) \
	X( vkGetBufferMemoryRequirements ) \
	X( vkGetDeviceQueue ) \
	X( vkGetImageMemoryRequirements ) \
	X( vkGetSwapchainImagesKHR ) \
	X( vkMapMemory ) \
	X( vkQueuePresentKHR ) \
	X( vkQueueSubmit ) \
	X( vkResetFences ) \
	X( vkUnmapMemory ) \
	X( vkUpdateDescriptorSets ) \
	X( vkWaitForFences )

#define VK_DECLARE_FUNCTION( name ) extern PFN_##name q##name;
VK_GLOBAL_FUNCTIONS( VK_DECLARE_FUNCTION )
VK_INSTANCE_FUNCTIONS( VK_DECLARE_FUNCTION )
VK_INSTANCE_FUNCTIONS_OPTIONAL( VK_DECLARE_FUNCTION )
VK_DEVICE_FUNCTIONS( VK_DECLARE_FUNCTION )
#undef VK_DECLARE_FUNCTION

/*
====================================================================

TYPES

====================================================================
*/

// a buffer with its own memory, persistently mapped when host visible
typedef struct {
	VkBuffer		buffer;
	VkDeviceMemory	memory;
	VkDeviceSize	size;
	byte			*mapped;
} vkBuffer_t;

// storage of a texture; replaced when vk_create_texture is called again
typedef struct {
	VkImage			image;
	VkDeviceMemory	memory;
	VkImageView		view;
	VkDescriptorSet	descriptorSet;	// combined image sampler, set 0 or 1
	VkDescriptorPool descriptorPool;	// the pool descriptorSet came from
	VkFormat		format;
	int				width, height;
	int				mipLevels;
	qboolean		compressed;
	qboolean		rgb;			// alpha samples as 1 (A = ONE swizzle)
	int				wrapX, wrapY;	// WRAP_REPEAT or WRAP_CLAMP

	// layout tracking for uploads (see vk_image.c); between command buffers
	// every image is in SHADER_READ_ONLY_OPTIMAL
	qboolean		uploadPending;	// in TRANSFER_DST_OPTIMAL in the upload command buffer
	qboolean		mainPending;	// in TRANSFER_DST_OPTIMAL in the main command buffer
	uint32_t		uploadMips;		// mip levels written since uploadPending was set
	uint32_t		mainMips;		// mip levels written since mainPending was set
	uint64_t		lastUseSegment;	// segment of the main command buffer that last referenced it
} vkTexture_t;

// a slot of the texture table, addressed by image_t::texnum
typedef struct {
	unsigned int	handle;			// 0 when the slot is free
	vkTexture_t		*texture;
} vkTextureSlot_t;

typedef enum {
	VK_DESTROY_BUFFER,
	VK_DESTROY_IMAGE,
	VK_DESTROY_IMAGE_VIEW,
	VK_DESTROY_MEMORY,
	VK_DESTROY_DESCRIPTOR_SET
} vkDestroyType_t;

// a resource released once the frame that may still use it has completed
typedef struct {
	vkDestroyType_t	type;
	uint64_t		handle;
	uint64_t		pool;			// descriptor pool for VK_DESTROY_DESCRIPTOR_SET
} vkDeferredDestroy_t;

typedef struct {
	VkCommandPool	commandPool;
	VkCommandBuffer	uploadCmd;		// texture uploads that need no ordering with draws, submitted first
	VkCommandBuffer	mainCmd;		// everything else, in the order the renderer issued it
	VkCommandBuffer	presentCmd;		// copy to the swapchain image
	VkFence			fence;			// signaled when every submission of the frame has completed
	VkSemaphore		imageAcquired;

	vkBuffer_t		vertexBuffer;	// vertexes and indexes of this frame
	VkDeviceSize	vertexOffset;
	vkBuffer_t		stagingBuffer;	// texture upload data of this frame
	VkDeviceSize	stagingOffset;

	vkDeferredDestroy_t	*destroyList;
	int				numDestroy, maxDestroy;
} vkFrame_t;

typedef struct {
	// instance and device
	VkInstance			instance;
	VkDebugUtilsMessengerEXT debugMessenger;
	VkSurfaceKHR		surface;
	VkPhysicalDevice	physicalDevice;
	VkPhysicalDeviceProperties deviceProperties;
	VkPhysicalDeviceFeatures enabledFeatures;
	qboolean			depthClipControl;	// VK_EXT_depth_clip_control: GL clip space depth (z in [-w, w])
	VkPhysicalDeviceMemoryProperties memoryProperties;
	VkDevice			device;
	uint32_t			queueFamily;
	VkQueue				queue;

	// swapchain
	VkSwapchainKHR		swapchain;
	VkFormat			swapchainFormat;
	VkExtent2D			swapchainExtent;
	VkPresentModeKHR	presentMode;
	uint32_t			swapchainImageCount;
	VkImage				swapchainImages[VK_MAX_SWAPCHAIN_IMAGES];
	VkSemaphore			presentSemaphores[VK_MAX_SWAPCHAIN_IMAGES];	// one per image: presentation has no fence
	qboolean			swapchainOutOfDate;
	int					swapchainDrawableWidth;		// window size the swapchain was created for
	int					swapchainDrawableHeight;

	// render target: the frame is drawn offscreen at glConfig.vidWidth x
	// vidHeight and blitted to the swapchain image when it is presented, so
	// it keeps its contents between frames and can be read back at any time
	int					width, height;
	VkFormat			colorFormat;
	VkFormat			depthFormat;
	VkImage				colorImage;
	VkDeviceMemory		colorMemory;
	VkImageView			colorView;
	VkImage				depthImage;
	VkDeviceMemory		depthMemory;
	VkImageView			depthView;
	VkRenderPass		renderPass;
	VkFramebuffer		framebuffer;

	// shared objects
	VkDescriptorSetLayout textureSetLayout;
	VkPipelineLayout	pipelineLayout;
	VkShaderModule		vertexShader;
	VkShaderModule		vertexShaderClip;	// writes gl_ClipDistance (shaderClipDistance only)
	VkShaderModule		fragmentShader;
	VkPipelineCache		pipelineCache;

	// frames
	vkFrame_t			frames[VK_MAX_FRAMES];
	int					frameIndex;
	vkFrame_t			*frame;			// current frame, NULL between frames
	qboolean			uploadCmdRecording;
	qboolean			renderPassActive;
	uint64_t			segment;		// incremented each time the main command buffer is submitted
	VkFence				lastSubmitFence;	// VK_NULL_HANDLE when nothing is pending

	vkBuffer_t			readbackBuffer;
} vkInstance_t;

extern vkInstance_t vk;

/*
====================================================================

FUNCTIONS

====================================================================
*/

// vk_instance.c
void		VK_CreateDevice( void );
void		VK_DestroyDevice( void );
void		VK_CreateSwapchain( void );
void		VK_DestroySwapchain( void );
const char	*VK_SwapchainPresentModeName( void );
void		VK_GetDrawableSize( int *width, int *height );
const char	*VK_ResultString( VkResult result );
void		VK_Check( VkResult result, const char *what );
#define		VK_CHECK( call ) VK_Check( ( call ), #call )

// vk.c
uint32_t	VK_FindMemoryType( uint32_t typeBits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred );
void		VK_CreateBuffer( vkBuffer_t *buffer, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred );
void		VK_DestroyBuffer( vkBuffer_t *buffer );
void		VK_DeferDestroy( vkDestroyType_t type, uint64_t handle, uint64_t pool );
void		VK_BeginFrameIfNeeded( void );
void		VK_EndRenderPass( void );
void		VK_FlushAndWait( void );
VkCommandBuffer VK_GetUploadCommandBuffer( void );
void		VK_ImageBarrier( VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
				VkImageLayout oldLayout, VkImageLayout newLayout,
				VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
				VkPipelineStageFlags dstStage, VkAccessFlags dstAccess );

// vk_image.c
void		VK_InitTextures( void );
void		VK_ShutdownTextures( void );
void		VK_ShutdownTextureObjects( void );
vkTexture_t	*VK_GetTexture( unsigned int handle );
vkTexture_t	*VK_DefaultTexture( void );
void		VK_FinishUploads( VkCommandBuffer cmd, qboolean upload );
void		*VK_StagingAlloc( VkDeviceSize size, VkDeviceSize alignment, VkBuffer *buffer, VkDeviceSize *offset );

// vk_pipeline.c
typedef struct {
	uint64_t	bits;			// VK_PIPE_* fields
	uint32_t	alphaRef;		// float bits of the alpha test reference
} vkPipelineKey_t;

void		VK_InitPipelines( void );
void		VK_ShutdownPipelines( void );
VkPipeline	VK_GetPipeline( const vkPipelineKey_t *key );

// pipeline key fields
#define VK_PIPE_SRCBLEND_SHIFT		0		// GLS_SRCBLEND_* value, 4 bits
#define VK_PIPE_DSTBLEND_SHIFT		4		// GLS_DSTBLEND_* value >> 4, 4 bits
#define VK_PIPE_DEPTHTEST			( 1ull << 8 )
#define VK_PIPE_DEPTHWRITE			( 1ull << 9 )
#define VK_PIPE_DEPTHFUNC_SHIFT		10		// vkDepthFunc_t, 2 bits
#define VK_PIPE_POLYMODE_LINE		( 1ull << 12 )
#define VK_PIPE_CULL_SHIFT			13		// cullFace_t, 2 bits
#define VK_PIPE_LINES				( 1ull << 15 )
#define VK_PIPE_COLORMASK_SHIFT		16		// RGB write mask, 3 bits
#define VK_PIPE_ATEST_SHIFT			19		// 0 off, 1 >, 2 <, 3 >=, 2 bits
#define VK_PIPE_DEPTHBIAS			( 1ull << 21 )
#define VK_PIPE_COLOR_ARRAY			( 1ull << 22 )
#define VK_PIPE_TEXENV0_SHIFT		23		// texEnv_t, 0 when unit 0 is off, 3 bits
#define VK_PIPE_TEXENV1_SHIFT		26		// texEnv_t, 0 when unit 1 is off, 3 bits
#define VK_PIPE_TEX0_RGB			( 1ull << 29 )
#define VK_PIPE_TEX1_RGB			( 1ull << 30 )
#define VK_PIPE_STENCIL				( 1ull << 31 )
#define VK_PIPE_STENCILFUNC_SHIFT	32		// stencilFunc_t, 1 bit
#define VK_PIPE_STENCILOP_SHIFT		33		// 3 x stencilOp_t, 2 bits each
#define VK_PIPE_FOG					( 1ull << 39 )	// linear fog
#define VK_PIPE_CLIPPLANE			( 1ull << 40 )	// user clip plane

typedef enum {
	VK_DEPTHFUNC_LEQUAL,
	VK_DEPTHFUNC_EQUAL,
	VK_DEPTHFUNC_ALWAYS
} vkDepthFunc_t;

// push constants of shaders/generic.*: the vertex stage reads the first
// VK_PUSH_VERTEX_SIZE bytes, the fragment stage the rest
typedef struct {
	float	mvp[16];			// projection * modelview
	float	eyeZ[4];			// row 2 of the modelview (eye space z)
	float	clipPlane[4];		// user clip plane in object space, 0 when off
	float	fogColor[4];
	float	fogParams[2];		// -1 / (end - start), end / (end - start)
} vkPushConstants_t;

#define VK_PUSH_VERTEX_SIZE		( (uint32_t)offsetof( vkPushConstants_t, fogColor ) )
#define VK_PUSH_FRAGMENT_SIZE	( (uint32_t)( sizeof( vkPushConstants_t ) - VK_PUSH_VERTEX_SIZE ) )

// vk_shaders.c
extern const uint32_t	vk_spirv_generic_vert[];
extern const size_t		vk_spirv_generic_vert_size;
extern const uint32_t	vk_spirv_generic_clip_vert[];
extern const size_t		vk_spirv_generic_clip_vert_size;
extern const uint32_t	vk_spirv_generic_frag[];
extern const size_t		vk_spirv_generic_frag_size;

// cvars
extern cvar_t	*r_vkValidation;
