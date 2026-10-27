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

// vk_temporal.c -- the temporal path (r_temporal): what temporal
// anti-aliasing and screen space ambient occlusion need from a frame
//
// A scene (the frame's 3D views of the world, vk_begin_scene in vk.c) draws
// into images of its own, with render passes of its own (renderPassClear
// begins the scene and clears the color and the normals, renderPass
// continues it):
//  - color[current]: RGBA16F
//  - normal: A2B10G10R10, the view space normal (octahedral, rg), flags in
//    b (TEMPORAL_FLAG_*: a draw gave the normal, it moves), the class in
//    the 2 alpha bits (VK_TEMPORAL_CLASS_*)
//  - velocity: RG16F, this frame's position of the surface minus the last
//    frame's, in units of the viewport (x right, y down), where the flag
//    TEMPORAL_FLAG_MOTION says a draw that moves wrote it
//  - vk.depthImage, made sampled
// Only the pipelines that write depth without blending write the normals
// (VK_TemporalWritesBuffers), and of those only the ones that draw what
// moves (VK_PIPE_MOTION: entities with a last frame, skeletal models) the
// motion vectors. Translucent surfaces leave the buffers of the surface
// behind them. What the buffers don't hold, their readers work out from the
// depth (shaders/temporal_common.glsl): the camera's motion of what doesn't
// move, the normals the draws didn't give.
//
// When the scene ends (VK_TemporalResolve) shaders/hiz_build.comp makes the
// depth pyramid hiz[current] (view distances, the minimum and the maximum
// of what a texel covers, level 0 at half the framebuffer's size): levels 0
// and 1 from the depth buffer in one dispatch (PREPARE), then two levels per
// dispatch, then the small ones that are left in one (TAIL; vkt.hizPasses).
// Only when a reader needs it (VK_TEMPORAL_READ_DEPTH): it costs about as
// much as the composite.
// Then what resolves the scene (VK_TemporalConsumers): with r_taa temporal
// anti-aliasing (shaders/taa_resolve.comp) blends the scene with the last
// frame's result into taa[current]; when it is the pyramid's only reader it
// makes level 0 itself (the only level it reads) and the pyramid's passes
// are left out. Then the scene (or that result) is drawn
// into the frame's color target (VK_TemporalComposite,
// shaders/temporal_composite.frag), where the 2D drawing follows. The other
// color image, pyramid and anti-aliased image hold the last frame's: the
// history (level 0 of the pyramid is the last frame's depth).
//
// Layouts between scenes: the color, motion vector and normal images
// SHADER_READ_ONLY_OPTIMAL, the pyramids and taa GENERAL, the depth image
// DEPTH_STENCIL_ATTACHMENT_OPTIMAL as always. The images the scene draws
// into are never storage images (which can cost them their compression).

#include "vk_local.h"

vkTemporal_t	vkt;

cvar_t	*r_temporal;
cvar_t	*r_temporalJitter;
cvar_t	*r_temporalJitterSamples;
cvar_t	*r_temporalShow;
cvar_t	*r_temporalShowMip;
cvar_t	*r_temporalShowScale;
cvar_t	*r_taa;
cvar_t	*r_taaFeedback;
cvar_t	*r_taaSharpen;
cvar_t	*r_saturation;

// push constants of the passes, see the shaders
typedef struct {
	vkTemporalView_t	view;
	int		size[4];		// framebuffer width and height, the pyramid's level 0's
	int		count;			// the levels to make: 1 or 2
	int		unused[3];
} vkPreparePush_t;

typedef struct {
	int		size[2];		// of the level the dispatch reads
	int		count;			// the levels it makes
	int		unused;
} vkHizPush_t;

typedef struct {
	vkTemporalView_t	view;
	int		mode;			// r_temporalShow
	int		mip;
	float	scale;
	float	sharpen;		// r_taaSharpen when it shows the anti-aliased scene
	int		rect[4];		// the scene's views (vkt.rect)
	float	hdr[8];			// HDR output's expansion (VK_OutputExpansion)
} vkCompositePush_t;

typedef struct {
	vkTemporalView_t	view;
	int		rect[4];		// the scene's views (vkt.rect)
	float	params[4];		// r_taaFeedback, 1: the history is the frame before's, 1: so is the pyramid, 1: it makes level 0 of this frame's
	float	weights[12];	// of the 3 x 3 (9), for this frame's color at the pixel's center
} vkTaaPush_t;

#define VK_TEMPORAL_PREPARE_BINDINGS	4	// depth, normals (sampled), levels 0 and 1 (storage)
#define VK_TEMPORAL_COMPOSITE_BINDINGS	7
#define VK_TEMPORAL_TAA_BINDINGS		8	// color, motion vectors, normals, depth, history, last pyramid (sampled), result, this pyramid's level 0 (storage)
#define VK_TEMPORAL_TAA_HISTORY_BINDING	4	// read with the bilinear sampler
#define VK_TEMPORAL_TAA_GROUP			16	// the workgroup's width and height (taa_resolve.comp, GROUP)

void VK_TemporalRegisterCvars( void ) {
	r_temporal = ri.Cvar_Get( "r_temporal", "0", CVAR_ARCHIVE | CVAR_LATCH );
	ri.Cvar_SetDescription( r_temporal, "1: the 3D view is drawn with motion vectors, normals, a depth pyramid and the last frame's color and depth (for temporal anti-aliasing and ambient occlusion; see the renderer's README)" );
	r_temporalJitter = ri.Cvar_Get( "r_temporalJitter", "0", 0 );
	ri.Cvar_SetDescription( r_temporalJitter, "with r_temporal 1: the 3D view's projection moves by a sub-pixel offset every frame (Halton 2, 3)" );
	r_temporalJitterSamples = ri.Cvar_Get( "r_temporalJitterSamples", "8", 0 );
	ri.Cvar_CheckRange( r_temporalJitterSamples, 1, 64, qtrue );
	r_temporalShow = ri.Cvar_Get( "r_temporalShow", "0", CVAR_CHEAT );
	ri.Cvar_SetDescription( r_temporalShow, "with r_temporal 1, instead of the 3D view: 1 motion vectors, 2 normals, 3 classes, 4 depth pyramid level r_temporalShowMip, 5 last frame's color, 6 view distances, 7 last frame's view distances, 8 the scene without anti-aliasing" );
	ri.Cvar_CheckRange( r_temporalShow, 0, 8, qtrue );
	r_temporalShowMip = ri.Cvar_Get( "r_temporalShowMip", "0", CVAR_CHEAT );
	ri.Cvar_CheckRange( r_temporalShowMip, 0, VK_TEMPORAL_MAX_MIPS - 1, qtrue );
	r_temporalShowScale = ri.Cvar_Get( "r_temporalShowScale", "20", CVAR_CHEAT );
	r_taa = ri.Cvar_Get( "r_taa", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_taa, "with r_temporal 1: temporal anti-aliasing (the projection is jittered with it)" );
	r_taaFeedback = ri.Cvar_Get( "r_taaFeedback", "0.75", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_taaFeedback, "with r_taa 1: the weight of the last frames in the result; more is smoother and blurs more in motion" );
	ri.Cvar_CheckRange( r_taaFeedback, 0.5f, 0.98f, qfalse );
	r_taaSharpen = ri.Cvar_Get( "r_taaSharpen", "0.8", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_taaSharpen, "with r_taa 1: sharpens the anti-aliased image, 0 off to 1" );
	ri.Cvar_CheckRange( r_taaSharpen, 0.0f, 1.0f, qfalse );
	r_saturation = ri.Cvar_Get( "r_saturation", "1.1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_saturation, "with r_temporal 1: the 3D view's color saturation (not the HUD's), 1 unchanged; in SDR and with HDR output" );
	ri.Cvar_CheckRange( r_saturation, 0.0f, 2.0f, qfalse );
}

/*
===============
VK_TemporalWritesBuffers

Whether a pipeline of the scene writes the normals buffer (and with
VK_PIPE_MOTION the motion vectors): it writes depth (and color) without
blending, so it hides what is behind it. The others mask them and skip their
work (specialization constants 11 and 12 of the shaders)
===============
*/
qboolean VK_TemporalWritesBuffers( uint64_t bits ) {
	const uint64_t blend = ( 15ull << VK_PIPE_SRCBLEND_SHIFT ) | ( 15ull << VK_PIPE_DSTBLEND_SHIFT );

	return ( bits & VK_PIPE_DEPTHWRITE ) && !( bits & blend ) && ( bits & ( 7ull << VK_PIPE_COLORMASK_SHIFT ) );
}

/*
====================================================================

TARGETS

====================================================================
*/

/*
===============
VK_TemporalCheckDevice

Whether the temporal path is on: r_temporal, no multisampling, and what the
device must have. Called by VK_CreateRenderTargets once the sample count
and the depth format are chosen, before the depth image is created (it
must be sampled too)
===============
*/
void VK_TemporalCheckDevice( void ) {
	const VkFormatFeatureFlags attachment = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	const uint32_t pushSize = Q_max( Q_max( Q_max( VK_TEMPORAL_PUSH_END, (uint32_t)sizeof( vkPreparePush_t ) ), (uint32_t)sizeof( vkCompositePush_t ) ),
		(uint32_t)sizeof( vkTaaPush_t ) );
	const char *missing = NULL;

	Com_Memset( &vkt, 0, sizeof( vkt ) );
	if ( !r_temporal || !r_temporal->integer ) {
		return;
	}

	// the scene's color in RGBA16F: room above 1 for HDR to come (the
	// frame's 8-bit format was the same frames as without the path and 0.03
	// ms less at 2560x1440, but clips at 1; see the README)
	vkt.colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	vkt.velocityFormat = VK_FORMAT_R16G16_SFLOAT;
	vkt.normalFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	vkt.hizFormat = VK_FORMAT_R32G32_SFLOAT;
	// the anti-aliasing's results in RGBA16F, as the scene (A2B10G10R10 was
	// 0.025 ms less, with a little more shimmer and no room above 1)
	vkt.taaFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

	if ( vk.samples != VK_SAMPLE_COUNT_1_BIT ) {
		missing = "multisampling (r_ext_multisample) is on";
	} else if ( vk.deviceProperties.limits.maxPushConstantsSize < pushSize ) {
		missing = va( "the device has %u bytes of push constants, %u are needed", vk.deviceProperties.limits.maxPushConstantsSize, pushSize );
	} else if ( !vk.enabledFeatures.shaderStorageImageExtendedFormats ) {
		missing = "the device has no shaderStorageImageExtendedFormats";
	} else if ( !vk.enabledFeatures.independentBlend ) {
		missing = "the device has no independentBlend";
	} else if ( !VK_FormatHasFeatures( vkt.colorFormat, attachment | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT ) ) {
		missing = "RGBA16F can't be the scene's color target";
	} else if ( !VK_FormatHasFeatures( vkt.velocityFormat, attachment ) ) {
		missing = "RG16F can't hold the motion vectors";
	} else if ( !VK_FormatHasFeatures( vkt.normalFormat, attachment ) ) {
		missing = "A2B10G10R10 can't hold the normals";
	} else if ( !VK_FormatHasFeatures( vkt.hizFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
		| VK_FORMAT_FEATURE_TRANSFER_DST_BIT ) ) {
		missing = "RG32F can't hold the depth pyramid";
	} else if ( !VK_FormatHasFeatures( vkt.taaFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
		| VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT ) ) {
		missing = "RGBA16F can't hold the anti-aliasing's history";
	} else if ( !VK_FormatHasFeatures( vk.depthFormat, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ) ) {
		missing = "the depth format can't be sampled";
	}

	if ( missing ) {
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "r_temporal: off, %s\n", missing );
		return;
	}
	vkt.enabled = qtrue;
}

static VkImageView VK_TemporalLevelView( VkImage image, VkFormat format, VkImageAspectFlags aspect, int level ) {
	VkImageViewCreateInfo viewInfo;
	VkImageView view;

	Com_Memset( &viewInfo, 0, sizeof( viewInfo ) );
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = aspect;
	viewInfo.subresourceRange.baseMipLevel = level;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	VK_CHECK( qvkCreateImageView( vk.device, &viewInfo, NULL, &view ) );
	return view;
}

static void VK_TemporalCreateImage( vkTemporalImage_t *img, VkFormat format, VkImageUsageFlags usage, int width, int height, int mipLevels ) {
	VK_CreateImage( format, VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_ASPECT_COLOR_BIT, width, height, mipLevels,
		&img->image, &img->memory, &img->view );
}

static void VK_TemporalDestroyImage( vkTemporalImage_t *img ) {
	VK_DestroyTargetImage( &img->image, &img->memory, &img->view );
}

/*
===============
VK_TemporalCreateRenderPass

The scene's: color, motion vectors, normals, depth/stencil. The one that
begins a scene (clear) clears the color and the normals, and doesn't load
the motion vectors (only where the normals' flag says a draw wrote them are
they read); what they held doesn't matter, and the passes after the scenes
before read them: a dependency on those stages. The one that continues it
keeps everything, like vk.renderPass. They are compatible (they differ in
load operations and initial layouts only): the same pipelines and
framebuffers serve both
===============
*/
static VkRenderPass VK_TemporalCreateRenderPass( qboolean clear ) {
	VkAttachmentDescription attachments[4];
	VkAttachmentReference colorRefs[3], depthRef;
	VkSubpassDescription subpass;
	VkSubpassDependency dependencies[2];
	VkRenderPassCreateInfo info;
	VkRenderPass renderPass;
	VkPipelineStageFlags stages;
	VkAccessFlags access;
	int i;

	Com_Memset( attachments, 0, sizeof( attachments ) );
	attachments[0].format = vkt.colorFormat;
	attachments[1].format = vkt.velocityFormat;
	attachments[2].format = vkt.normalFormat;
	attachments[3].format = vk.depthFormat;
	for ( i = 0; i < 4; i++ ) {
		qboolean depth = i == 3;

		attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		if ( clear && !depth ) {
			attachments[i].loadOp = i == 1 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
		}
		attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[i].stencilLoadOp = depth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[i].stencilStoreOp = depth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[i].finalLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[i].initialLayout = clear && !depth ? VK_IMAGE_LAYOUT_UNDEFINED : attachments[i].finalLayout;
	}
	for ( i = 0; i < 3; i++ ) {
		colorRefs[i].attachment = i;
		colorRefs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	depthRef.attachment = 3;
	depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	Com_Memset( &subpass, 0, sizeof( subpass ) );
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 3;
	subpass.pColorAttachments = colorRefs;
	subpass.pDepthStencilAttachment = &depthRef;

	// as vk.renderPass: successive render passes read what the ones before wrote
	stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	Com_Memset( dependencies, 0, sizeof( dependencies ) );
	for ( i = 0; i < 2; i++ ) {
		dependencies[i].srcSubpass = i ? 0 : VK_SUBPASS_EXTERNAL;
		dependencies[i].dstSubpass = i ? VK_SUBPASS_EXTERNAL : 0;
		dependencies[i].srcStageMask = stages;
		dependencies[i].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[i].dstStageMask = stages;
		dependencies[i].dstAccessMask = access;
	}
	// the images were last read by the passes after the scenes before (the
	// clear's write waits for them); the same in both render passes, which
	// are only compatible with identical dependencies
	dependencies[0].srcStageMask |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = ARRAY_LEN( attachments );
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = ARRAY_LEN( dependencies );
	info.pDependencies = dependencies;
	VK_CHECK( qvkCreateRenderPass( vk.device, &info, NULL, &renderPass ) );
	return renderPass;
}

// the framebuffers and the sampled view of vk.depthImage, which
// VK_UpdateDepthTarget replaces
static void VK_TemporalCreateDepthObjects( void ) {
	VkFramebufferCreateInfo info;
	VkImageView views[4];
	int i;

	vkt.depthView = VK_TemporalLevelView( vk.depthImage, vk.depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 0 );

	for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
		views[0] = vkt.color[i].view;
		views[1] = vkt.velocity.view;
		views[2] = vkt.normal.view;
		views[3] = vk.depthView;
		Com_Memset( &info, 0, sizeof( info ) );
		info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		info.renderPass = vkt.renderPass;
		info.attachmentCount = 4;
		info.pAttachments = views;
		info.width = vk.width;
		info.height = vk.height;
		info.layers = 1;
		VK_CHECK( qvkCreateFramebuffer( vk.device, &info, NULL, &vkt.framebuffers[i] ) );
	}
}

void VK_TemporalDepthChanged( void ) {
	int i;

	if ( !vkt.enabled || !vkt.renderPass ) {
		return;
	}
	// frames in flight may still use them
	VK_DeferDestroy( VK_DESTROY_IMAGE_VIEW, (uint64_t)vkt.depthView, 0 );
	for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
		VK_DeferDestroy( VK_DESTROY_FRAMEBUFFER, (uint64_t)vkt.framebuffers[i], 0 );
	}
	VK_TemporalCreateDepthObjects();
}

// clears an image in the frame being recorded and leaves it in layout
static void VK_TemporalClearImage( VkCommandBuffer cmd, VkImage image, VkImageLayout layout, const float *color ) {
	VkImageSubresourceRange range;
	VkClearColorValue value;

	Com_Memset( &range, 0, sizeof( range ) );
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = VK_REMAINING_MIP_LEVELS;
	range.layerCount = 1;
	Com_Memcpy( value.float32, color, sizeof( value.float32 ) );

	VK_ImageBarrier( cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
	qvkCmdClearColorImage( cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range );
	VK_ImageBarrier( cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT );
}

/*
===============
VK_TemporalPlanPyramid

The dispatches of the pyramid after the prepare pass (levels 0 and 1): two
levels each, as long as the level they read is larger than
VK_TEMPORAL_TAIL_SIZE, then the rest in one (at most
VK_TEMPORAL_TAIL_IMAGES - 1 levels: 64, 32, ... 1)
===============
*/
static void VK_TemporalPlanPyramid( void ) {
	int level = Q_min( 1, vkt.hizLevels - 1 );

	vkt.numHizPasses = 0;
	while ( level < vkt.hizLevels - 1 && vkt.numHizPasses < VK_TEMPORAL_MAX_PASSES ) {
		vkHizPass_t *pass = &vkt.hizPasses[vkt.numHizPasses++];
		int w = Q_max( vkt.hizWidth >> level, 1 );
		int h = Q_max( vkt.hizHeight >> level, 1 );

		pass->src = level;
		if ( w <= VK_TEMPORAL_TAIL_SIZE && h <= VK_TEMPORAL_TAIL_SIZE ) {
			pass->tail = qtrue;
			pass->count = vkt.hizLevels - 1 - level;
		} else {
			pass->tail = qfalse;
			pass->count = Q_min( 2, vkt.hizLevels - 1 - level );
		}
		level += pass->count;
	}
}

/*
===============
VK_TemporalCreateTargets

The scene's images, render passes and framebuffers; at the end of
VK_CreateRenderTargets, in its frame. The color images and the pyramids
start black (the history); the motion vectors and the normals are written by
every scene before anything reads them
===============
*/
void VK_TemporalCreateTargets( void ) {
	static const float black[4] = { 0, 0, 0, 1 };
	static const float zero[4] = { 0, 0, 0, 0 };
	const VkImageUsageFlags attachment = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkSamplerCreateInfo samplerInfo;
	VkCommandBuffer cmd;
	int i, l, size;

	if ( !vkt.enabled ) {
		return;
	}

	// level 0 of the pyramid covers 2 x 2 pixels
	vkt.hizWidth = Q_max( vk.width >> 1, 1 );
	vkt.hizHeight = Q_max( vk.height >> 1, 1 );
	for ( size = Q_max( vkt.hizWidth, vkt.hizHeight ), vkt.hizLevels = 1; size > 1 && vkt.hizLevels < VK_TEMPORAL_MAX_MIPS; size >>= 1 ) {
		vkt.hizLevels++;
	}
	VK_TemporalPlanPyramid();

	for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
		VK_TemporalCreateImage( &vkt.color[i], vkt.colorFormat, attachment | VK_IMAGE_USAGE_TRANSFER_DST_BIT, vk.width, vk.height, 1 );
		VK_TemporalCreateImage( &vkt.hiz[i], vkt.hizFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			vkt.hizWidth, vkt.hizHeight, vkt.hizLevels );
		for ( l = 0; l < vkt.hizLevels; l++ ) {
			vkt.hizLevel[i][l] = VK_TemporalLevelView( vkt.hiz[i].image, vkt.hizFormat, VK_IMAGE_ASPECT_COLOR_BIT, l );
		}
	}
	VK_TemporalCreateImage( &vkt.velocity, vkt.velocityFormat, attachment, vk.width, vk.height, 1 );
	VK_TemporalCreateImage( &vkt.normal, vkt.normalFormat, attachment, vk.width, vk.height, 1 );
	// the anti-aliasing's results
	for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
		VK_TemporalCreateImage( &vkt.taa[i], vkt.taaFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			vk.width, vk.height, 1 );
	}

	vkt.renderPass = VK_TemporalCreateRenderPass( qfalse );
	vkt.renderPassClear = VK_TemporalCreateRenderPass( qtrue );
	VK_TemporalCreateDepthObjects();

	Com_Memset( &samplerInfo, 0, sizeof( samplerInfo ) );
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
	VK_CHECK( qvkCreateSampler( vk.device, &samplerInfo, NULL, &vkt.sampler ) );
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.maxLod = 0.0f;
	VK_CHECK( qvkCreateSampler( vk.device, &samplerInfo, NULL, &vkt.linearSampler ) );

	// the normals of the scene's draws that have none, read with a stride of 0
	VK_CreateBuffer( &vkt.zeroBuffer, sizeof( float ) * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0 );
	Com_Memset( vkt.zeroBuffer.mapped, 0, sizeof( float ) * 4 );

	// the layouts between scenes
	VK_BeginFrameIfNeeded();
	cmd = vk.frame->mainCmd;
	for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
		VK_TemporalClearImage( cmd, vkt.color[i].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, black );
		VK_TemporalClearImage( cmd, vkt.hiz[i].image, VK_IMAGE_LAYOUT_GENERAL, zero );
		VK_TemporalClearImage( cmd, vkt.taa[i].image, VK_IMAGE_LAYOUT_GENERAL, black );
	}

	vkt.current = 0;
	vkt.sceneSerial = 0;
	vkt.historySerial = 0;
	Com_Memset( vkt.hizSerial, 0, sizeof( vkt.hizSerial ) );
	Com_Memset( vkt.taaSerial, 0, sizeof( vkt.taaSerial ) );

	ri.Printf( PRINT_ALL, "Vulkan temporal path (r_temporal): %dx%d, depth pyramid of %d levels from %dx%d in %d dispatches\n",
		vk.width, vk.height, vkt.hizLevels, vkt.hizWidth, vkt.hizHeight, vkt.numHizPasses + 1 );
}

// called when nothing uses them anymore
void VK_TemporalDestroyTargets( void ) {
	int i, l;

	for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
		if ( vkt.framebuffers[i] ) {
			qvkDestroyFramebuffer( vk.device, vkt.framebuffers[i], NULL );
			vkt.framebuffers[i] = VK_NULL_HANDLE;
		}
		for ( l = 0; l < VK_TEMPORAL_MAX_MIPS; l++ ) {
			if ( vkt.hizLevel[i][l] ) {
				qvkDestroyImageView( vk.device, vkt.hizLevel[i][l], NULL );
				vkt.hizLevel[i][l] = VK_NULL_HANDLE;
			}
		}
		VK_TemporalDestroyImage( &vkt.color[i] );
		VK_TemporalDestroyImage( &vkt.hiz[i] );
		VK_TemporalDestroyImage( &vkt.taa[i] );
	}
	VK_TemporalDestroyImage( &vkt.velocity );
	VK_TemporalDestroyImage( &vkt.normal );
	if ( vkt.depthView ) {
		qvkDestroyImageView( vk.device, vkt.depthView, NULL );
		vkt.depthView = VK_NULL_HANDLE;
	}
	if ( vkt.renderPass ) {
		qvkDestroyRenderPass( vk.device, vkt.renderPass, NULL );
		vkt.renderPass = VK_NULL_HANDLE;
	}
	if ( vkt.renderPassClear ) {
		qvkDestroyRenderPass( vk.device, vkt.renderPassClear, NULL );
		vkt.renderPassClear = VK_NULL_HANDLE;
	}
	if ( vkt.sampler ) {
		qvkDestroySampler( vk.device, vkt.sampler, NULL );
		vkt.sampler = VK_NULL_HANDLE;
	}
	if ( vkt.linearSampler ) {
		qvkDestroySampler( vk.device, vkt.linearSampler, NULL );
		vkt.linearSampler = VK_NULL_HANDLE;
	}
	VK_DestroyBuffer( &vkt.zeroBuffer );
	vkt.active = qfalse;
}

/*
====================================================================

PIPELINES

====================================================================
*/

// count bindings of one descriptor each, of types[i]
static VkDescriptorSetLayout VK_TemporalSetLayout( const VkDescriptorType *types, int count, VkShaderStageFlags stages ) {
	VkDescriptorSetLayoutBinding bindings[VK_TEMPORAL_TAIL_IMAGES];
	VkDescriptorSetLayoutCreateInfo info;
	VkDescriptorSetLayout layout;
	int i;

	Com_Memset( bindings, 0, sizeof( bindings ) );
	for ( i = 0; i < count; i++ ) {
		bindings[i].binding = i;
		bindings[i].descriptorType = types[i];
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = stages;
	}
	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = count;
	info.pBindings = bindings;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &info, NULL, &layout ) );
	return layout;
}

static VkPipelineLayout VK_TemporalPipelineLayout( const VkDescriptorSetLayout *sets, int numSets, const VkPushConstantRange *ranges, int numRanges ) {
	VkPipelineLayoutCreateInfo info;
	VkPipelineLayout layout;

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	info.setLayoutCount = numSets;
	info.pSetLayouts = sets;
	info.pushConstantRangeCount = numRanges;
	info.pPushConstantRanges = ranges;
	VK_CHECK( qvkCreatePipelineLayout( vk.device, &info, NULL, &layout ) );
	return layout;
}

// a pipeline layout of one set and one push constant range
static VkPipelineLayout VK_TemporalSimpleLayout( VkDescriptorSetLayout set, VkShaderStageFlags stages, uint32_t pushSize ) {
	VkPushConstantRange range;

	range.stageFlags = stages;
	range.offset = 0;
	range.size = pushSize;
	return VK_TemporalPipelineLayout( &set, 1, &range, 1 );
}

static VkPipeline VK_TemporalComputePipeline( VkShaderModule module, VkPipelineLayout layout ) {
	VkComputePipelineCreateInfo info;
	VkPipeline pipeline;

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = module;
	info.stage.pName = "main";
	info.layout = layout;
	VK_CHECK( qvkCreateComputePipelines( vk.device, vk.pipelineCache, 1, &info, NULL, &pipeline ) );
	return pipeline;
}

// the scene into the frame's color target: a triangle over the framebuffer
// in vk.renderPass, no vertex input, no depth test, no blending
static VkPipeline VK_TemporalCompositePipeline( void ) {
	VkSpecializationMapEntry specEntry;
	VkSpecializationInfo specInfo;
	VkBool32 hdrExpand;
	VkPipelineShaderStageCreateInfo stages[2];
	VkPipelineVertexInputStateCreateInfo vertexInput;
	VkPipelineInputAssemblyStateCreateInfo inputAssembly;
	VkPipelineViewportStateCreateInfo viewportState;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depthStencil;
	VkPipelineColorBlendAttachmentState blendAttachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkDynamicState dynamicStates[2];
	VkPipelineDynamicStateCreateInfo dynamicState;
	VkGraphicsPipelineCreateInfo info;
	VkPipeline pipeline;

	Com_Memset( stages, 0, sizeof( stages ) );
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vkt.compositeVertexShader;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = vkt.compositeFragmentShader;
	stages[1].pName = "main";
	// HDR output: the scene expanded toward the display's peak
	hdrExpand = vko.enabled ? VK_TRUE : VK_FALSE;
	specEntry.constantID = 0;
	specEntry.offset = 0;
	specEntry.size = sizeof( hdrExpand );
	specInfo.mapEntryCount = 1;
	specInfo.pMapEntries = &specEntry;
	specInfo.dataSize = sizeof( hdrExpand );
	specInfo.pData = &hdrExpand;
	stages[1].pSpecializationInfo = &specInfo;

	Com_Memset( &vertexInput, 0, sizeof( vertexInput ) );
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	Com_Memset( &inputAssembly, 0, sizeof( inputAssembly ) );
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	Com_Memset( &viewportState, 0, sizeof( viewportState ) );
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;
	Com_Memset( &rasterization, 0, sizeof( rasterization ) );
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;
	Com_Memset( &multisample, 0, sizeof( multisample ) );
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	Com_Memset( &depthStencil, 0, sizeof( depthStencil ) );
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.maxDepthBounds = 1.0f;
	Com_Memset( &blendAttachment, 0, sizeof( blendAttachment ) );
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	Com_Memset( &blend, 0, sizeof( blend ) );
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blendAttachment;
	dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
	dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
	Com_Memset( &dynamicState, 0, sizeof( dynamicState ) );
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &inputAssembly;
	info.pViewportState = &viewportState;
	info.pRasterizationState = &rasterization;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depthStencil;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamicState;
	info.layout = vkt.compositeLayout;
	info.renderPass = vk.renderPass;
	VK_CHECK( qvkCreateGraphicsPipelines( vk.device, vk.pipelineCache, 1, &info, NULL, &pipeline ) );
	return pipeline;
}

static void VK_TemporalAllocateSets( VkDescriptorSetLayout layout, VkDescriptorSet *sets, int count ) {
	VkDescriptorSetLayout layouts[VK_TEMPORAL_MAX_PASSES];
	VkDescriptorSetAllocateInfo info;
	int i;

	for ( i = 0; i < count; i++ ) {
		layouts[i] = layout;
	}
	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	info.descriptorPool = vkt.pool;
	info.descriptorSetCount = count;
	info.pSetLayouts = layouts;
	VK_CHECK( qvkAllocateDescriptorSets( vk.device, &info, sets ) );
}

// writes the image descriptors of bindings 0 to count - 1 of a set; binding
// linear (-1 for none) with the bilinear sampler
static void VK_TemporalWriteSet( VkDescriptorSet set, const VkDescriptorType *types, const VkImageView *views,
	const VkImageLayout *layouts, int count, int linear )
{
	VkDescriptorImageInfo infos[VK_TEMPORAL_TAIL_IMAGES];
	VkWriteDescriptorSet writes[VK_TEMPORAL_TAIL_IMAGES];
	int i;

	Com_Memset( writes, 0, sizeof( writes ) );
	for ( i = 0; i < count; i++ ) {
		infos[i].sampler = i == linear ? vkt.linearSampler : vkt.sampler;
		infos[i].imageView = views[i];
		infos[i].imageLayout = layouts[i];
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = types[i];
		writes[i].pImageInfo = &infos[i];
	}
	qvkUpdateDescriptorSets( vk.device, count, writes, 0, NULL );
}

static const VkDescriptorType vk_prepareTypes[VK_TEMPORAL_PREPARE_BINDINGS] = {
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
};
static const VkDescriptorType vk_hizTypes[VK_TEMPORAL_TAIL_IMAGES] = {
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
};
static const VkDescriptorType vk_compositeTypes[VK_TEMPORAL_COMPOSITE_BINDINGS] = {
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
};
static const VkDescriptorType vk_taaTypes[VK_TEMPORAL_TAA_BINDINGS] = {
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
};

/*
===============
VK_TemporalWriteStaticSets

The sets that don't change until the targets are created again: those of
the pyramid's dispatches and of the composite, one of each per current
image. Frames in flight may bind them: they are never written again
===============
*/
static void VK_TemporalWriteStaticSets( void ) {
	VkImageView views[VK_TEMPORAL_TAIL_IMAGES];
	VkImageLayout layouts[VK_TEMPORAL_TAIL_IMAGES];
	int cur, i, l;

	for ( cur = 0; cur < VK_TEMPORAL_HISTORY; cur++ ) {
		// the levels a dispatch reads and writes, the last repeated in the
		// bindings it doesn't use
		for ( i = 0; i < vkt.numHizPasses; i++ ) {
			const vkHizPass_t *pass = &vkt.hizPasses[i];

			for ( l = 0; l < VK_TEMPORAL_TAIL_IMAGES; l++ ) {
				views[l] = vkt.hizLevel[cur][pass->src + Q_min( l, pass->count )];
				layouts[l] = VK_IMAGE_LAYOUT_GENERAL;
			}
			VK_TemporalWriteSet( vkt.hizSets[cur][i], vk_hizTypes, views, layouts, VK_TEMPORAL_TAIL_IMAGES, -1 );
		}

		views[0] = vkt.color[cur].view;
		views[1] = vkt.velocity.view;
		views[2] = vkt.normal.view;
		views[3] = vkt.hiz[cur].view;
		views[4] = vkt.color[cur ^ 1].view;
		views[5] = vkt.hiz[cur ^ 1].view;
		views[6] = vkt.color[cur].view;
		for ( l = 0; l < VK_TEMPORAL_COMPOSITE_BINDINGS; l++ ) {
			layouts[l] = l == 3 || l == 5 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
		VK_TemporalWriteSet( vkt.compositeSets[cur], vk_compositeTypes, views, layouts, VK_TEMPORAL_COMPOSITE_BINDINGS, -1 );

		// the same, showing the anti-aliased scene
		views[0] = vkt.taa[cur].view;
		layouts[0] = VK_IMAGE_LAYOUT_GENERAL;
		VK_TemporalWriteSet( vkt.compositeTaaSets[cur], vk_compositeTypes, views, layouts, VK_TEMPORAL_COMPOSITE_BINDINGS, -1 );
	}
}

/*
===============
VK_TemporalInitPipelines

After VK_InitPipelines and VK_TemporalCreateTargets: the scene's pipeline
layout and shaders (vk_pipeline.c creates its pipelines from
VK_PIPE_TEMPORAL keys), the passes after it, their descriptor sets, the ring
of the skinned draws' frame data
===============
*/
void VK_TemporalInitPipelines( void ) {
	VkDescriptorSetLayoutBinding bindings[4];
	VkDescriptorSetLayoutCreateInfo setLayoutInfo;
	VkDescriptorSetLayout sets[3];
	VkPushConstantRange ranges[2];
	VkDescriptorPoolSize poolSizes[3];
	VkDescriptorPoolCreateInfo poolInfo;
	int i;

	if ( !vkt.enabled ) {
		return;
	}

	// set 2 of the scene's pipelines: the dlights (fragment), the skin data,
	// this frame's and the last frame's frame data (vertex)
	Com_Memset( bindings, 0, sizeof( bindings ) );
	for ( i = 0; i < 4; i++ ) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = i ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	Com_Memset( &setLayoutInfo, 0, sizeof( setLayoutInfo ) );
	setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayoutInfo.bindingCount = 4;
	setLayoutInfo.pBindings = bindings;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &setLayoutInfo, NULL, &vkt.graphicsSetLayout ) );

	// vkPushConstants_t, then vkTemporalPush_t for the vertex stage: its
	// range covers the fragment stage's, whose pushes then name both stages
	sets[0] = sets[1] = vk.textureSetLayout;
	sets[2] = vkt.graphicsSetLayout;
	ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	ranges[0].offset = 0;
	ranges[0].size = VK_TEMPORAL_PUSH_END;
	ranges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	ranges[1].offset = VK_PUSH_VERTEX_SIZE;
	ranges[1].size = VK_PUSH_FRAGMENT_SIZE;
	vkt.pipelineLayout = VK_TemporalPipelineLayout( sets, 3, ranges, 2 );

	vkt.vertexShader = VK_CreateShaderModule( vk_spirv_generic_temporal_vert, vk_spirv_generic_temporal_vert_size );
	vkt.skinVertexShader = VK_CreateShaderModule( vk_spirv_skinned_temporal_vert, vk_spirv_skinned_temporal_vert_size );
	vkt.effectsVertexShader = VK_CreateShaderModule( vk_spirv_effects_temporal_vert, vk_spirv_effects_temporal_vert_size );
	if ( vk.enabledFeatures.shaderClipDistance ) {
		vkt.vertexShaderClip = VK_CreateShaderModule( vk_spirv_generic_temporal_clip_vert, vk_spirv_generic_temporal_clip_vert_size );
		vkt.skinVertexShaderClip = VK_CreateShaderModule( vk_spirv_skinned_temporal_clip_vert, vk_spirv_skinned_temporal_clip_vert_size );
		vkt.effectsVertexShaderClip = VK_CreateShaderModule( vk_spirv_effects_temporal_clip_vert, vk_spirv_effects_temporal_clip_vert_size );
	}
	vkt.fragmentShader = VK_CreateShaderModule( vk_spirv_generic_temporal_frag, vk_spirv_generic_temporal_frag_size );

	// the passes after the scene
	vkt.prepareSetLayout = VK_TemporalSetLayout( vk_prepareTypes, VK_TEMPORAL_PREPARE_BINDINGS, VK_SHADER_STAGE_COMPUTE_BIT );
	vkt.hizSetLayout = VK_TemporalSetLayout( vk_hizTypes, VK_TEMPORAL_TAIL_IMAGES, VK_SHADER_STAGE_COMPUTE_BIT );
	vkt.compositeSetLayout = VK_TemporalSetLayout( vk_compositeTypes, VK_TEMPORAL_COMPOSITE_BINDINGS, VK_SHADER_STAGE_FRAGMENT_BIT );
	vkt.prepareLayout = VK_TemporalSimpleLayout( vkt.prepareSetLayout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof( vkPreparePush_t ) );
	vkt.hizLayout = VK_TemporalSimpleLayout( vkt.hizSetLayout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof( vkHizPush_t ) );
	vkt.compositeLayout = VK_TemporalSimpleLayout( vkt.compositeSetLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof( vkCompositePush_t ) );
	vkt.taaSetLayout = VK_TemporalSetLayout( vk_taaTypes, VK_TEMPORAL_TAA_BINDINGS, VK_SHADER_STAGE_COMPUTE_BIT );
	vkt.taaLayout = VK_TemporalSimpleLayout( vkt.taaSetLayout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof( vkTaaPush_t ) );

	vkt.prepareShader = VK_CreateShaderModule( vk_spirv_hiz_prepare_comp, vk_spirv_hiz_prepare_comp_size );
	vkt.hizShader = VK_CreateShaderModule( vk_spirv_hiz_build_comp, vk_spirv_hiz_build_comp_size );
	vkt.hizTailShader = VK_CreateShaderModule( vk_spirv_hiz_tail_comp, vk_spirv_hiz_tail_comp_size );
	vkt.compositeVertexShader = VK_CreateShaderModule( vk_spirv_temporal_composite_vert, vk_spirv_temporal_composite_vert_size );
	vkt.compositeFragmentShader = VK_CreateShaderModule( vk_spirv_temporal_composite_frag, vk_spirv_temporal_composite_frag_size );
	vkt.preparePipeline = VK_TemporalComputePipeline( vkt.prepareShader, vkt.prepareLayout );
	vkt.hizPipeline = VK_TemporalComputePipeline( vkt.hizShader, vkt.hizLayout );
	vkt.hizTailPipeline = VK_TemporalComputePipeline( vkt.hizTailShader, vkt.hizLayout );
	vkt.compositePipeline = VK_TemporalCompositePipeline();
	vkt.taaShader = VK_CreateShaderModule( vk_spirv_taa_resolve_comp, vk_spirv_taa_resolve_comp_size );
	vkt.taaPipeline = VK_TemporalComputePipeline( vkt.taaShader, vkt.taaLayout );

	// per frame slot: set 2 of the scene, the prepare pass's and the
	// anti-aliasing's sets; per current image: the pyramid's dispatches' and
	// the composite's two
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSizes[0].descriptorCount = VK_MAX_FRAMES * 4;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[1].descriptorCount = VK_MAX_FRAMES * ( 2 + VK_TEMPORAL_TAA_BINDINGS - 2 ) + VK_TEMPORAL_HISTORY * 2 * VK_TEMPORAL_COMPOSITE_BINDINGS;
	poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	poolSizes[2].descriptorCount = VK_MAX_FRAMES * 4 + VK_TEMPORAL_HISTORY * VK_TEMPORAL_MAX_PASSES * VK_TEMPORAL_TAIL_IMAGES;
	Com_Memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = VK_MAX_FRAMES * 3 + VK_TEMPORAL_HISTORY * ( VK_TEMPORAL_MAX_PASSES + 2 );
	poolInfo.poolSizeCount = ARRAY_LEN( poolSizes );
	poolInfo.pPoolSizes = poolSizes;
	VK_CHECK( qvkCreateDescriptorPool( vk.device, &poolInfo, NULL, &vkt.pool ) );

	VK_TemporalAllocateSets( vkt.graphicsSetLayout, vkt.graphicsSets, VK_MAX_FRAMES );
	VK_TemporalAllocateSets( vkt.prepareSetLayout, vkt.prepareSets, VK_MAX_FRAMES );
	VK_TemporalAllocateSets( vkt.compositeSetLayout, vkt.compositeSets, VK_TEMPORAL_HISTORY );
	VK_TemporalAllocateSets( vkt.compositeSetLayout, vkt.compositeTaaSets, VK_TEMPORAL_HISTORY );
	VK_TemporalAllocateSets( vkt.taaSetLayout, vkt.taaSets, VK_MAX_FRAMES );
	for ( i = 0; i < VK_TEMPORAL_HISTORY; i++ ) {
		if ( vkt.numHizPasses ) {
			VK_TemporalAllocateSets( vkt.hizSetLayout, vkt.hizSets[i], vkt.numHizPasses );
		}
	}
	VK_TemporalWriteStaticSets();

	// the frame slots' buffers and one more
	for ( i = 0; i < VK_MAX_FRAMES; i++ ) {
		vkt.skinRing[i] = &vk.frames[i].skinBuffer;
	}
	VK_CreateBuffer( &vkt.skinExtra, VK_SKIN_FRAME_WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
	vkt.skinRing[VK_MAX_FRAMES] = &vkt.skinExtra;
	vkt.skinSlot = 0;
}

void VK_TemporalShutdownPipelines( void ) {
	VkShaderModule *modules[] = {
		&vkt.vertexShader, &vkt.vertexShaderClip, &vkt.skinVertexShader, &vkt.skinVertexShaderClip,
		&vkt.effectsVertexShader, &vkt.effectsVertexShaderClip, &vkt.fragmentShader,
		&vkt.prepareShader, &vkt.hizShader, &vkt.hizTailShader, &vkt.compositeVertexShader, &vkt.compositeFragmentShader,
		&vkt.taaShader
	};
	VkPipeline *pipelines[] = { &vkt.preparePipeline, &vkt.hizPipeline, &vkt.hizTailPipeline, &vkt.compositePipeline, &vkt.taaPipeline };
	VkPipelineLayout *pipelineLayouts[] = { &vkt.pipelineLayout, &vkt.prepareLayout, &vkt.hizLayout, &vkt.compositeLayout, &vkt.taaLayout };
	VkDescriptorSetLayout *setLayouts[] = { &vkt.graphicsSetLayout, &vkt.prepareSetLayout, &vkt.hizSetLayout, &vkt.compositeSetLayout,
		&vkt.taaSetLayout };
	unsigned int i;

	if ( !vk.device ) {
		return;
	}
	for ( i = 0; i < ARRAY_LEN( pipelines ); i++ ) {
		if ( *pipelines[i] ) {
			qvkDestroyPipeline( vk.device, *pipelines[i], NULL );
			*pipelines[i] = VK_NULL_HANDLE;
		}
	}
	for ( i = 0; i < ARRAY_LEN( modules ); i++ ) {
		if ( *modules[i] ) {
			qvkDestroyShaderModule( vk.device, *modules[i], NULL );
			*modules[i] = VK_NULL_HANDLE;
		}
	}
	for ( i = 0; i < ARRAY_LEN( pipelineLayouts ); i++ ) {
		if ( *pipelineLayouts[i] ) {
			qvkDestroyPipelineLayout( vk.device, *pipelineLayouts[i], NULL );
			*pipelineLayouts[i] = VK_NULL_HANDLE;
		}
	}
	if ( vkt.pool ) {
		qvkDestroyDescriptorPool( vk.device, vkt.pool, NULL );
		vkt.pool = VK_NULL_HANDLE;
	}
	for ( i = 0; i < ARRAY_LEN( setLayouts ); i++ ) {
		if ( *setLayouts[i] ) {
			qvkDestroyDescriptorSetLayout( vk.device, *setLayouts[i], NULL );
			*setLayouts[i] = VK_NULL_HANDLE;
		}
	}
	// the frame slots' buffers go with the frames
	VK_DestroyBuffer( &vkt.skinExtra );
	Com_Memset( vkt.skinRing, 0, sizeof( vkt.skinRing ) );
	Com_Memset( vkt.graphicsSets, 0, sizeof( vkt.graphicsSets ) );
	Com_Memset( vkt.prepareSets, 0, sizeof( vkt.prepareSets ) );
	Com_Memset( vkt.hizSets, 0, sizeof( vkt.hizSets ) );
	Com_Memset( vkt.compositeSets, 0, sizeof( vkt.compositeSets ) );
	Com_Memset( vkt.compositeTaaSets, 0, sizeof( vkt.compositeTaaSets ) );
	Com_Memset( vkt.taaSets, 0, sizeof( vkt.taaSets ) );
}

/*
====================================================================

FRAMES AND SCENES

====================================================================
*/

static void VK_TemporalWriteBuffer( VkWriteDescriptorSet *write, VkDescriptorBufferInfo *info, VkDescriptorSet set, int binding, VkBuffer buffer ) {
	info->buffer = buffer;
	info->offset = 0;
	info->range = VK_WHOLE_SIZE;
	Com_Memset( write, 0, sizeof( *write ) );
	write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write->dstSet = set;
	write->dstBinding = binding;
	write->descriptorCount = 1;
	write->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write->pBufferInfo = info;
}

/*
===============
VK_TemporalBeginFrame

At the start of a frame, once its slot is free: the frame data of the
skinned draws goes to the next buffer of the ring, and set 2 of the slot
(both the one of the normal pipelines and the scene's) points at it; the
scene's also at the last frame's
===============
*/
void VK_TemporalBeginFrame( vkFrame_t *frame, VkBuffer skinData ) {
	VkWriteDescriptorSet writes[5];
	VkDescriptorBufferInfo infos[5];
	VkDescriptorSet set;
	int prev;

	if ( !vkt.enabled || !vkt.graphicsSets[0] || !frame->dlightSet ) {
		return;
	}
	vkt.skinSlot = (int)( frame->serial % VK_SKIN_RING );
	prev = (int)( ( frame->serial + VK_SKIN_RING - 1 ) % VK_SKIN_RING );
	set = vkt.graphicsSets[frame - vk.frames];

	VK_TemporalWriteBuffer( &writes[0], &infos[0], frame->dlightSet, 2, vkt.skinRing[vkt.skinSlot]->buffer );
	VK_TemporalWriteBuffer( &writes[1], &infos[1], set, 0, frame->dlightBuffer.buffer );
	VK_TemporalWriteBuffer( &writes[2], &infos[2], set, 1, skinData );
	VK_TemporalWriteBuffer( &writes[3], &infos[3], set, 2, vkt.skinRing[vkt.skinSlot]->buffer );
	VK_TemporalWriteBuffer( &writes[4], &infos[4], set, 3, vkt.skinRing[prev]->buffer );
	qvkUpdateDescriptorSets( vk.device, 5, writes, 0, NULL );
}

static void VK_TemporalImageBarrier( VkImageMemoryBarrier *b, VkImage image, VkImageAspectFlags aspect,
	VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess )
{
	Com_Memset( b, 0, sizeof( *b ) );
	b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b->srcAccessMask = srcAccess;
	b->dstAccessMask = dstAccess;
	b->oldLayout = oldLayout;
	b->newLayout = newLayout;
	b->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b->image = image;
	b->subresourceRange.aspectMask = aspect;
	b->subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
	b->subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
}

// what compute shaders wrote, to the stages that read (or write) it next
static void VK_TemporalComputeBarrier( VkCommandBuffer cmd, VkPipelineStageFlags dstStages ) {
	VkMemoryBarrier memory;

	Com_Memset( &memory, 0, sizeof( memory ) );
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	qvkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dstStages, 0, 1, &memory, 0, NULL, 0, NULL );
}

/*
===============
VK_TemporalConsumers

Where what resolves the scene goes, between the pyramid and the composite,
outside of a render pass: this frame's color, motion vectors and normals,
the last frame's color (when vkt.historySerial is the frame before and
vkt.view.params[2] says the view has a last frame), the view (vkt.view,
TemporalView of shaders/temporal_common.glsl, with its camera motion and
derived normals), the jitter (vkt.jitterPixels); with VK_TEMPORAL_READ_DEPTH
in vkt.readers (set it when the reader is created) the depth image
(vkt.depthView, read only) and this frame's depth pyramid, and the last
frame's when vkt.hizSerial says it was made in the frame before.

Temporal anti-aliasing (VK_TemporalTaa, which needs the depth): this frame's
scene and the last frame's result (taa[current ^ 1], when it is the frame
before's and the view has a last frame) into taa[current], which the
composite shows then (vkt.taaRan); with level0 it also makes level 0 of this
frame's pyramid, which no pass made then (its only reader is the next
frame's anti-aliasing). VK_TemporalResolve puts taa[current] into GENERAL
before and makes its writes visible after. Ambient occlusion is to come.
===============
*/
static void VK_TemporalConsumers( VkCommandBuffer cmd, qboolean level0 ) {
	const int cur = vkt.current;
	const VkDescriptorSet set = vkt.taaSets[vk.frameIndex];
	VkImageView views[VK_TEMPORAL_TAA_BINDINGS];
	VkImageLayout layouts[VK_TEMPORAL_TAA_BINDINGS];
	vkTaaPush_t push;
	float sum;
	int x, y;

	// the depth image's view can change between frames; this frame slot's
	// last use of the set is done
	views[0] = vkt.color[cur].view;
	views[1] = vkt.velocity.view;
	views[2] = vkt.normal.view;
	views[3] = vkt.depthView;
	views[4] = vkt.taa[cur ^ 1].view;
	views[5] = vkt.hiz[cur ^ 1].view;
	views[6] = vkt.taa[cur].view;
	views[7] = vkt.hizLevel[cur][0];
	layouts[0] = layouts[1] = layouts[2] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	layouts[3] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	layouts[4] = layouts[5] = layouts[6] = layouts[7] = VK_IMAGE_LAYOUT_GENERAL;
	VK_TemporalWriteSet( set, vk_taaTypes, views, layouts, VK_TEMPORAL_TAA_BINDINGS, VK_TEMPORAL_TAA_HISTORY_BINDING );

	Com_Memset( &push, 0, sizeof( push ) );
	push.view = vkt.view;
	Com_Memcpy( push.rect, vkt.rect, sizeof( push.rect ) );
	push.params[0] = r_taaFeedback->value;
	push.params[1] = vkt.taaSerial[cur ^ 1] && vkt.taaSerial[cur ^ 1] + 1 == vk.frameSerial && vkt.view.params[2] != 0.0f ? 1.0f : 0.0f;
	// this frame's color at the pixel's center: each pixel's sample is at its
	// center minus the jitter, weighted by its distance to the center (a
	// Gaussian close to Blackman-Harris), the same for every pixel
	for ( y = 0, sum = 0.0f; y < 3; y++ ) {
		for ( x = 0; x < 3; x++ ) {
			float ox = x - 1 - vkt.jitterPixels[0];
			float oy = y - 1 - vkt.jitterPixels[1];

			push.weights[y * 3 + x] = expf( -2.29f * ( ox * ox + oy * oy ) );
			sum += push.weights[y * 3 + x];
		}
	}
	for ( x = 0; x < 9; x++ ) {
		push.weights[x] /= sum;
	}
	push.params[2] = vkt.hizSerial[cur ^ 1] && vkt.hizSerial[cur ^ 1] + 1 == vk.frameSerial ? 1.0f : 0.0f;
	push.params[3] = level0 ? 1.0f : 0.0f;

	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.taaPipeline );
	qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.taaLayout, 0, 1, &set, 0, NULL );
	qvkCmdPushConstants( cmd, vkt.taaLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( push ), &push );
	qvkCmdDispatch( cmd, ( vkt.rect[2] + VK_TEMPORAL_TAA_GROUP - 1 ) / VK_TEMPORAL_TAA_GROUP,
		( vkt.rect[3] + VK_TEMPORAL_TAA_GROUP - 1 ) / VK_TEMPORAL_TAA_GROUP, 1 );

	vkt.taaSerial[cur] = vk.frameSerial;
	vkt.taaRan = qtrue;
}

// the far distance of the primary view's projection (GL: z_ndc 1)
static float VK_TemporalFar( void ) {
	const float *p = vkt.projection;

	return fabs( 1.0f + p[10] ) > 1e-9f ? p[14] / ( 1.0f + p[10] ) : 65536.0f;
}

/*
===============
VK_TemporalReaders

What the readers of this scene's buffers need: theirs (vkt.readers), and
what r_temporalShow's view reads (the motion vectors and the normals of what
the draws left to the readers, the pyramid itself). The anti-aliasing's
(VK_TemporalTaa) are apart: the depth image and level 0 of the pyramid only,
which it makes itself when nothing else needs the pyramid
===============
*/
static int VK_TemporalReaders( void ) {
	int readers = vkt.readers;

	switch ( r_temporalShow->integer ) {
	case 1: case 2: case 4: case 6: case 7:
		readers |= VK_TEMPORAL_READ_DEPTH;
		break;
	}
	return readers;
}

// whether the anti-aliasing resolves this scene
static qboolean VK_TemporalTaa( void ) {
	return vkt.taaOn && vkt.rect[2] && vkt.rect[3];
}

/*
===============
VK_TemporalResolve

After the scene's last render pass, outside of a render pass: the depth
pyramid (levels 0 and 1 from the depth buffer, then the others) when a
reader needs it, then what resolves the scene; the depth image goes back to
an attachment for the 2D drawing that follows. When the anti-aliasing is the
pyramid's only reader it makes level 0 itself in its pass (it has the view
distances of its pixels already; the pyramid's passes and barriers cost
about as much as a fifth of it), unless the views don't cover the
framebuffer (its dispatch covers the views only)
===============
*/
void VK_TemporalResolve( VkCommandBuffer cmd ) {
	const int cur = vkt.current;
	const VkDescriptorSet prepareSet = vkt.prepareSets[vk.frameIndex];
	const VkPipelineStageFlags depthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	const VkPipelineStageFlags readers = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	const qboolean taa = VK_TemporalTaa();
	qboolean pyramid = ( VK_TemporalReaders() & VK_TEMPORAL_READ_DEPTH ) != 0;
	VkImageMemoryBarrier barriers[6];
	VkMemoryBarrier memory;
	VkImageView views[VK_TEMPORAL_PREPARE_BINDINGS];
	VkImageLayout layouts[VK_TEMPORAL_PREPARE_BINDINGS];
	VkPipeline bound = VK_NULL_HANDLE;
	vkPreparePush_t prepare;
	vkHizPush_t hiz;
	int i, w, h, first, levels, count;

	vkt.taaRan = qfalse;
	if ( taa && !pyramid && ( vkt.rect[0] || vkt.rect[1] || vkt.rect[2] != vk.width || vkt.rect[3] != vk.height ) ) {
		pyramid = qtrue;
	}
	if ( !pyramid && !taa ) {
		// the scene's images to their readers, the depth image stays as it is
		VK_TemporalImageBarrier( &barriers[0], vkt.normal.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
		VK_TemporalImageBarrier( &barriers[1], vkt.velocity.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
		VK_TemporalImageBarrier( &barriers[2], vkt.color[cur].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
		qvkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, readers, 0, 0, NULL, 0, NULL, 3, barriers );
		return;
	}
	// this frame's level 0 at least (the anti-aliasing's or the prepare pass's)
	vkt.hizSerial[cur] = vk.frameSerial;

	// the view's depth range and far distance, as the scene's draws used them
	vkt.view.depth[0] = vkt.projection[10];
	vkt.view.depth[1] = vkt.projection[14];
	vkt.view.depth[2] = vk_depth_floor();
	vkt.view.depth[3] = VK_TemporalFar();

	if ( pyramid ) {
		// the prepare pass's set: the depth image's view can change between
		// frames; this frame slot's last use of it is done
		views[0] = vkt.depthView;
		views[1] = vkt.normal.view;
		views[2] = vkt.hizLevel[cur][0];
		views[3] = vkt.hizLevel[cur][Q_min( 1, vkt.hizLevels - 1 )];
		layouts[0] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		layouts[1] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		layouts[2] = layouts[3] = VK_IMAGE_LAYOUT_GENERAL;
		VK_TemporalWriteSet( prepareSet, vk_prepareTypes, views, layouts, VK_TEMPORAL_PREPARE_BINDINGS, -1 );
	}

	// what the scene wrote, to its readers; this frame's pyramid was last
	// read (as the history) by the frame before, taa[cur] (the result two
	// frames ago) by that frame's composite and the last frame's resolve:
	// both written over
	VK_TemporalImageBarrier( &barriers[0], vk.depthImage, vk.depthAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
	VK_TemporalImageBarrier( &barriers[1], vkt.normal.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
	VK_TemporalImageBarrier( &barriers[2], vkt.velocity.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
	VK_TemporalImageBarrier( &barriers[3], vkt.color[cur].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
	VK_TemporalImageBarrier( &barriers[4], vkt.hiz[cur].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT );
	count = 5;
	if ( taa ) {
		VK_TemporalImageBarrier( &barriers[count++], vkt.taa[cur].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT );
	}
	qvkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | depthStages | readers, readers,
		0, 0, NULL, 0, NULL, count, barriers );

	if ( pyramid ) {
		// levels 0 and 1, an invocation per texel of level 0 (8 x 8 a workgroup)
		levels = Q_min( 2, vkt.hizLevels );
		Com_Memset( &prepare, 0, sizeof( prepare ) );
		prepare.view = vkt.view;
		prepare.size[0] = vk.width;
		prepare.size[1] = vk.height;
		prepare.size[2] = vkt.hizWidth;
		prepare.size[3] = vkt.hizHeight;
		prepare.count = levels;
		w = vkt.hizWidth;
		h = vkt.hizHeight;
		qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.preparePipeline );
		qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.prepareLayout, 0, 1, &prepareSet, 0, NULL );
		qvkCmdPushConstants( cmd, vkt.prepareLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( prepare ), &prepare );
		qvkCmdDispatch( cmd, ( w + 7 ) / 8, ( h + 7 ) / 8, 1 );
		VK_TemporalComputeBarrier( cmd, vkt.numHizPasses ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : readers );

		// the other levels
		for ( i = 0; i < vkt.numHizPasses; i++ ) {
			const vkHizPass_t *pass = &vkt.hizPasses[i];
			VkPipeline pipeline = pass->tail ? vkt.hizTailPipeline : vkt.hizPipeline;

			if ( pipeline != bound ) {
				qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline );
				bound = pipeline;
			}
			Com_Memset( &hiz, 0, sizeof( hiz ) );
			hiz.size[0] = Q_max( vkt.hizWidth >> pass->src, 1 );
			hiz.size[1] = Q_max( vkt.hizHeight >> pass->src, 1 );
			hiz.count = pass->count;
			qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkt.hizLayout, 0, 1, &vkt.hizSets[cur][i], 0, NULL );
			qvkCmdPushConstants( cmd, vkt.hizLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( hiz ), &hiz );
			if ( pass->tail ) {
				qvkCmdDispatch( cmd, 1, 1, 1 );
			} else {
				// an invocation per texel of the first level it makes
				first = pass->src + 1;
				w = Q_max( vkt.hizWidth >> first, 1 );
				h = Q_max( vkt.hizHeight >> first, 1 );
				qvkCmdDispatch( cmd, ( w + 7 ) / 8, ( h + 7 ) / 8, 1 );
			}
			VK_TemporalComputeBarrier( cmd, i + 1 < vkt.numHizPasses ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : readers );
		}
	}

	if ( taa ) {
		VK_TemporalConsumers( cmd, !pyramid );
	}

	// the 2D drawing and the next scene draw with the depth image again; what
	// the anti-aliasing wrote to the composite and to the next frame's (its
	// history, level 0 of the pyramid)
	VK_TemporalImageBarrier( &barriers[0], vk.depthImage, vk.depthAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );
	Com_Memset( &memory, 0, sizeof( memory ) );
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	qvkCmdPipelineBarrier( cmd, readers, depthStages | ( taa ? readers : 0 ), 0, taa ? 1 : 0, &memory, 0, NULL, 1, barriers );
}

/*
===============
VK_TemporalComposite

In vk.renderPass, after VK_TemporalResolve: the scene into the frame's color
target, over the rectangle of its views (or what r_temporalShow shows): the
anti-aliased scene when the resolve made it, sharpened (r_taaSharpen).
The caller rebinds what it draws with next
===============
*/
void VK_TemporalComposite( VkCommandBuffer cmd ) {
	vkCompositePush_t push;
	VkViewport viewport;
	VkRect2D scissor;

	viewport.x = 0;
	viewport.y = 0;
	viewport.width = vk.width;
	viewport.height = vk.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	scissor.offset.x = vkt.rect[0];
	scissor.offset.y = vkt.rect[1];
	scissor.extent.width = vkt.rect[2];
	scissor.extent.height = vkt.rect[3];
	if ( !scissor.extent.width || !scissor.extent.height ) {
		return;
	}

	Com_Memset( &push, 0, sizeof( push ) );
	push.view = vkt.view;
	push.mode = r_temporalShow->integer;
	push.mip = r_temporalShowMip->integer;
	push.scale = r_temporalShowScale->value;
	push.sharpen = vkt.taaRan ? r_taaSharpen->value : 0.0f;
	Com_Memcpy( push.rect, vkt.rect, sizeof( push.rect ) );
	VK_OutputExpansion( push.hdr );

	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkt.compositePipeline );
	qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkt.compositeLayout, 0, 1,
		vkt.taaRan ? &vkt.compositeTaaSets[vkt.current] : &vkt.compositeSets[vkt.current], 0, NULL );
	qvkCmdPushConstants( cmd, vkt.compositeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( push ), &push );
	qvkCmdSetViewport( cmd, 0, 1, &viewport );
	qvkCmdSetScissor( cmd, 0, 1, &scissor );
	qvkCmdDraw( cmd, 3, 1, 0, 0 );
}
