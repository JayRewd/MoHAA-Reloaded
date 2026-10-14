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

// vk.c -- Vulkan implementation of the backend interface in vk.h
//
// Frames: the renderer draws into an offscreen color and depth/stencil
// target of glConfig.vidWidth x vidHeight. At the end of the frame the color
// target is blitted to the swapchain image and presented. The target keeps
// its contents between frames, like a GL back buffer, so reading pixels
// before anything is drawn returns the last presented frame.
//
// Each frame records into a main command buffer (draws, clears, readbacks
// and uploads that must be ordered with draws) and an upload command buffer
// (see vk_image.c); both are submitted at vk_end_frame, or earlier when the
// CPU needs the results (vk_read_pixels) or the staging buffer is full. A
// frame starts lazily on the first operation that needs a command buffer.
//
// State: the vk_set_* functions only record state. vk_draw turns it into a
// pipeline (vk_pipeline.c) and dynamic state, copies the vertexes and
// indexes into a per-frame buffer and records the draw.

#include "vk_local.h"

#define VK_VERTEX_BUFFER_SIZE	( 4 * 1024 * 1024 )		// per frame, grows when needed
#define VK_STAGING_BUFFER_SIZE	( 32 * 1024 * 1024 )	// per frame

// dynamic state to set before the next draw
#define VK_DIRTY_VIEWPORT		1
#define VK_DIRTY_SCISSOR		2
#define VK_DIRTY_DEPTHBIAS		4
#define VK_DIRTY_LINEWIDTH		8
#define VK_DIRTY_STENCIL		16
#define VK_DIRTY_MVP			32		// vertex stage push constants
#define VK_DIRTY_FOG			64		// fragment stage push constants
#define VK_DIRTY_STIPPLE		128		// line stipple, applied with stippled pipelines only
#define VK_DIRTY_ALL			255

vkInstance_t	vk;
cvar_t			*r_vkValidation;

// render state recorded by the vk_set_* functions
typedef struct {
	unsigned long	stateBits;
	texEnv_t		texEnv[NUM_TEXTURE_BUNDLES];
	unsigned int	texture[NUM_TEXTURE_BUNDLES];	// image_t::texnum of the bound images
	cullFace_t		cull;
	qboolean		colorMask[4];
	qboolean		polygonOffset;
	float			offsetFactor, offsetUnits;
	float			depthMin, depthMax;
	int				viewport[4];		// window coordinates, origin at the bottom-left
	int				scissor[4];
	float			projection[16];
	float			modelview[16];
	float			lineWidth;
	qboolean		stencilTest;
	stencilFunc_t	stencilFunc;
	int				stencilRef;
	unsigned int	stencilMask;
	unsigned int	stencilWriteMask;
	stencilOp_t		stencilOp[3];		// stencil fail, depth fail, depth pass
	qboolean		fog;
	vec4_t			fogColor;
	float			fogStart, fogEnd;
	qboolean		clipPlaneEnabled;
	float			clipPlane[4];		// eye space
	int				lineStippleFactor;	// 0: no stipple
	unsigned short	lineStipplePattern;
} vkState_t;

static vkState_t	vks;
static int			vk_dirty;

// what is bound in the main command buffer being recorded
static struct {
	VkPipeline		pipeline;
	VkDescriptorSet	sets[NUM_TEXTURE_BUNDLES];
} vkb;

static vkPipelineKey_t	vk_lastKey;
static VkPipeline		vk_lastPipeline;

/*
====================================================================

HELPERS

====================================================================
*/

uint32_t VK_FindMemoryType( uint32_t typeBits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred ) {
	const VkPhysicalDeviceMemoryProperties *props = &vk.memoryProperties;
	uint32_t i;

	for ( i = 0; i < props->memoryTypeCount; i++ ) {
		if ( ( typeBits & ( 1u << i ) ) && ( props->memoryTypes[i].propertyFlags & ( required | preferred ) ) == ( required | preferred ) ) {
			return i;
		}
	}
	for ( i = 0; i < props->memoryTypeCount; i++ ) {
		if ( ( typeBits & ( 1u << i ) ) && ( props->memoryTypes[i].propertyFlags & required ) == required ) {
			return i;
		}
	}

	ri.Error( ERR_FATAL, "Vulkan: no memory type with properties 0x%x", required );
	return 0;
}

void VK_CreateBuffer( vkBuffer_t *buffer, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred ) {
	VkBufferCreateInfo info;
	VkMemoryRequirements memReqs;
	VkMemoryAllocateInfo allocInfo;
	void *mapped;

	Com_Memset( buffer, 0, sizeof( *buffer ) );

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK( qvkCreateBuffer( vk.device, &info, NULL, &buffer->buffer ) );

	qvkGetBufferMemoryRequirements( vk.device, buffer->buffer, &memReqs );
	Com_Memset( &allocInfo, 0, sizeof( allocInfo ) );
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = VK_FindMemoryType( memReqs.memoryTypeBits, required, preferred );
	VK_CHECK( qvkAllocateMemory( vk.device, &allocInfo, NULL, &buffer->memory ) );
	VK_CHECK( qvkBindBufferMemory( vk.device, buffer->buffer, buffer->memory, 0 ) );

	if ( required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ) {
		VK_CHECK( qvkMapMemory( vk.device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &mapped ) );
		buffer->mapped = mapped;
	}
	buffer->size = size;
}

void VK_DestroyBuffer( vkBuffer_t *buffer ) {
	if ( buffer->buffer ) {
		qvkDestroyBuffer( vk.device, buffer->buffer, NULL );
	}
	if ( buffer->memory ) {
		qvkFreeMemory( vk.device, buffer->memory, NULL );
	}
	Com_Memset( buffer, 0, sizeof( *buffer ) );
}

void VK_ImageBarrier( VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
	VkImageLayout oldLayout, VkImageLayout newLayout,
	VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
	VkPipelineStageFlags dstStage, VkAccessFlags dstAccess )
{
	VkImageMemoryBarrier barrier;

	Com_Memset( &barrier, 0, sizeof( barrier ) );
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = aspect;
	barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

	qvkCmdPipelineBarrier( cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &barrier );
}

/*
====================================================================

DEFERRED DESTRUCTION

====================================================================
*/

/*
===============
VK_DeferDestroy

Releases a resource once the GPU is done with every command recorded so far
===============
*/
void VK_DeferDestroy( vkDestroyType_t type, uint64_t handle, uint64_t pool ) {
	vkFrame_t *frame;

	if ( !handle ) {
		return;
	}

	// the list of a frame is processed when the frame slot is reused, after
	// waiting for its fence; between frames, the last submitted frame is
	// the one that may still use the resource
	if ( vk.frame ) {
		frame = vk.frame;
	} else {
		frame = &vk.frames[( vk.frameIndex + VK_MAX_FRAMES - 1 ) % VK_MAX_FRAMES];
	}

	if ( frame->numDestroy == frame->maxDestroy ) {
		vkDeferredDestroy_t *list;

		frame->maxDestroy = frame->maxDestroy ? frame->maxDestroy * 2 : 1024;
		list = ri.Malloc( sizeof( *list ) * frame->maxDestroy );
		if ( frame->numDestroy ) {
			Com_Memcpy( list, frame->destroyList, sizeof( *list ) * frame->numDestroy );
		}
		if ( frame->destroyList ) {
			ri.Free( frame->destroyList );
		}
		frame->destroyList = list;
	}

	frame->destroyList[frame->numDestroy].type = type;
	frame->destroyList[frame->numDestroy].handle = handle;
	frame->destroyList[frame->numDestroy].pool = pool;
	frame->numDestroy++;
}

static void VK_ProcessDestroyList( vkFrame_t *frame ) {
	int i;

	for ( i = 0; i < frame->numDestroy; i++ ) {
		const vkDeferredDestroy_t *d = &frame->destroyList[i];

		switch ( d->type ) {
		case VK_DESTROY_BUFFER:
			qvkDestroyBuffer( vk.device, (VkBuffer)d->handle, NULL );
			break;
		case VK_DESTROY_IMAGE:
			qvkDestroyImage( vk.device, (VkImage)d->handle, NULL );
			break;
		case VK_DESTROY_IMAGE_VIEW:
			qvkDestroyImageView( vk.device, (VkImageView)d->handle, NULL );
			break;
		case VK_DESTROY_MEMORY:
			qvkFreeMemory( vk.device, (VkDeviceMemory)d->handle, NULL );
			break;
		case VK_DESTROY_DESCRIPTOR_SET:
			{
				VkDescriptorSet set = (VkDescriptorSet)d->handle;

				VK_CHECK( qvkFreeDescriptorSets( vk.device, (VkDescriptorPool)d->pool, 1, &set ) );
			}
			break;
		}
	}
	frame->numDestroy = 0;
}

/*
====================================================================

FRAMES AND COMMAND BUFFERS

====================================================================
*/

/*
===============
VK_DepthTexelValue

Window-space depth of a texel copied from the depth aspect of the depth
target
===============
*/
static float VK_DepthTexelValue( const byte *texel ) {
	float depth;

	if ( vk.depthFormat == VK_FORMAT_D16_UNORM_S8_UINT ) {
		return *(const unsigned short *)texel / 65535.0f;
	}
	if ( vk.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ) {
		return ( *(const unsigned int *)texel & 0xFFFFFF ) / 16777215.0f;
	}
	Com_Memcpy( &depth, texel, sizeof( depth ) );
	return depth;
}

/*
===============
VK_CollectDepthReads

Called once the fence of a frame slot has been waited for: keeps the
results of the depth reads the slot's last frame recorded
===============
*/
static void VK_CollectDepthReads( vkFrame_t *frame ) {
	int i;

	for ( i = 0; i < frame->numDepthReads; i++ ) {
		if ( frame->depthReadInside[i] ) {
			frame->depthResults[i] = VK_DepthTexelValue( frame->depthReadBuffer.mapped + i * 4 );
		} else {
			frame->depthResults[i] = 0.0f;
		}
	}
	frame->numDepthResults = frame->numDepthReads;
	frame->resultSerial = frame->serial;
	frame->numDepthReads = 0;
}

static void VK_BeginMainCommandBuffer( void ) {
	VkCommandBufferBeginInfo info;

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK( qvkBeginCommandBuffer( vk.frame->mainCmd, &info ) );

	// nothing is bound in a new command buffer
	Com_Memset( &vkb, 0, sizeof( vkb ) );
	vk_dirty = VK_DIRTY_ALL;
}

void VK_BeginFrameIfNeeded( void ) {
	vkFrame_t *frame;

	if ( vk.frame ) {
		return;
	}

	frame = &vk.frames[vk.frameIndex];

	// the last frame that used this slot must be done
	VK_CHECK( qvkWaitForFences( vk.device, 1, &frame->fence, VK_TRUE, UINT64_MAX ) );
	VK_ProcessDestroyList( frame );
	VK_CollectDepthReads( frame );

	frame->vertexOffset = 0;
	frame->stagingOffset = 0;
	frame->serial = ++vk.frameSerial;

	vk.frame = frame;
	vk.uploadCmdRecording = qfalse;
	vk.renderPassActive = qfalse;

	VK_BeginMainCommandBuffer();
}

VkCommandBuffer VK_GetUploadCommandBuffer( void ) {
	VK_BeginFrameIfNeeded();

	if ( !vk.uploadCmdRecording ) {
		VkCommandBufferBeginInfo info;

		Com_Memset( &info, 0, sizeof( info ) );
		info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK( qvkBeginCommandBuffer( vk.frame->uploadCmd, &info ) );
		vk.uploadCmdRecording = qtrue;
	}

	return vk.frame->uploadCmd;
}

static void VK_BeginRenderPass( void ) {
	VkRenderPassBeginInfo info;

	VK_BeginFrameIfNeeded();
	if ( vk.renderPassActive ) {
		return;
	}

	// textures uploaded in order with the draws go back to the sampled layout
	VK_FinishUploads( vk.frame->mainCmd, qfalse );

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	info.renderPass = vk.renderPass;
	info.framebuffer = vk.framebuffer;
	info.renderArea.extent.width = vk.width;
	info.renderArea.extent.height = vk.height;
	qvkCmdBeginRenderPass( vk.frame->mainCmd, &info, VK_SUBPASS_CONTENTS_INLINE );

	vk.renderPassActive = qtrue;
}

void VK_EndRenderPass( void ) {
	if ( vk.renderPassActive ) {
		qvkCmdEndRenderPass( vk.frame->mainCmd );
		vk.renderPassActive = qfalse;
	}
}

/*
===============
VK_SubmitFrameCommands

Submits what the current frame has recorded so far: the upload command
buffer, then the main one
===============
*/
static void VK_SubmitFrameCommands( VkFence fence ) {
	vkFrame_t *frame = vk.frame;
	VkCommandBuffer cmds[2];
	VkSubmitInfo submit;
	uint32_t numCmds;

	numCmds = 0;
	if ( vk.uploadCmdRecording ) {
		VK_FinishUploads( frame->uploadCmd, qtrue );
		VK_CHECK( qvkEndCommandBuffer( frame->uploadCmd ) );
		cmds[numCmds++] = frame->uploadCmd;
		vk.uploadCmdRecording = qfalse;
	}

	VK_EndRenderPass();
	VK_FinishUploads( frame->mainCmd, qfalse );
	VK_CHECK( qvkEndCommandBuffer( frame->mainCmd ) );
	cmds[numCmds++] = frame->mainCmd;

	Com_Memset( &submit, 0, sizeof( submit ) );
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = numCmds;
	submit.pCommandBuffers = cmds;

	if ( fence ) {
		VK_CHECK( qvkResetFences( vk.device, 1, &fence ) );
		vk.lastSubmitFence = fence;
	}
	VK_CHECK( qvkQueueSubmit( vk.queue, 1, &submit, fence ) );

	// draws recorded from now on are in a new segment (see vk_image.c)
	vk.segment++;
}

/*
===============
VK_FlushAndWait

Submits the frame so far, waits for the GPU and continues the frame in a
new command buffer
===============
*/
void VK_FlushAndWait( void ) {
	int i;

	if ( !vk.frame ) {
		return;
	}

	VK_SubmitFrameCommands( vk.frame->fence );
	VK_CHECK( qvkWaitForFences( vk.device, 1, &vk.frame->fence, VK_TRUE, UINT64_MAX ) );

	// the GPU is idle: everything waiting for destruction can go, and the
	// buffers of the frame can be reused
	for ( i = 0; i < VK_MAX_FRAMES; i++ ) {
		VK_ProcessDestroyList( &vk.frames[i] );
	}
	vk.frame->vertexOffset = 0;
	vk.frame->stagingOffset = 0;

	VK_BeginMainCommandBuffer();
}

/*
===============
VK_Present

Copies the frame to a swapchain image and presents it; returns qfalse when
there is nothing to present to (minimized window), the frame fence is then
not submitted
===============
*/
static qboolean VK_Present( vkFrame_t *frame ) {
	VkCommandBufferBeginInfo beginInfo;
	VkPipelineStageFlags waitStage;
	VkSubmitInfo submit;
	VkPresentInfoKHR presentInfo;
	VkImageBlit blit;
	VkCommandBuffer cmd;
	VkImage image;
	uint32_t imageIndex;
	VkResult result;
	int width, height;

	// recreate the swapchain when the window changed; there is none while
	// the window has no area (minimized)
	VK_GetDrawableSize( &width, &height );
	if ( !vk.swapchain || vk.swapchainOutOfDate || width != vk.swapchainDrawableWidth || height != vk.swapchainDrawableHeight ) {
		VK_CreateSwapchain();
	}
	if ( !vk.swapchain ) {
		return qfalse;
	}

	result = qvkAcquireNextImageKHR( vk.device, vk.swapchain, UINT64_MAX, frame->imageAcquired, VK_NULL_HANDLE, &imageIndex );
	if ( result == VK_ERROR_OUT_OF_DATE_KHR ) {
		VK_CreateSwapchain();
		if ( !vk.swapchain ) {
			return qfalse;
		}
		result = qvkAcquireNextImageKHR( vk.device, vk.swapchain, UINT64_MAX, frame->imageAcquired, VK_NULL_HANDLE, &imageIndex );
	}
	if ( result == VK_ERROR_OUT_OF_DATE_KHR ) {
		vk.swapchainOutOfDate = qtrue;
		return qfalse;
	}
	if ( result == VK_SUBOPTIMAL_KHR ) {
		// still presentable, recreate it next frame
		vk.swapchainOutOfDate = qtrue;
	} else {
		VK_CHECK( result );
	}

	image = vk.swapchainImages[imageIndex];
	cmd = frame->presentCmd;

	Com_Memset( &beginInfo, 0, sizeof( beginInfo ) );
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK( qvkBeginCommandBuffer( cmd, &beginInfo ) );

	VK_ImageBarrier( cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT );
	VK_ImageBarrier( cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );

	// the window may have been resized since the last vid_restart: scale
	Com_Memset( &blit, 0, sizeof( blit ) );
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[1].x = vk.width;
	blit.srcOffsets[1].y = vk.height;
	blit.srcOffsets[1].z = 1;
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.layerCount = 1;
	blit.dstOffsets[1].x = vk.swapchainExtent.width;
	blit.dstOffsets[1].y = vk.swapchainExtent.height;
	blit.dstOffsets[1].z = 1;
	qvkCmdBlitImage( cmd, vk.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit, ( vk.width == (int)vk.swapchainExtent.width && vk.height == (int)vk.swapchainExtent.height ) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR );

	VK_ImageBarrier( cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0 );
	VK_ImageBarrier( cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );

	VK_CHECK( qvkEndCommandBuffer( cmd ) );

	waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	Com_Memset( &submit, 0, sizeof( submit ) );
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &frame->imageAcquired;
	submit.pWaitDstStageMask = &waitStage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &vk.presentSemaphores[imageIndex];

	VK_CHECK( qvkResetFences( vk.device, 1, &frame->fence ) );
	VK_CHECK( qvkQueueSubmit( vk.queue, 1, &submit, frame->fence ) );
	vk.lastSubmitFence = frame->fence;

	Com_Memset( &presentInfo, 0, sizeof( presentInfo ) );
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &vk.presentSemaphores[imageIndex];
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &vk.swapchain;
	presentInfo.pImageIndices = &imageIndex;

	result = qvkQueuePresentKHR( vk.queue, &presentInfo );
	if ( result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ) {
		vk.swapchainOutOfDate = qtrue;
	} else {
		VK_CHECK( result );
	}

	return qtrue;
}

static void VK_CreateFrames( void ) {
	VkCommandPoolCreateInfo poolInfo;
	VkCommandBufferAllocateInfo allocInfo;
	VkFenceCreateInfo fenceInfo;
	VkSemaphoreCreateInfo semaphoreInfo;
	VkCommandBuffer cmds[3];
	int i;

	Com_Memset( &semaphoreInfo, 0, sizeof( semaphoreInfo ) );
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	for ( i = 0; i < VK_MAX_FRAMES; i++ ) {
		vkFrame_t *frame = &vk.frames[i];

		Com_Memset( &poolInfo, 0, sizeof( poolInfo ) );
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = vk.queueFamily;
		VK_CHECK( qvkCreateCommandPool( vk.device, &poolInfo, NULL, &frame->commandPool ) );

		Com_Memset( &allocInfo, 0, sizeof( allocInfo ) );
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = frame->commandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = ARRAY_LEN( cmds );
		VK_CHECK( qvkAllocateCommandBuffers( vk.device, &allocInfo, cmds ) );
		frame->uploadCmd = cmds[0];
		frame->mainCmd = cmds[1];
		frame->presentCmd = cmds[2];

		// signaled: the first wait for it returns at once
		Com_Memset( &fenceInfo, 0, sizeof( fenceInfo ) );
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		VK_CHECK( qvkCreateFence( vk.device, &fenceInfo, NULL, &frame->fence ) );

		VK_CHECK( qvkCreateSemaphore( vk.device, &semaphoreInfo, NULL, &frame->imageAcquired ) );

		VK_CreateBuffer( &frame->vertexBuffer, VK_VERTEX_BUFFER_SIZE,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
		VK_CreateBuffer( &frame->stagingBuffer, VK_STAGING_BUFFER_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0 );
		VK_CreateBuffer( &frame->depthReadBuffer, VK_MAX_DEPTH_READS * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT );
	}

	for ( i = 0; i < VK_MAX_SWAPCHAIN_IMAGES; i++ ) {
		VK_CHECK( qvkCreateSemaphore( vk.device, &semaphoreInfo, NULL, &vk.presentSemaphores[i] ) );
	}

	vk.frameIndex = 0;
	vk.frame = NULL;
}

static void VK_DestroyFrames( void ) {
	int i;

	for ( i = 0; i < VK_MAX_FRAMES; i++ ) {
		vkFrame_t *frame = &vk.frames[i];

		VK_ProcessDestroyList( frame );
		if ( frame->destroyList ) {
			ri.Free( frame->destroyList );
		}
		VK_DestroyBuffer( &frame->vertexBuffer );
		VK_DestroyBuffer( &frame->stagingBuffer );
		VK_DestroyBuffer( &frame->depthReadBuffer );
		if ( frame->imageAcquired ) {
			qvkDestroySemaphore( vk.device, frame->imageAcquired, NULL );
		}
		if ( frame->fence ) {
			qvkDestroyFence( vk.device, frame->fence, NULL );
		}
		if ( frame->commandPool ) {
			qvkDestroyCommandPool( vk.device, frame->commandPool, NULL );
		}
		Com_Memset( frame, 0, sizeof( *frame ) );
	}

	for ( i = 0; i < VK_MAX_SWAPCHAIN_IMAGES; i++ ) {
		if ( vk.presentSemaphores[i] ) {
			qvkDestroySemaphore( vk.device, vk.presentSemaphores[i], NULL );
			vk.presentSemaphores[i] = VK_NULL_HANDLE;
		}
	}

	vk.frame = NULL;
}

/*
====================================================================

RENDER TARGETS

====================================================================
*/

static qboolean VK_FormatHasFeatures( VkFormat format, VkFormatFeatureFlags features ) {
	VkFormatProperties props;

	qvkGetPhysicalDeviceFormatProperties( vk.physicalDevice, format, &props );
	return ( props.optimalTilingFeatures & features ) == features;
}

static void VK_CreateTargetImage( VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
	VkImage *image, VkDeviceMemory *memory, VkImageView *view )
{
	VkImageCreateInfo imageInfo;
	VkImageViewCreateInfo viewInfo;
	VkMemoryRequirements memReqs;
	VkMemoryAllocateInfo allocInfo;

	Com_Memset( &imageInfo, 0, sizeof( imageInfo ) );
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = format;
	imageInfo.extent.width = vk.width;
	imageInfo.extent.height = vk.height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = usage;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK( qvkCreateImage( vk.device, &imageInfo, NULL, image ) );

	qvkGetImageMemoryRequirements( vk.device, *image, &memReqs );
	Com_Memset( &allocInfo, 0, sizeof( allocInfo ) );
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = VK_FindMemoryType( memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 );
	VK_CHECK( qvkAllocateMemory( vk.device, &allocInfo, NULL, memory ) );
	VK_CHECK( qvkBindImageMemory( vk.device, *image, *memory, 0 ) );

	Com_Memset( &viewInfo, 0, sizeof( viewInfo ) );
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = *image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = aspect;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	VK_CHECK( qvkCreateImageView( vk.device, &viewInfo, NULL, view ) );
}

static int VK_DepthBits( VkFormat format ) {
	switch ( format ) {
	case VK_FORMAT_D16_UNORM_S8_UINT: return 16;
	case VK_FORMAT_D24_UNORM_S8_UINT: return 24;
	default: return 32;
	}
}

static void VK_CreateRenderTargets( void ) {
	static const VkFormat depthFormats[] = {
		VK_FORMAT_D24_UNORM_S8_UINT,	// what GL1 asks for
		VK_FORMAT_D32_SFLOAT_S8_UINT,	// AMD has no D24
		VK_FORMAT_D16_UNORM_S8_UINT
	};
	VkAttachmentDescription attachments[2];
	VkAttachmentReference colorRef, depthRef;
	VkSubpassDescription subpass;
	VkSubpassDependency dependencies[2];
	VkRenderPassCreateInfo renderPassInfo;
	VkFramebufferCreateInfo framebufferInfo;
	VkImageView views[2];
	VkClearColorValue clearColor;
	VkClearDepthStencilValue clearDepth;
	VkImageSubresourceRange range;
	VkCommandBuffer cmd;
	VkPipelineStageFlags attachmentStages;
	unsigned int i;

	vk.width = glConfig.vidWidth;
	vk.height = glConfig.vidHeight;

	// rendered to, blitted to the swapchain, read back and cleared at creation
	vk.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
	if ( !VK_FormatHasFeatures( vk.colorFormat, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
		| VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT ) ) {
		ri.Error( ERR_FATAL, "Vulkan: VK_FORMAT_R8G8B8A8_UNORM can't be rendered to" );
	}

	vk.depthFormat = VK_FORMAT_UNDEFINED;
	for ( i = 0; i < ARRAY_LEN( depthFormats ); i++ ) {
		if ( VK_FormatHasFeatures( depthFormats[i], VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
			| VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT ) ) {
			vk.depthFormat = depthFormats[i];
			break;
		}
	}
	if ( vk.depthFormat == VK_FORMAT_UNDEFINED ) {
		ri.Error( ERR_FATAL, "Vulkan: no depth/stencil format" );
	}

	VK_CreateTargetImage( vk.colorFormat,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		VK_IMAGE_ASPECT_COLOR_BIT, &vk.colorImage, &vk.colorMemory, &vk.colorView );
	VK_CreateTargetImage( vk.depthFormat,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, &vk.depthImage, &vk.depthMemory, &vk.depthView );

	// the attachments keep their contents across render passes: a render
	// pass ends for every readback and every upload ordered with draws
	Com_Memset( attachments, 0, sizeof( attachments ) );
	attachments[0].format = vk.colorFormat;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[1].format = vk.depthFormat;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	colorRef.attachment = 0;
	colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	depthRef.attachment = 1;
	depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	Com_Memset( &subpass, 0, sizeof( subpass ) );
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef;
	subpass.pDepthStencilAttachment = &depthRef;

	// successive render passes read what the previous ones wrote
	attachmentStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
		| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	Com_Memset( dependencies, 0, sizeof( dependencies ) );
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = attachmentStages;
	dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[0].dstStageMask = attachmentStages;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = attachmentStages;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstStageMask = attachmentStages;
	dependencies[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	Com_Memset( &renderPassInfo, 0, sizeof( renderPassInfo ) );
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = ARRAY_LEN( attachments );
	renderPassInfo.pAttachments = attachments;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = ARRAY_LEN( dependencies );
	renderPassInfo.pDependencies = dependencies;
	VK_CHECK( qvkCreateRenderPass( vk.device, &renderPassInfo, NULL, &vk.renderPass ) );

	views[0] = vk.colorView;
	views[1] = vk.depthView;
	Com_Memset( &framebufferInfo, 0, sizeof( framebufferInfo ) );
	framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.renderPass = vk.renderPass;
	framebufferInfo.attachmentCount = ARRAY_LEN( views );
	framebufferInfo.pAttachments = views;
	framebufferInfo.width = vk.width;
	framebufferInfo.height = vk.height;
	framebufferInfo.layers = 1;
	VK_CHECK( qvkCreateFramebuffer( vk.device, &framebufferInfo, NULL, &vk.framebuffer ) );

	// initial contents: black with alpha 1, depth 1, stencil 0
	VK_BeginFrameIfNeeded();
	cmd = vk.frame->mainCmd;

	Com_Memset( &range, 0, sizeof( range ) );
	range.levelCount = 1;
	range.layerCount = 1;

	VK_ImageBarrier( cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
	Com_Memset( &clearColor, 0, sizeof( clearColor ) );
	clearColor.float32[3] = 1.0f;
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	qvkCmdClearColorImage( cmd, vk.colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range );
	VK_ImageBarrier( cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );

	VK_ImageBarrier( cmd, vk.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
	clearDepth.depth = 1.0f;
	clearDepth.stencil = 0;
	range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	qvkCmdClearDepthStencilImage( cmd, vk.depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearDepth, 1, &range );
	VK_ImageBarrier( cmd, vk.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );
}

static void VK_DestroyRenderTargets( void ) {
	if ( vk.framebuffer ) {
		qvkDestroyFramebuffer( vk.device, vk.framebuffer, NULL );
	}
	if ( vk.renderPass ) {
		qvkDestroyRenderPass( vk.device, vk.renderPass, NULL );
	}
	if ( vk.colorView ) {
		qvkDestroyImageView( vk.device, vk.colorView, NULL );
	}
	if ( vk.colorImage ) {
		qvkDestroyImage( vk.device, vk.colorImage, NULL );
	}
	if ( vk.colorMemory ) {
		qvkFreeMemory( vk.device, vk.colorMemory, NULL );
	}
	if ( vk.depthView ) {
		qvkDestroyImageView( vk.device, vk.depthView, NULL );
	}
	if ( vk.depthImage ) {
		qvkDestroyImage( vk.device, vk.depthImage, NULL );
	}
	if ( vk.depthMemory ) {
		qvkFreeMemory( vk.device, vk.depthMemory, NULL );
	}
	vk.framebuffer = VK_NULL_HANDLE;
	vk.renderPass = VK_NULL_HANDLE;
	vk.colorView = vk.depthView = VK_NULL_HANDLE;
	vk.colorImage = vk.depthImage = VK_NULL_HANDLE;
	vk.colorMemory = vk.depthMemory = VK_NULL_HANDLE;
}

/*
====================================================================

LIFECYCLE

====================================================================
*/

static const char *VK_VendorName( uint32_t vendorID ) {
	switch ( vendorID ) {
	case 0x1002: return "AMD";
	case 0x10DE: return "NVIDIA";
	case 0x8086: return "Intel";
	case 0x13B5: return "ARM";
	case 0x5143: return "Qualcomm";
	case 0x106B: return "Apple";
	case 0x10005: return "Mesa";
	default: return va( "0x%04x", vendorID );
	}
}

/*
===============
VK_ReportCapabilities

Fills glConfig with what the device supports; R_ApplyExtensionCvars then
turns features off from the r_ext_* cvars
===============
*/
static void VK_ReportCapabilities( void ) {
	const VkPhysicalDeviceProperties *props = &vk.deviceProperties;
	char driver[VK_MAX_DRIVER_NAME_SIZE + VK_MAX_DRIVER_INFO_SIZE + 4];

	driver[0] = 0;
	if ( qvkGetPhysicalDeviceProperties2 && props->apiVersion >= VK_API_VERSION_1_2 ) {
		VkPhysicalDeviceProperties2 props2;
		VkPhysicalDeviceDriverProperties driverProps;

		Com_Memset( &driverProps, 0, sizeof( driverProps ) );
		driverProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
		Com_Memset( &props2, 0, sizeof( props2 ) );
		props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		props2.pNext = &driverProps;
		qvkGetPhysicalDeviceProperties2( vk.physicalDevice, &props2 );
		Com_sprintf( driver, sizeof( driver ), ", %s %s", driverProps.driverName, driverProps.driverInfo );
	}

	Q_strncpyz( glConfig.vendor_string, VK_VendorName( props->vendorID ), sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, props->deviceName, sizeof( glConfig.renderer_string ) );
	Com_sprintf( glConfig.version_string, sizeof( glConfig.version_string ), "Vulkan %d.%d.%d%s",
		VK_API_VERSION_MAJOR( props->apiVersion ), VK_API_VERSION_MINOR( props->apiVersion ),
		VK_API_VERSION_PATCH( props->apiVersion ), driver );

	glConfig.maxTextureSize = props->limits.maxImageDimension2D;
	glConfig.numTextureUnits = 2;
	glConfig.textureEnvAddAvailable = qtrue;
	glConfig.textureCompression = vk.enabledFeatures.textureCompressionBC ? TC_S3TC_ARB : TC_NONE;

	textureFilterAnisotropic = vk.enabledFeatures.samplerAnisotropy ? qtrue : qfalse;
	maxAnisotropy = textureFilterAnisotropic ? (int)props->limits.maxSamplerAnisotropy : 0;
	haveClampToEdge = qtrue;

	// the bits of the formats actually used; like GL1 the stencil buffer is
	// only reported when r_stencilbits asked for one
	glConfig.colorBits = 24;
	glConfig.depthBits = VK_DepthBits( vk.depthFormat );
	glConfig.stencilBits = r_stencilbits->integer > 0 ? 8 : 0;

	ri.Printf( PRINT_ALL, "Vulkan device: %s (%s)\n", glConfig.renderer_string, glConfig.version_string );
	ri.Printf( PRINT_ALL, "Vulkan present mode: %s, render target %dx%d\n", VK_SwapchainPresentModeName(), vk.width, vk.height );
}

static void VK_ResetState( void ) {
	int i;

	Com_Memset( &vks, 0, sizeof( vks ) );
	for ( i = 0; i < NUM_TEXTURE_BUNDLES; i++ ) {
		vks.texEnv[i] = TEXENV_MODULATE;
	}
	vks.stateBits = GLS_DEPTHTEST_DISABLE | GLS_DEPTHMASK_TRUE;
	vks.colorMask[0] = vks.colorMask[1] = vks.colorMask[2] = vks.colorMask[3] = qtrue;
	vks.depthMax = 1.0f;
	vks.viewport[2] = vks.scissor[2] = glConfig.vidWidth;
	vks.viewport[3] = vks.scissor[3] = glConfig.vidHeight;
	vks.projection[0] = vks.projection[5] = vks.projection[10] = vks.projection[15] = 1.0f;
	vks.modelview[0] = vks.modelview[5] = vks.modelview[10] = vks.modelview[15] = 1.0f;
	vks.lineWidth = 1.0f;
	vks.stencilMask = vks.stencilWriteMask = ~0U;
	vks.fogColor[3] = 1.0f;
	vks.fogEnd = 1.0f;
	vks.lineStipplePattern = 0xFFFF;

	Com_Memset( &vk_lastKey, 0, sizeof( vk_lastKey ) );
	vk_lastPipeline = VK_NULL_HANDLE;
	vk_dirty = VK_DIRTY_ALL;
}

void vk_initialize( void ) {
	r_vkValidation = ri.Cvar_Get( "r_vkValidation", "0", CVAR_LATCH );

	Com_Memset( &vk, 0, sizeof( vk ) );
	VK_ResetState();
	// 0 is the segment of new textures, which no draw has used
	vk.segment = 1;

	VK_CreateDevice();
	VK_CreateFrames();
	VK_CreateSwapchain();
	VK_CreateRenderTargets();
	VK_InitPipelines();
	VK_InitTextures();
	VK_ReportCapabilities();
}

void vk_shutdown( void ) {
	int i;

	if ( vk.device ) {
		qvkDeviceWaitIdle( vk.device );

		VK_ShutdownTextures();
		for ( i = 0; i < VK_MAX_FRAMES; i++ ) {
			VK_ProcessDestroyList( &vk.frames[i] );
		}
		VK_ShutdownTextureObjects();
		VK_ShutdownPipelines();
		VK_DestroyFrames();
		VK_DestroyRenderTargets();
		VK_DestroyBuffer( &vk.readbackBuffer );
		VK_DestroySwapchain();
	}

	VK_DestroyDevice();

	Com_Memset( &vk, 0, sizeof( vk ) );
}

void vk_begin_frame( void ) {
	if ( !vk.device ) {
		return;
	}
	VK_BeginFrameIfNeeded();
}

void vk_end_frame( void ) {
	vkFrame_t *frame;

	if ( !vk.device ) {
		return;
	}

	VK_BeginFrameIfNeeded();
	frame = vk.frame;

	VK_SubmitFrameCommands( VK_NULL_HANDLE );

	if ( !VK_Present( frame ) ) {
		// nothing presented: the fence still has to signal for the next
		// use of this frame slot
		VK_CHECK( qvkResetFences( vk.device, 1, &frame->fence ) );
		VK_CHECK( qvkQueueSubmit( vk.queue, 0, NULL, frame->fence ) );
		vk.lastSubmitFence = frame->fence;
	}

	vk.frame = NULL;
	vk.frameIndex = ( vk.frameIndex + 1 ) % VK_MAX_FRAMES;
}

void vk_finish( void ) {
	// like glFinish for what has been submitted; the commands of the frame
	// being recorded are submitted at vk_end_frame
	if ( vk.device && vk.lastSubmitFence ) {
		VK_CHECK( qvkWaitForFences( vk.device, 1, &vk.lastSubmitFence, VK_TRUE, UINT64_MAX ) );
	}
}

/*
====================================================================

RENDER STATE

====================================================================
*/

void vk_set_state( unsigned long stateBits ) {
	vks.stateBits = stateBits;
}

void vk_bind_texture( int tmu, image_t *image ) {
	if ( tmu < 0 || tmu >= NUM_TEXTURE_BUNDLES ) {
		return;
	}
	// the handle, not the storage: a draw uses the storage the image has
	// when the draw is recorded
	vks.texture[tmu] = image ? image->texnum : 0;
}

void vk_set_texenv( int tmu, texEnv_t env ) {
	if ( tmu < 0 || tmu >= NUM_TEXTURE_BUNDLES || env < TEXENV_MODULATE || env > TEXENV_COMBINE4 ) {
		return;
	}
	vks.texEnv[tmu] = env;
}

void vk_set_cull( cullFace_t face ) {
	vks.cull = face;
}

void vk_set_depth_range( float zNear, float zFar ) {
	vks.depthMin = Com_Clamp( 0.0f, 1.0f, zNear );
	vks.depthMax = Com_Clamp( 0.0f, 1.0f, zFar );
	vk_dirty |= VK_DIRTY_VIEWPORT;
}

void vk_set_polygon_offset( qboolean enable, float factor, float units ) {
	vks.polygonOffset = enable;
	vks.offsetFactor = factor;
	vks.offsetUnits = units;
	vk_dirty |= VK_DIRTY_DEPTHBIAS;
}

void vk_set_color_mask( qboolean red, qboolean green, qboolean blue, qboolean alpha ) {
	vks.colorMask[0] = red;
	vks.colorMask[1] = green;
	vks.colorMask[2] = blue;
	vks.colorMask[3] = alpha;
}

void vk_set_fog( qboolean enable ) {
	vks.fog = enable;
}

void vk_set_fog_color( const vec4_t color ) {
	Vector4Copy( color, vks.fogColor );
	vk_dirty |= VK_DIRTY_FOG;
}

void vk_set_fog_range( float start, float end ) {
	vks.fogStart = start;
	vks.fogEnd = end;
	vk_dirty |= VK_DIRTY_FOG;
}

void vk_set_clip_plane( const float *eyePlane ) {
	vks.clipPlaneEnabled = eyePlane != NULL;
	if ( eyePlane ) {
		Vector4Copy( eyePlane, vks.clipPlane );
	}
	// the shaders get the plane in object space
	vk_dirty |= VK_DIRTY_MVP;
}

void vk_set_stencil_test( qboolean enable ) {
	vks.stencilTest = enable;
}

void vk_set_stencil_func( stencilFunc_t func, int ref, unsigned int mask ) {
	vks.stencilFunc = func;
	vks.stencilRef = ref;
	vks.stencilMask = mask;
	vk_dirty |= VK_DIRTY_STENCIL;
}

void vk_set_stencil_op( stencilOp_t stencilFail, stencilOp_t depthFail, stencilOp_t depthPass ) {
	vks.stencilOp[0] = stencilFail;
	vks.stencilOp[1] = depthFail;
	vks.stencilOp[2] = depthPass;
}

void vk_set_stencil_write_mask( unsigned int mask ) {
	vks.stencilWriteMask = mask;
	vk_dirty |= VK_DIRTY_STENCIL;
}

void vk_set_line_width( float width ) {
	vks.lineWidth = width;
	vk_dirty |= VK_DIRTY_LINEWIDTH;
}

void vk_set_line_stipple( int factor, unsigned short pattern ) {
	// GL clamps the factor to [1, 256]; 0 disables stippling here
	vks.lineStippleFactor = factor > 0 ? Q_min( factor, 256 ) : 0;
	vks.lineStipplePattern = pattern;
	vk_dirty |= VK_DIRTY_STIPPLE;
}

void vk_set_projection( const float *matrix ) {
	Com_Memcpy( vks.projection, matrix, sizeof( vks.projection ) );
	vk_dirty |= VK_DIRTY_MVP;
}

void vk_set_modelview( const float *matrix ) {
	Com_Memcpy( vks.modelview, matrix, sizeof( vks.modelview ) );
	vk_dirty |= VK_DIRTY_MVP;
}

void vk_set_viewport( int x, int y, int width, int height ) {
	vks.viewport[0] = x;
	vks.viewport[1] = y;
	vks.viewport[2] = width;
	vks.viewport[3] = height;
	vk_dirty |= VK_DIRTY_VIEWPORT;
}

void vk_set_scissor( int x, int y, int width, int height ) {
	vks.scissor[0] = x;
	vks.scissor[1] = y;
	vks.scissor[2] = width;
	vks.scissor[3] = height;
	vk_dirty |= VK_DIRTY_SCISSOR;
}

/*
====================================================================

DRAWING

====================================================================
*/

/*
===============
VK_ScissorRect

The scissor rectangle in framebuffer coordinates (origin at the top-left),
clipped to the framebuffer; returns qfalse when it is empty
===============
*/
static qboolean VK_ScissorRect( VkRect2D *rect ) {
	int x0, y0, x1, y1;

	x0 = Q_max( vks.scissor[0], 0 );
	x1 = Q_min( vks.scissor[0] + vks.scissor[2], vk.width );
	y0 = Q_max( vk.height - ( vks.scissor[1] + vks.scissor[3] ), 0 );
	y1 = Q_min( vk.height - vks.scissor[1], vk.height );

	if ( x1 <= x0 || y1 <= y0 ) {
		Com_Memset( rect, 0, sizeof( *rect ) );
		return qfalse;
	}

	rect->offset.x = x0;
	rect->offset.y = y0;
	rect->extent.width = x1 - x0;
	rect->extent.height = y1 - y0;
	return qtrue;
}

/*
===============
VK_ComputeMVP

projection * modelview, with the projection remapped from GL clip space
(z in [-w, w]) to Vulkan's (z in [0, w]): z' = ( z + w ) / 2
===============
*/
static void VK_ComputeMVP( float *mvp ) {
	const float *mv = vks.modelview;
	float p[16];
	int c, r;

	for ( c = 0; c < 4; c++ ) {
		p[c * 4 + 0] = vks.projection[c * 4 + 0];
		p[c * 4 + 1] = vks.projection[c * 4 + 1];
		p[c * 4 + 2] = vk.depthClipControl ? vks.projection[c * 4 + 2] : 0.5f * ( vks.projection[c * 4 + 2] + vks.projection[c * 4 + 3] );
		p[c * 4 + 3] = vks.projection[c * 4 + 3];
	}

	for ( c = 0; c < 4; c++ ) {
		for ( r = 0; r < 4; r++ ) {
			mvp[c * 4 + r] = p[0 * 4 + r] * mv[c * 4 + 0] + p[1 * 4 + r] * mv[c * 4 + 1]
				+ p[2 * 4 + r] * mv[c * 4 + 2] + p[3 * 4 + r] * mv[c * 4 + 3];
		}
	}
}

static void VK_ApplyDynamicState( VkCommandBuffer cmd, qboolean stippled ) {
	if ( vk_dirty & VK_DIRTY_VIEWPORT ) {
		VkViewport viewport;

		// GL window coordinates have the origin at the bottom-left: a
		// negative height flips y
		viewport.x = vks.viewport[0];
		viewport.y = vk.height - vks.viewport[1];
		viewport.width = Q_max( vks.viewport[2], 1 );
		viewport.height = -(float)Q_max( vks.viewport[3], 1 );
		viewport.minDepth = vks.depthMin;
		viewport.maxDepth = vks.depthMax;
		qvkCmdSetViewport( cmd, 0, 1, &viewport );
	}

	if ( vk_dirty & VK_DIRTY_SCISSOR ) {
		VkRect2D scissor;

		VK_ScissorRect( &scissor );
		qvkCmdSetScissor( cmd, 0, 1, &scissor );
	}

	if ( vk_dirty & VK_DIRTY_DEPTHBIAS ) {
		// only used by pipelines with VK_PIPE_DEPTHBIAS
		qvkCmdSetDepthBias( cmd, vks.offsetUnits, 0.0f, vks.offsetFactor );
	}

	if ( vk_dirty & VK_DIRTY_LINEWIDTH ) {
		float width = 1.0f;

		if ( vk.enabledFeatures.wideLines ) {
			width = Com_Clamp( vk.deviceProperties.limits.lineWidthRange[0], vk.deviceProperties.limits.lineWidthRange[1], vks.lineWidth );
		}
		qvkCmdSetLineWidth( cmd, width );
	}

	if ( vk_dirty & VK_DIRTY_STENCIL ) {
		qvkCmdSetStencilCompareMask( cmd, VK_STENCIL_FACE_FRONT_AND_BACK, vks.stencilMask & 0xFF );
		qvkCmdSetStencilWriteMask( cmd, VK_STENCIL_FACE_FRONT_AND_BACK, vks.stencilWriteMask & 0xFF );
		qvkCmdSetStencilReference( cmd, VK_STENCIL_FACE_FRONT_AND_BACK, vks.stencilRef & 0xFF );
	}

	if ( vk_dirty & VK_DIRTY_MVP ) {
		const float *mv = vks.modelview;
		vkPushConstants_t pc;
		int i;

		VK_ComputeMVP( pc.mvp );

		pc.eyeZ[0] = mv[2];
		pc.eyeZ[1] = mv[6];
		pc.eyeZ[2] = mv[10];
		pc.eyeZ[3] = mv[14];

		// the eye space plane P keeps eye positions with P . ( MV v ) >= 0,
		// so the object space plane is P * MV
		for ( i = 0; i < 4; i++ ) {
			pc.clipPlane[i] = vks.clipPlaneEnabled ? vks.clipPlane[0] * mv[i * 4 + 0] + vks.clipPlane[1] * mv[i * 4 + 1]
				+ vks.clipPlane[2] * mv[i * 4 + 2] + vks.clipPlane[3] * mv[i * 4 + 3] : 0.0f;
		}

		qvkCmdPushConstants( cmd, vk.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, VK_PUSH_VERTEX_SIZE, &pc );
	}

	if ( vk_dirty & VK_DIRTY_FOG ) {
		vkPushConstants_t pc;
		float scale;
		int i;

		for ( i = 0; i < 4; i++ ) {
			pc.fogColor[i] = Com_Clamp( 0.0f, 1.0f, vks.fogColor[i] );
		}
		// computed like Mesa's gl_MesaFogParamsOptimized, so the factor
		// matches GL1 on Mesa drivers to the bit
		scale = ( vks.fogEnd == vks.fogStart ) ? 1.0f : -1.0f / ( vks.fogEnd - vks.fogStart );
		pc.fogParams[0] = scale;
		pc.fogParams[1] = vks.fogEnd * -scale;

		qvkCmdPushConstants( cmd, vk.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, VK_PUSH_VERTEX_SIZE, VK_PUSH_FRAGMENT_SIZE,
			(const byte *)&pc + VK_PUSH_VERTEX_SIZE );
	}

	if ( stippled ) {
		if ( vk_dirty & VK_DIRTY_STIPPLE ) {
			qvkCmdSetLineStippleEXT( cmd, vks.lineStippleFactor, vks.lineStipplePattern );
		}
		vk_dirty = 0;
	} else {
		// a pipeline with static stipple state invalidates the dynamic one:
		// set it again for the next stippled pipeline
		vk_dirty = VK_DIRTY_STIPPLE;
	}
}

static void VK_BuildPipelineKey( vkPipelineKey_t *key, primType_t prim, const drawGeometry_t *geometry, vkTexture_t **textures ) {
	unsigned long stateBits = vks.stateBits;
	uint64_t bits = 0;
	float alphaRef = 0.0f;
	int alphaFunc = 0;
	int i;

	if ( stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) {
		bits |= (uint64_t)( stateBits & GLS_SRCBLEND_BITS ) << VK_PIPE_SRCBLEND_SHIFT;
		bits |= (uint64_t)( ( stateBits & GLS_DSTBLEND_BITS ) >> 4 ) << VK_PIPE_DSTBLEND_SHIFT;
	}

	// like GL, no depth writes without the depth test
	if ( !( stateBits & GLS_DEPTHTEST_DISABLE ) ) {
		bits |= VK_PIPE_DEPTHTEST;
		if ( stateBits & GLS_DEPTHMASK_TRUE ) {
			bits |= VK_PIPE_DEPTHWRITE;
		}
		if ( stateBits & GLS_DEPTHFUNC_EQUAL ) {
			bits |= (uint64_t)VK_DEPTHFUNC_EQUAL << VK_PIPE_DEPTHFUNC_SHIFT;
		}
	}

	if ( prim == PRIM_LINES ) {
		bits |= VK_PIPE_LINES;
		if ( vks.lineStippleFactor && vk.lineStipple ) {
			bits |= VK_PIPE_STIPPLE;
		}
	} else {
		// culling, polygon mode and polygon offset only apply to polygons
		bits |= (uint64_t)vks.cull << VK_PIPE_CULL_SHIFT;
		if ( stateBits & GLS_POLYMODE_LINE ) {
			if ( vk.enabledFeatures.fillModeNonSolid ) {
				bits |= VK_PIPE_POLYMODE_LINE;
				if ( vks.lineStippleFactor && vk.lineStipple ) {
					bits |= VK_PIPE_STIPPLE;
				}
			}
		} else if ( vks.polygonOffset ) {
			// GL_POLYGON_OFFSET_FILL: filled polygons only
			bits |= VK_PIPE_DEPTHBIAS;
		}
	}

	bits |= (uint64_t)( ( vks.colorMask[0] ? 1 : 0 ) | ( vks.colorMask[1] ? 2 : 0 ) | ( vks.colorMask[2] ? 4 : 0 ) ) << VK_PIPE_COLORMASK_SHIFT;

	switch ( stateBits & GLS_ATEST_BITS ) {
	case GLS_ATEST_GT_0:
		alphaFunc = 1;
		alphaRef = 0.05f;
		break;
	case GLS_ATEST_LT_80:
		alphaFunc = 2;
		alphaRef = 0.5f;
		break;
	case GLS_ATEST_GE_80:
		alphaFunc = 3;
		alphaRef = 0.5f;
		break;
	case GLS_ATEST_LT_FOLIAGE1:
		alphaFunc = 2;
		alphaRef = r_alpha_foliage1->value;
		break;
	case GLS_ATEST_GE_FOLIAGE1:
		alphaFunc = 3;
		alphaRef = r_alpha_foliage1->value;
		break;
	case GLS_ATEST_LT_FOLIAGE2:
		alphaFunc = 2;
		alphaRef = r_alpha_foliage2->value;
		break;
	case GLS_ATEST_GE_FOLIAGE2:
		alphaFunc = 3;
		alphaRef = r_alpha_foliage2->value;
		break;
	}
	bits |= (uint64_t)alphaFunc << VK_PIPE_ATEST_SHIFT;

	if ( geometry->colors ) {
		bits |= VK_PIPE_COLOR_ARRAY;
	}

	for ( i = 0; i < NUM_TEXTURE_BUNDLES; i++ ) {
		if ( textures[i] ) {
			bits |= (uint64_t)vks.texEnv[i] << ( i ? VK_PIPE_TEXENV1_SHIFT : VK_PIPE_TEXENV0_SHIFT );
			// only TEXENV_REPLACE treats RGB textures differently
			if ( textures[i]->rgb && vks.texEnv[i] == TEXENV_REPLACE ) {
				bits |= i ? VK_PIPE_TEX1_RGB : VK_PIPE_TEX0_RGB;
			}
		}
	}

	if ( vks.fog ) {
		bits |= VK_PIPE_FOG;
	}
	if ( vks.clipPlaneEnabled ) {
		bits |= VK_PIPE_CLIPPLANE;
	}

	if ( vks.stencilTest ) {
		bits |= VK_PIPE_STENCIL;
		bits |= (uint64_t)vks.stencilFunc << VK_PIPE_STENCILFUNC_SHIFT;
		bits |= (uint64_t)vks.stencilOp[0] << VK_PIPE_STENCILOP_SHIFT;
		bits |= (uint64_t)vks.stencilOp[1] << ( VK_PIPE_STENCILOP_SHIFT + 2 );
		bits |= (uint64_t)vks.stencilOp[2] << ( VK_PIPE_STENCILOP_SHIFT + 4 );
	}

	key->bits = bits;
	key->alphaRef = 0;
	if ( alphaFunc ) {
		Com_Memcpy( &key->alphaRef, &alphaRef, sizeof( alphaRef ) );
	}
}

/*
===============
VK_VertexAlloc

Space for size bytes in the vertex buffer of the frame
===============
*/
static byte *VK_VertexAlloc( VkDeviceSize size, VkBuffer *buffer, VkDeviceSize *offset ) {
	vkFrame_t *frame = vk.frame;
	VkDeviceSize start;

	start = PAD( frame->vertexOffset, 16 );
	if ( start + size > frame->vertexBuffer.size ) {
		VkDeviceSize newSize = frame->vertexBuffer.size * 2;

		while ( newSize < size ) {
			newSize *= 2;
		}

		// commands recorded so far still read the old buffer
		VK_DeferDestroy( VK_DESTROY_BUFFER, (uint64_t)frame->vertexBuffer.buffer, 0 );
		VK_DeferDestroy( VK_DESTROY_MEMORY, (uint64_t)frame->vertexBuffer.memory, 0 );
		VK_CreateBuffer( &frame->vertexBuffer, newSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
		ri.Printf( PRINT_DEVELOPER, "Vulkan: vertex buffer grown to %d KB\n", (int)( newSize / 1024 ) );
		start = 0;
	}

	frame->vertexOffset = start + size;
	*buffer = frame->vertexBuffer.buffer;
	*offset = start;
	return frame->vertexBuffer.mapped + start;
}

static void VK_CopyStream( float *dst, const float *src, int count, int numFloats, int stride ) {
	int i;

	if ( stride == (int)sizeof( float ) * numFloats ) {
		Com_Memcpy( dst, src, sizeof( float ) * numFloats * count );
		return;
	}

	for ( i = 0; i < count; i++ ) {
		Com_Memcpy( dst, src, sizeof( float ) * numFloats );
		dst += numFloats;
		src = (const float *)( (const byte *)src + stride );
	}
}

/*
===============
VK_UploadGeometry

Copies the vertex streams and the indexes into the vertex buffer of the
frame and binds them: positions, colors (or the constant color), texture
coordinates of units 0 and 1, each in its own binding
===============
*/
static void VK_UploadGeometry( VkCommandBuffer cmd, const drawGeometry_t *geometry, int numIndexes, const glIndex_t *indexes ) {
	VkBuffer buffer, buffers[4];
	VkDeviceSize offset, offsets[4];
	VkDeviceSize xyzSize, colorSize, stSize[NUM_TEXTURE_BUNDLES], indexSize, total, pos;
	int numVertexes = geometry->numVertexes;
	byte *data;
	int i;

	xyzSize = PAD( sizeof( float ) * 3 * numVertexes, 16 );
	colorSize = geometry->colors ? PAD( 4 * numVertexes, 16 ) : sizeof( float ) * 4;
	total = xyzSize + colorSize;
	for ( i = 0; i < NUM_TEXTURE_BUNDLES; i++ ) {
		stSize[i] = geometry->st[i] ? PAD( sizeof( float ) * 2 * numVertexes, 16 ) : 0;
		total += stSize[i];
	}
	indexSize = sizeof( glIndex_t ) * numIndexes;
	total += indexSize;

	data = VK_VertexAlloc( total, &buffer, &offset );
	pos = 0;

	VK_CopyStream( (float *)( data + pos ), geometry->xyz, numVertexes, 3, geometry->xyzStride );
	buffers[0] = buffer;
	offsets[0] = offset + pos;
	pos += xyzSize;

	if ( geometry->colors ) {
		Com_Memcpy( data + pos, geometry->colors, 4 * numVertexes );
	} else {
		Com_Memcpy( data + pos, geometry->color, sizeof( float ) * 4 );
	}
	buffers[1] = buffer;
	offsets[1] = offset + pos;
	pos += colorSize;

	for ( i = 0; i < NUM_TEXTURE_BUNDLES; i++ ) {
		buffers[2 + i] = buffer;
		if ( geometry->st[i] ) {
			VK_CopyStream( (float *)( data + pos ), geometry->st[i], numVertexes, 2, geometry->stStride[i] );
			offsets[2 + i] = offset + pos;
			pos += stSize[i];
		} else {
			// the pipeline reads it with a stride of 0 and ignores it
			offsets[2 + i] = offsets[0];
		}
	}

	Com_Memcpy( data + pos, indexes, indexSize );

	qvkCmdBindVertexBuffers( cmd, 0, 4, buffers, offsets );
	qvkCmdBindIndexBuffer( cmd, buffer, offset + pos, VK_INDEX_TYPE_UINT32 );
}

void vk_draw( primType_t prim, const drawGeometry_t *geometry, int numIndexes, const glIndex_t *indexes ) {
	vkTexture_t *textures[NUM_TEXTURE_BUNDLES];
	vkPipelineKey_t key;
	VkCommandBuffer cmd;
	VkPipeline pipeline;
	int i;

	if ( !vk.device || numIndexes <= 0 || geometry->numVertexes <= 0 || !geometry->xyz ) {
		return;
	}

	VK_BeginRenderPass();
	cmd = vk.frame->mainCmd;

	// a unit is used when the geometry has texture coordinates for it
	for ( i = 0; i < NUM_TEXTURE_BUNDLES; i++ ) {
		textures[i] = NULL;
		if ( geometry->st[i] ) {
			textures[i] = VK_GetTexture( vks.texture[i] );
			if ( !textures[i] ) {
				textures[i] = VK_DefaultTexture();
			}
			textures[i]->lastUseSegment = vk.segment;
		}
	}

	VK_BuildPipelineKey( &key, prim, geometry, textures );
	if ( key.bits == vk_lastKey.bits && key.alphaRef == vk_lastKey.alphaRef && vk_lastPipeline ) {
		pipeline = vk_lastPipeline;
	} else {
		pipeline = VK_GetPipeline( &key );
		vk_lastKey = key;
		vk_lastPipeline = pipeline;
	}
	if ( pipeline != vkb.pipeline ) {
		qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
		vkb.pipeline = pipeline;
	}

	// every set the shaders declare must be bound, used or not
	for ( i = 0; i < NUM_TEXTURE_BUNDLES; i++ ) {
		VkDescriptorSet set = ( textures[i] ? textures[i] : VK_DefaultTexture() )->descriptorSet;

		if ( set != vkb.sets[i] ) {
			qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipelineLayout, i, 1, &set, 0, NULL );
			vkb.sets[i] = set;
		}
	}

	VK_ApplyDynamicState( cmd, ( key.bits & VK_PIPE_STIPPLE ) != 0 );
	VK_UploadGeometry( cmd, geometry, numIndexes, indexes );

	qvkCmdDrawIndexed( cmd, numIndexes, 1, 0, 0, 0 );
}

/*
===============
VK_ClearComponent

A clear color component rounded to the nearest 8-bit value of the color
target. Drivers convert clear colors with float arithmetic that can round
the other way near a half step (RADV packs 0.7f as 179, GL gives 178, the
correctly rounded value); a color that is exactly representable converts
the same everywhere.
===============
*/
static float VK_ClearComponent( float c ) {
	return (float)( floor( Com_Clamp( 0.0f, 1.0f, c ) * 255.0 + 0.5 ) / 255.0 );
}

/*
===============
VK_ClearColorMasked

glClear of the color buffer with a partial color mask: a quad over the
scissor rectangle with the current mask, since vkCmdClearAttachments
ignores the mask
===============
*/
static void VK_ClearColorMasked( const vec4_t color ) {
	static const float xyz[4][3] = { { -1, -1, 0 }, { 1, -1, 0 }, { 1, 1, 0 }, { -1, 1, 0 } };
	static const glIndex_t indexes[6] = { 0, 1, 2, 0, 2, 3 };
	drawGeometry_t geometry;
	vkState_t saved;
	int i;

	saved = vks;

	vks.stateBits = GLS_DEPTHTEST_DISABLE;
	vks.cull = CULL_NONE;
	vks.polygonOffset = qfalse;
	vks.stencilTest = qfalse;
	vks.fog = qfalse;
	vks.clipPlaneEnabled = qfalse;
	vks.viewport[0] = vks.viewport[1] = 0;
	vks.viewport[2] = vk.width;
	vks.viewport[3] = vk.height;
	Com_Memset( vks.projection, 0, sizeof( vks.projection ) );
	Com_Memset( vks.modelview, 0, sizeof( vks.modelview ) );
	for ( i = 0; i < 4; i++ ) {
		vks.projection[i * 5] = vks.modelview[i * 5] = 1.0f;
	}
	vk_dirty |= VK_DIRTY_VIEWPORT | VK_DIRTY_MVP;

	Com_Memset( &geometry, 0, sizeof( geometry ) );
	geometry.numVertexes = 4;
	geometry.xyz = xyz[0];
	geometry.xyzStride = sizeof( xyz[0] );
	for ( i = 0; i < 4; i++ ) {
		geometry.color[i] = VK_ClearComponent( color[i] );
	}
	vk_draw( PRIM_TRIANGLES, &geometry, 6, indexes );

	vks = saved;
	vk_dirty |= VK_DIRTY_VIEWPORT | VK_DIRTY_MVP;
}

void vk_clear( int buffers, const vec4_t color ) {
	VkClearAttachment attachments[2];
	VkClearRect rect;
	qboolean maskedColor;
	uint32_t numAttachments;

	if ( !vk.device ) {
		return;
	}

	if ( !VK_ScissorRect( &rect.rect ) ) {
		return;
	}
	rect.baseArrayLayer = 0;
	rect.layerCount = 1;

	numAttachments = 0;
	maskedColor = qfalse;

	// alpha isn't stored (no destination alpha, see vk_pipeline.c), so only
	// the RGB mask matters
	if ( ( buffers & CLEAR_COLOR ) && color && ( vks.colorMask[0] || vks.colorMask[1] || vks.colorMask[2] ) ) {
		if ( vks.colorMask[0] && vks.colorMask[1] && vks.colorMask[2] ) {
			attachments[numAttachments].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			attachments[numAttachments].colorAttachment = 0;
			attachments[numAttachments].clearValue.color.float32[0] = VK_ClearComponent( color[0] );
			attachments[numAttachments].clearValue.color.float32[1] = VK_ClearComponent( color[1] );
			attachments[numAttachments].clearValue.color.float32[2] = VK_ClearComponent( color[2] );
			attachments[numAttachments].clearValue.color.float32[3] = 1.0f;
			numAttachments++;
		} else {
			maskedColor = qtrue;
		}
	}

	// like glClear, the depth write mask and the stencil write mask apply
	// (a partial stencil mask clears all bits; the renderer never sets one)
	attachments[numAttachments].aspectMask = 0;
	if ( ( buffers & CLEAR_DEPTH ) && ( vks.stateBits & GLS_DEPTHMASK_TRUE ) ) {
		attachments[numAttachments].aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
	}
	if ( ( buffers & CLEAR_STENCIL ) && ( vks.stencilWriteMask & 0xFF ) ) {
		attachments[numAttachments].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}
	if ( attachments[numAttachments].aspectMask ) {
		attachments[numAttachments].colorAttachment = 0;
		attachments[numAttachments].clearValue.depthStencil.depth = 1.0f;
		attachments[numAttachments].clearValue.depthStencil.stencil = 0;
		numAttachments++;
	}

	if ( !numAttachments && !maskedColor ) {
		return;
	}

	VK_BeginRenderPass();
	if ( numAttachments ) {
		qvkCmdClearAttachments( vk.frame->mainCmd, numAttachments, attachments, 1, &rect );
	}
	if ( maskedColor ) {
		VK_ClearColorMasked( color );
	}
}

void vk_read_pixels( int x, int y, int width, int height, readFormat_t format, void *buffer ) {
	VkBufferImageCopy region;
	VkBufferMemoryBarrier bufferBarrier;
	VkImageAspectFlags aspect, barrierAspect;
	VkImageLayout layout;
	VkPipelineStageFlags stages;
	VkAccessFlags access;
	VkCommandBuffer cmd;
	VkImage image;
	VkDeviceSize size;
	int pixelSize, texelSize;
	int x0, y0, x1, y1, w, h, row, col;

	switch ( format ) {
	case READ_RGB8:
		pixelSize = 3;
		break;
	case READ_DEPTH_FLOAT:
		pixelSize = sizeof( float );
		break;
	case READ_STENCIL8:
	default:
		pixelSize = 1;
		break;
	}

	if ( width <= 0 || height <= 0 ) {
		return;
	}
	// pixels outside of the framebuffer read as 0
	Com_Memset( buffer, 0, (size_t)pixelSize * width * height );

	x0 = Q_max( x, 0 );
	y0 = Q_max( y, 0 );
	x1 = Q_min( x + width, vk.width );
	y1 = Q_min( y + height, vk.height );
	if ( !vk.device || x1 <= x0 || y1 <= y0 ) {
		return;
	}
	w = x1 - x0;
	h = y1 - y0;

	if ( format == READ_RGB8 ) {
		image = vk.colorImage;
		aspect = barrierAspect = VK_IMAGE_ASPECT_COLOR_BIT;
		layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		texelSize = 4;
	} else {
		image = vk.depthImage;
		aspect = format == READ_DEPTH_FLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
		barrierAspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		if ( format == READ_STENCIL8 ) {
			texelSize = 1;
		} else if ( vk.depthFormat == VK_FORMAT_D16_UNORM_S8_UINT ) {
			texelSize = 2;
		} else {
			texelSize = 4;
		}
	}

	size = (VkDeviceSize)w * h * texelSize;
	if ( vk.readbackBuffer.size < size ) {
		// readbacks are waited for, nothing uses the old buffer
		VK_DestroyBuffer( &vk.readbackBuffer );
		VK_CreateBuffer( &vk.readbackBuffer, PAD( size, 65536 ), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT );
	}

	VK_BeginFrameIfNeeded();
	VK_EndRenderPass();
	cmd = vk.frame->mainCmd;

	VK_ImageBarrier( cmd, image, barrierAspect, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		stages, access, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT );

	Com_Memset( &region, 0, sizeof( region ) );
	region.imageSubresource.aspectMask = aspect;
	region.imageSubresource.layerCount = 1;
	region.imageOffset.x = x0;
	region.imageOffset.y = vk.height - y1;
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	region.imageExtent.depth = 1;
	qvkCmdCopyImageToBuffer( cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.readbackBuffer.buffer, 1, &region );

	VK_ImageBarrier( cmd, image, barrierAspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, stages, access | ( format == READ_RGB8 ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT ) );

	Com_Memset( &bufferBarrier, 0, sizeof( bufferBarrier ) );
	bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	bufferBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bufferBarrier.buffer = vk.readbackBuffer.buffer;
	bufferBarrier.size = VK_WHOLE_SIZE;
	qvkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bufferBarrier, 0, NULL );

	VK_FlushAndWait();

	// the copy has the top row first, the caller wants the bottom row first
	for ( row = 0; row < h; row++ ) {
		const byte *src = vk.readbackBuffer.mapped + (size_t)( h - 1 - row ) * w * texelSize;
		byte *dst = (byte *)buffer + ( (size_t)( y0 - y + row ) * width + ( x0 - x ) ) * pixelSize;

		switch ( format ) {
		case READ_RGB8:
			for ( col = 0; col < w; col++ ) {
				dst[col * 3 + 0] = src[col * 4 + 0];
				dst[col * 3 + 1] = src[col * 4 + 1];
				dst[col * 3 + 2] = src[col * 4 + 2];
			}
			break;
		case READ_DEPTH_FLOAT:
			for ( col = 0; col < w; col++ ) {
				float depth = VK_DepthTexelValue( src + col * texelSize );

				Com_Memcpy( dst + col * sizeof( float ), &depth, sizeof( float ) );
			}
			break;
		case READ_STENCIL8:
		default:
			Com_Memcpy( dst, src, w );
			break;
		}
	}
}

/*
===============
vk_queue_depth_reads

Records copies of single depth texels into the frame's depth read buffer,
in order with the draws; the results are collected when the frame slot is
reused (VK_CollectDepthReads), so nothing waits for the GPU. An id is the
frame serial (low 24 bits) and the index of the read in the frame.
===============
*/
void vk_queue_depth_reads( int count, const int *xy, unsigned int *ids ) {
	VkBufferImageCopy regions[VK_MAX_DEPTH_READS];
	VkBufferMemoryBarrier bufferBarrier;
	VkCommandBuffer cmd;
	vkFrame_t *frame;
	int i, numRegions;

	if ( count <= 0 ) {
		return;
	}
	if ( !vk.device ) {
		Com_Memset( ids, 0, sizeof( *ids ) * count );
		return;
	}

	VK_BeginFrameIfNeeded();
	frame = vk.frame;

	numRegions = 0;
	for ( i = 0; i < count; i++ ) {
		int x = xy[i * 2 + 0];
		int y = xy[i * 2 + 1];
		int index = frame->numDepthReads;

		if ( index == VK_MAX_DEPTH_READS ) {
			ids[i] = 0;
			continue;
		}
		frame->numDepthReads++;
		ids[i] = (unsigned int)( ( frame->serial & 0xFFFFFF ) << 8 ) | index;

		frame->depthReadInside[index] = x >= 0 && y >= 0 && x < vk.width && y < vk.height;
		if ( !frame->depthReadInside[index] ) {
			continue;
		}

		Com_Memset( &regions[numRegions], 0, sizeof( regions[numRegions] ) );
		regions[numRegions].bufferOffset = index * 4;
		regions[numRegions].imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		regions[numRegions].imageSubresource.layerCount = 1;
		regions[numRegions].imageOffset.x = x;
		regions[numRegions].imageOffset.y = vk.height - 1 - y;
		regions[numRegions].imageExtent.width = 1;
		regions[numRegions].imageExtent.height = 1;
		regions[numRegions].imageExtent.depth = 1;
		numRegions++;
	}

	if ( !numRegions ) {
		return;
	}

	VK_EndRenderPass();
	cmd = frame->mainCmd;

	VK_ImageBarrier( cmd, vk.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT );

	qvkCmdCopyImageToBuffer( cmd, vk.depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame->depthReadBuffer.buffer, numRegions, regions );

	VK_ImageBarrier( cmd, vk.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );

	// the host reads the buffer after waiting for the frame fence
	Com_Memset( &bufferBarrier, 0, sizeof( bufferBarrier ) );
	bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	bufferBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bufferBarrier.buffer = frame->depthReadBuffer.buffer;
	bufferBarrier.size = VK_WHOLE_SIZE;
	qvkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bufferBarrier, 0, NULL );
}

depthReadStatus_t vk_get_depth_read( unsigned int id, float *depth ) {
	unsigned int serial = id >> 8;
	int index = id & 255;
	int i;

	if ( !id || !vk.device ) {
		return DEPTHREAD_LOST;
	}

	for ( i = 0; i < VK_MAX_FRAMES; i++ ) {
		const vkFrame_t *frame = &vk.frames[i];

		if ( frame->resultSerial && ( frame->resultSerial & 0xFFFFFF ) == serial && index < frame->numDepthResults ) {
			*depth = frame->depthResults[index];
			return DEPTHREAD_DONE;
		}
		// recorded in a frame that is being recorded or still in flight
		if ( frame->serial && ( frame->serial & 0xFFFFFF ) == serial && index < frame->numDepthReads ) {
			return DEPTHREAD_PENDING;
		}
	}

	return DEPTHREAD_LOST;
}
