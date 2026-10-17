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
#define VK_ACQUIRE_SEMAPHORES		( VK_MAX_FRAMES + 1 )	// see VK_AcquireNextImage in vk.c
#define VK_MAX_SWAPCHAIN_IMAGES		16
#define VK_MAX_TEXTURES				4096	// live texture handles
#define VK_MAX_DEPTH_READS			256		// vk_queue_depth_reads per frame
#define VK_DEPTH_READ_FRAMES		300		// the depth image stays readable this many frames after the last read

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
	X( vkCmdCopyBuffer ) \
	X( vkCmdCopyBufferToImage ) \
	X( vkCmdCopyImageToBuffer ) \
	X( vkCmdDrawIndexed ) \
	X( vkCmdEndRenderPass ) \
	X( vkCmdPipelineBarrier ) \
	X( vkCmdPushConstants ) \
	X( vkCmdResetQueryPool ) \
	X( vkCmdSetDepthBias ) \
	X( vkCmdSetLineWidth ) \
	X( vkCmdSetScissor ) \
	X( vkCmdSetStencilCompareMask ) \
	X( vkCmdSetStencilReference ) \
	X( vkCmdSetStencilWriteMask ) \
	X( vkCmdSetViewport ) \
	X( vkCmdWriteTimestamp ) \
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
	X( vkCreateQueryPool ) \
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
	X( vkDestroyQueryPool ) \
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
	X( vkGetPipelineCacheData ) \
	X( vkGetQueryPoolResults ) \
	X( vkGetSwapchainImagesKHR ) \
	X( vkMapMemory ) \
	X( vkQueuePresentKHR ) \
	X( vkQueueSubmit ) \
	X( vkQueueWaitIdle ) \
	X( vkResetFences ) \
	X( vkUnmapMemory ) \
	X( vkUpdateDescriptorSets ) \
	X( vkWaitForFences )

// optional device functions, NULL when the extension is not enabled;
// vkCmdSetLineStippleEXT is loaded under the name of the enabled
// extension (VK_KHR_line_rasterization or VK_EXT_line_rasterization)
#define VK_DEVICE_FUNCTIONS_OPTIONAL( X ) \
	X( vkCmdSetLineStippleEXT ) \
	X( vkCreateRenderPass2KHR )

#define VK_DECLARE_FUNCTION( name ) extern PFN_##name q##name;
VK_GLOBAL_FUNCTIONS( VK_DECLARE_FUNCTION )
VK_INSTANCE_FUNCTIONS( VK_DECLARE_FUNCTION )
VK_INSTANCE_FUNCTIONS_OPTIONAL( VK_DECLARE_FUNCTION )
VK_DEVICE_FUNCTIONS( VK_DECLARE_FUNCTION )
VK_DEVICE_FUNCTIONS_OPTIONAL( VK_DECLARE_FUNCTION )
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
	VkDeviceMemory	memory;			// a block of vk_image.c or memory of its own
	int				memoryBlock;	// index of the block, -1 for memory of its own
	VkDeviceSize	memoryOffset, memorySize;
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
	VK_DESTROY_MEMORY_RANGE,		// a range of a texture memory block (vk_image.c)
	VK_DESTROY_DESCRIPTOR_SET,
	VK_DESTROY_FRAMEBUFFER
} vkDestroyType_t;

// a resource released once the frame that may still use it has completed
typedef struct {
	vkDestroyType_t	type;
	uint64_t		handle;			// the object; the block index for VK_DESTROY_MEMORY_RANGE
	uint64_t		pool;			// descriptor pool for VK_DESTROY_DESCRIPTOR_SET, offset for VK_DESTROY_MEMORY_RANGE
	uint64_t		size;			// VK_DESTROY_MEMORY_RANGE
} vkDeferredDestroy_t;

typedef enum {
	VK_TARGET_NONE,				// not chosen yet: nothing drawn or read in the frame
	VK_TARGET_OFFSCREEN,		// vk.colorImage, blitted to the swapchain at present
	VK_TARGET_SWAPCHAIN			// the acquired swapchain image
} vkTarget_t;

typedef struct {
	VkCommandPool	commandPool;
	VkCommandBuffer	uploadCmd;		// texture uploads that need no ordering with draws, submitted first
	VkCommandBuffer	mainCmd;		// everything else, in the order the renderer issued it
	VkCommandBuffer	presentCmd;		// copy to the swapchain image
	VkFence			fence;			// signaled when every submission of the frame has completed
	VkQueryPool		queryPool;		// VK_NUM_TIMESTAMPS timestamps, when the queue has them
	uint32_t		timestampsWritten;	// bits of the VK_TS_* recorded in this frame

	vkBuffer_t		vertexBuffer;	// vertexes and indexes of this frame
	VkDeviceSize	vertexOffset;
	vkBuffer_t		stagingBuffer;	// texture upload data of this frame
	VkDeviceSize	stagingOffset;
	vkBuffer_t		dlightBuffer;	// lights of vk_add_dlights (vkDlightGPU_t), set 2 of the shaders
	VkDescriptorSet	dlightSet;		// set 2: the dlight buffer, the skin data and this frame's skin data
	int				numDlights;
	vkBuffer_t		skinBuffer;		// frame data of the skinned draws (vk_alloc_skin_frame)
	int				skinWords;

	vkDeferredDestroy_t	*destroyList;
	int				numDestroy, maxDestroy;

	// depth reads (vk_queue_depth_reads) recorded in this frame, and the
	// results of the frame that used this slot before, copied when the slot
	// is reused
	uint64_t		serial;			// vk.frameSerial of the frame recorded in this slot
	vkBuffer_t		depthReadBuffer;	// one 4-byte texel per read
	int				numDepthReads;
	byte			depthReadInside[VK_MAX_DEPTH_READS];	// qfalse: outside of the framebuffer, reads 0
	uint64_t		resultSerial;	// frame whose results are in depthResults, 0 for none
	int				numDepthResults;
	float			depthResults[VK_MAX_DEPTH_READS];
} vkFrame_t;

// counters for the vkinfo command (vk.c), accumulated since the device was
// created; vkinfo prints what changed since its last call
typedef struct {
	uint64_t	frames;				// vk_end_frame calls
	uint64_t	renderPasses;		// render pass instances begun
	uint64_t	submits;			// vkQueueSubmit calls
	uint64_t	flushes;			// mid-frame submissions that waited for the GPU
	uint64_t	draws;
	uint64_t	staticDraws;		// draws from the static geometry (vk_set_static_geometry)
	uint64_t	skinDraws;			// skinned draws (drawGeometry_t::isSkinned)
	uint64_t	pipelineBinds;
	uint64_t	descriptorBinds;	// descriptor sets bound
	uint64_t	vertexBytes;		// copied into the vertex buffers
	uint64_t	stagingBytes;		// texture data copied into staging buffers
	uint64_t	mainUploads;		// uploads recorded in order with the draws (they end the render pass)
	uint64_t	textures;			// texture storages created
	uint64_t	pipelines;			// pipelines created
	uint64_t	allocations;		// vkAllocateMemory calls
	uint64_t	depthTargets;		// depth images replaced to become readable or not (VK_UpdateDepthTarget)
	uint64_t	swapchains;			// swapchains created (VK_CreateSwapchain)
	uint64_t	vertexGrowth;		// bytes of the frame vertex buffers created when one was full (VK_VertexAlloc)
	// microseconds spent in
	uint64_t	waitUs;				// waits for frame fences, mid-frame flushes included
	uint64_t	flushUs;			// waits of the mid-frame flushes (VK_FlushAndWait)
	uint64_t	submitUs;			// vkQueueSubmit
	uint64_t	acquireUs;			// vkAcquireNextImageKHR, in a frame (before presenting it)
	uint64_t	acquireAheadUs;		// vkAcquireNextImageKHR right after a present, for the next frame
	uint64_t	frameToPresentUs;	// from the start of a frame's recording to its present
	uint64_t	presentUs;			// vkQueuePresentKHR
	uint64_t	pipelineUs;			// vkCreateGraphicsPipelines
	uint64_t	allocationUs;		// vkAllocateMemory
	uint64_t	textureUs;			// creating texture storages (image, memory, view, descriptor set)
	// GPU time from timestamps, of the frames that presented
	uint64_t	gpuFrames;
	uint64_t	gpuMainUs;			// the main command buffer (all segments of the frame)
	uint64_t	gpuPresentUs;		// the copy to the swapchain image
	uint64_t	gpuFrameUs;			// start of the main command buffer to the end of the copy
} vkStats_t;

// GPU timestamps of a frame
#define VK_TS_MAIN_START		0
#define VK_TS_MAIN_END			1
#define VK_TS_PRESENT_START		2
#define VK_TS_PRESENT_END		3
#define VK_NUM_TIMESTAMPS		4

typedef struct {
	// instance and device
	VkInstance			instance;
	VkDebugUtilsMessengerEXT debugMessenger;
	VkSurfaceKHR		surface;
	VkPhysicalDevice	physicalDevice;
	VkPhysicalDeviceProperties deviceProperties;
	VkPhysicalDeviceFeatures enabledFeatures;
	qboolean			depthClipControl;	// VK_EXT_depth_clip_control: GL clip space depth (z in [-w, w])
	qboolean			bresenhamLines;		// VK_KHR/EXT_line_rasterization: GL's diamond-exit line rule
	qboolean			lineStipple;		// stippledBresenhamLines
	qboolean			depthStencilResolve;	// VK_KHR_depth_stencil_resolve (and create_renderpass2), for multisampling
	VkPhysicalDeviceMemoryProperties memoryProperties;
	VkDevice			device;
	uint32_t			queueFamily;
	uint32_t			timestampValidBits;	// 0: the queue has no timestamps
	VkQueue				queue;

	// swapchain
	VkSwapchainKHR		swapchain;
	VkFormat			swapchainFormat;
	VkExtent2D			swapchainExtent;
	VkPresentModeKHR	presentMode;
	uint32_t			swapchainImageCount;
	VkImage				swapchainImages[VK_MAX_SWAPCHAIN_IMAGES];
	VkSemaphore			presentSemaphores[VK_MAX_SWAPCHAIN_IMAGES];	// one per image: presentation has no fence
	VkImageUsageFlags	swapchainUsage;
	// frames are drawn into the swapchain images directly when they can be
	// color attachments and readback sources of the render target's format
	// (see VK_SelectTarget in vk.c)
	qboolean			swapchainDirect;
	VkImageView			swapchainViews[VK_MAX_SWAPCHAIN_IMAGES];
	VkFramebuffer		swapchainFramebuffers[VK_MAX_SWAPCHAIN_IMAGES];
	qboolean			swapchainOutOfDate;
	int					swapchainDrawableWidth;		// window size the swapchain was created for
	int					swapchainDrawableHeight;

	// render target of glConfig.vidWidth x vidHeight: the color target of a
	// frame is the swapchain image it will present (vk.target), or, when the
	// swapchain doesn't have that size (a window resize before the
	// vid_restart) or can't be drawn to, the offscreen color image, blitted
	// to the swapchain image at present. The depth/stencil image is always
	// offscreen. With multisampling (r_ext_multisample) the draws go to the
	// multisampled images; every render pass resolves the color into the
	// color target, the depth/stencil buffer is resolved into the depth image
	// before readbacks
	int					width, height;
	VkSampleCountFlagBits samples;
	VkFormat			colorFormat;
	VkFormat			depthFormat;
	VkImageAspectFlags	depthAspect;	// depth, and stencil when the format has it (see VK_CreateRenderTargets)
	VkImage				colorImage;
	VkDeviceMemory		colorMemory;
	VkImageView			colorView;
	VkImage				depthImage;
	VkDeviceMemory		depthMemory;
	VkImageView			depthView;
	// the depth image can be copied from (TRANSFER_SRC): always with
	// multisampling, otherwise only while depth or stencil reads are wanted
	// (VK_UpdateDepthTarget in vk.c)
	qboolean			depthReadable;
	// color clears to colors other than 0 and 1 are drawn as a quad (RADV,
	// see VK_DrawClearColor in vk.c)
	qboolean			drawColorClears;
	uint64_t			depthReadSerial;	// frame in which depth or stencil reads were last wanted, 0 for never
	VkImage				msColorImage, msDepthImage;
	VkDeviceMemory		msColorMemory, msDepthMemory;
	VkImageView			msColorView, msDepthView;
	VkRenderPass		renderPass;
	VkFramebuffer		framebuffer;
	VkRenderPass		depthResolvePass;	// multisampling: resolves the depth/stencil buffer for readbacks
	VkFramebuffer		depthResolveFramebuffer;

	// shared objects
	VkDescriptorSetLayout textureSetLayout;
	VkDescriptorSetLayout dlightSetLayout;	// set 2: the frame's dlight buffer, the skin data and the frame's skin data
	VkDescriptorPool	dlightPool;
	VkPipelineLayout	pipelineLayout;
	VkShaderModule		vertexShader;
	VkShaderModule		vertexShaderClip;	// writes gl_ClipDistance (shaderClipDistance only)
	VkShaderModule		skinVertexShader;	// skinned draws (shaders/skinned.vert)
	VkShaderModule		skinVertexShaderClip;
	VkShaderModule		fragmentShader;
	VkPipelineCache		pipelineCache;

	// frames
	vkFrame_t			frames[VK_MAX_FRAMES];
	int					frameIndex;
	vkFrame_t			*frame;			// current frame, NULL between frames
	vkTarget_t			target;			// color target of the current frame, chosen at its first render pass
	VkImage				targetImage;
	VkFramebuffer		targetFramebuffer;
	uint32_t			targetIndex;	// swapchain image for VK_TARGET_SWAPCHAIN
	qboolean			acquireWaitPending;	// the next submission waits for the held image's semaphore

	// the swapchain image acquired for the next present (VK_AcquireNextImage),
	// usually right after the previous present
	qboolean			heldImage;
	uint32_t			heldIndex;
	int					heldSemaphore;	// index in acquireSemaphores
	VkSemaphore			acquireSemaphores[VK_ACQUIRE_SEMAPHORES];
	uint64_t			acquireWaitSerial[VK_ACQUIRE_SEMAPHORES];	// frame whose submission waited for it, 0 for none
	int					nextAcquireSemaphore;
	uint64_t			frameStartUs;	// when the frame being recorded started
	uint64_t			lastPresentUs;	// when vkQueuePresentKHR was last called (vk_framelog.c)
	uint64_t			frameSerial;	// incremented when a frame starts, the first frame is 1
	qboolean			uploadCmdRecording;
	qboolean			renderPassActive;
	uint64_t			segment;		// incremented each time the main command buffer is submitted
	VkFence				lastSubmitFence;	// VK_NULL_HANDLE when nothing is pending

	vkBuffer_t			readbackBuffer;

	vkStats_t			stats;
	vkStats_t			statsReported;	// stats at the last vkinfo
	int					liveAllocations;	// device memory objects
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
uint64_t	VK_Microseconds( void );
void		VK_Check( VkResult result, const char *what );
#define		VK_CHECK( call ) VK_Check( ( call ), #call )

// vk.c
void		VK_CreateSwapchainTargets( void );
void		VK_ReleaseHeldImage( void );
void		VK_DestroySwapchainTargets( void );
uint32_t	VK_FindMemoryType( uint32_t typeBits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred );
VkDeviceMemory VK_AllocateMemory( VkDeviceSize size, uint32_t memoryType );
void		VK_FreeMemory( VkDeviceMemory memory );
void		VK_CreateBuffer( vkBuffer_t *buffer, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred );
void		VK_DestroyBuffer( vkBuffer_t *buffer );
void		VK_DeferDestroy( vkDestroyType_t type, uint64_t handle, uint64_t pool );
void		VK_DeferDestroyRange( int block, VkDeviceSize offset, VkDeviceSize size );
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
void		VK_FreeMemoryRange( int block, VkDeviceSize offset, VkDeviceSize size );
void		VK_MemoryBlockInfo( int *numBlocks, VkDeviceSize *size, VkDeviceSize *used );
void		*VK_StagingAlloc( VkDeviceSize size, VkDeviceSize alignment, VkBuffer *buffer, VkDeviceSize *offset );

// vk_framelog.c
extern qboolean	vk_frameLog;	// r_vkFrameLog: the frame log records
void		VK_FrameLogGpu( uint64_t serial, uint64_t mainStart, uint64_t mainEnd, uint64_t frameEnd, uint64_t mask, double usPerTick );

// vk_pipeline.c
typedef struct {
	uint64_t	bits;			// VK_PIPE_* fields
	uint32_t	alphaRef;		// float bits of the alpha test reference
} vkPipelineKey_t;

void		VK_InitPipelines( void );
void		VK_ShutdownPipelines( void );
VkPipeline	VK_GetPipeline( const vkPipelineKey_t *key );
int			VK_NumPipelines( void );

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
#define VK_PIPE_STIPPLE				( 1ull << 41 )	// line stipple (lines and polygon edges)
#define VK_PIPE_DLIGHT_SHIFT		42		// unit + 1 whose texture gets the dynamic lights, 0 for none, 2 bits
#define VK_PIPE_DLIGHT_BACKS		( 1ull << 44 )	// no dynamic light from behind the triangle
#define VK_PIPE_SKINNED				( 1ull << 45 )	// skinned draw: shaders/skinned.vert, no vertex input

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
	uint32_t	dlightBase;		// first light of the list in the frame's dlight buffer
	uint32_t	dlightMask;		// the lights of the list that the draw adds
} vkPushConstants_t;

// a light of vk_add_dlights in the frame's dlight buffer (shaders/generic.frag)
typedef struct {
	float	origin[3];
	float	invRadius;			// 1 / radius
	float	color[3];			// vkDlight_t::color / scale
	float	scale;
} vkDlightGPU_t;

#define VK_MAX_FRAME_DLIGHTS	4096	// per frame: 128 lists of 32 lights

#define VK_SKIN_DATA_WORDS		( 8 * 1024 * 1024 )	// skin data, 32 MB
#define VK_SKIN_FRAME_WORDS		( 512 * 1024 )		// frame data of the skinned draws, 2 MB per frame

#define VK_PUSH_VERTEX_SIZE		( (uint32_t)offsetof( vkPushConstants_t, fogColor ) )
#define VK_PUSH_FRAGMENT_SIZE	( (uint32_t)( sizeof( vkPushConstants_t ) - VK_PUSH_VERTEX_SIZE ) )

// vk_shaders.c
extern const uint32_t	vk_spirv_generic_vert[];
extern const size_t		vk_spirv_generic_vert_size;
extern const uint32_t	vk_spirv_generic_clip_vert[];
extern const size_t		vk_spirv_generic_clip_vert_size;
extern const uint32_t	vk_spirv_generic_frag[];
extern const size_t		vk_spirv_generic_frag_size;
extern const uint32_t	vk_spirv_skinned_vert[];
extern const size_t		vk_spirv_skinned_vert_size;
extern const uint32_t	vk_spirv_skinned_clip_vert[];
extern const size_t		vk_spirv_skinned_clip_vert_size;

// cvars
extern cvar_t	*r_vkValidation;
extern cvar_t	*r_ext_multisample;
