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

// vk_output.c -- HDR output (r_hdrOutput): the frame to the swapchain
// through a pass of its own (see vk_local.h, OUTPUT PASS).
//
// Whether the display shows HDR is asked of Vulkan and SDL only, on every
// system alike: the surface lists the HDR10 (ST 2084) and scRGB (extended
// sRGB linear) formats when the window system can take them (on Wayland the
// driver's WSI does it through wp_color_manager_v1: scRGB through
// create_windows_scrgb, HDR10 through a parametric image description with
// BT.2020 primaries and PQ; on Windows through DXGI), and SDL says whether
// the window is on a display in HDR now (SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN),
// with its SDR white level and headroom. The window's state is read every
// frame: when it changes (HDR turned on or off, the window moved to another
// display) the swapchain is made again in the other mode.
//
// The 3D scene is expanded toward the peak in temporal_composite.frag
// (VK_OutputExpansion), the HUD stays at paper white.

#include <SDL3/SDL.h>

#include "vk_local.h"

extern SDL_Window *SDL_window;

vkOutput_t	vko;

cvar_t	*r_hdrOutput;
cvar_t	*r_hdrPaperWhite;
cvar_t	*r_hdrPeak;
cvar_t	*r_hdrExpand;
cvar_t	*r_hdrExpandKnee;
cvar_t	*r_hdrSkyBoost;
cvar_t	*r_hdrEffectsBoost;
cvar_t	*r_hdrBlack;
cvar_t	*r_hdrOutputTest;

#define VK_HDR_DEFAULT_WHITE	203.0f		// nits of paper white without SDL's (ITU-R BT.2408)
#define VK_HDR_DEFAULT_PEAK		1000.0f

void VK_OutputRegisterCvars( void ) {
	r_hdrOutput = ri.Cvar_Get( "r_hdrOutput", "0", CVAR_ARCHIVE | CVAR_LATCH );
	ri.Cvar_SetDescription( r_hdrOutput, "0 off; 1 HDR output where the display is in HDR (HDR10, else scRGB), SDR otherwise; 2 the HDR look on an SDR display (with r_temporal 1: the expansion and r_hdrBlack put back into SDR); 3 HDR10 and 4 scRGB whatever the window's state (tests)" );
	ri.Cvar_CheckRange( r_hdrOutput, 0, 4, qtrue );
	r_hdrPaperWhite = ri.Cvar_Get( "r_hdrPaperWhite", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_hdrPaperWhite, "with HDR output: the brightness of the whole picture, in nits of the game's white (the menus, the HUD and the 3D view; the expansion goes from there toward r_hdrPeak); 0 the system's SDR white level" );
	ri.Cvar_CheckRange( r_hdrPaperWhite, 0.0f, 1000.0f, qfalse );
	r_hdrPeak = ri.Cvar_Get( "r_hdrPeak", "1000", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_hdrPeak, "with HDR output: the brightest the 3D view goes, in nits (find your display's with r_hdrOutputTest 1); 0 what the system reports, which is often lower than the display can show" );
	ri.Cvar_CheckRange( r_hdrPeak, 0.0f, 10000.0f, qfalse );
	r_hdrExpand = ri.Cvar_Get( "r_hdrExpand", "0.15", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_hdrExpand, "with HDR output and r_temporal 1: how far the 3D view's bright parts go toward the peak, 0 none (SDR at paper white) to 1 (the game's white at the peak)" );
	ri.Cvar_CheckRange( r_hdrExpand, 0.0f, 1.0f, qfalse );
	r_hdrExpandKnee = ri.Cvar_Get( "r_hdrExpandKnee", "0.1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_hdrExpandKnee, "with HDR output: the level (linear, of the game's white) above which the 3D view is expanded; lower lifts more of the image" );
	ri.Cvar_CheckRange( r_hdrExpandKnee, 0.02f, 0.95f, qfalse );
	r_hdrSkyBoost = ri.Cvar_Get( "r_hdrSkyBoost", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_hdrSkyBoost, "with HDR output: r_hdrExpand times this for the sky" );
	ri.Cvar_CheckRange( r_hdrSkyBoost, 0.0f, 4.0f, qfalse );
	r_hdrEffectsBoost = ri.Cvar_Get( "r_hdrEffectsBoost", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_hdrEffectsBoost, "with HDR output: the 3D view's range above the game's white (where additive effects stack) times this" );
	ri.Cvar_CheckRange( r_hdrEffectsBoost, 0.0f, 8.0f, qfalse );
	r_hdrBlack = ri.Cvar_Get( "r_hdrBlack", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_hdrBlack, "with HDR output: the black level (calibrate with r_hdrOutputTest 1); higher deepens the darkest shades, lower lifts them, the midtones stay" );
	ri.Cvar_CheckRange( r_hdrBlack, -0.5f, 1.0f, qfalse );
	r_hdrOutputTest = ri.Cvar_Get( "r_hdrOutputTest", "0", 0 );
	ri.Cvar_SetDescription( r_hdrOutputTest, "with HDR output, calibration screens: 1 black (adjust r_hdrBlack until the box is barely visible), 2 peak (raise r_hdrPeak until the box disappears), 3 patches of 100 to 4000 nits and a ramp to twice r_hdrPeak; 0 off" );
	ri.Cvar_CheckRange( r_hdrOutputTest, 0, 3, qtrue );
}

// HDR output: r_hdrOutput 1, 3 or 4 (2 is the HDR look in SDR, which needs
// no output pass)
qboolean VK_OutputWanted( void ) {
	return r_hdrOutput && r_hdrOutput->integer != 0 && r_hdrOutput->integer != 2;
}

/*
====================================================================

THE WINDOW'S HDR STATE

====================================================================
*/

static float vk_sdlLevel = 1.0f, vk_sdlHeadroom = 1.0f;	// SDL's last values, for the console

// SDL's state of the window; white and peak in nits (what SDL reports or
// the defaults; the cvars apply in VK_OutputUpdate)
static qboolean VK_WindowHdr( float *white, float *peak ) {
	SDL_PropertiesID props = SDL_window ? SDL_GetWindowProperties( SDL_window ) : 0;
	qboolean hdr = qfalse;
	float level = 1.0f, headroom = 1.0f;

	if ( props ) {
		hdr = SDL_GetBooleanProperty( props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false ) ? qtrue : qfalse;
		level = SDL_GetFloatProperty( props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f );
		headroom = SDL_GetFloatProperty( props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f );
	}
	vk_sdlLevel = level;
	vk_sdlHeadroom = headroom;
	// SDR white in units of scRGB's 80 nits; 1 (or nothing) when SDL doesn't know
	*white = level > 1.01f ? level * 80.0f : VK_HDR_DEFAULT_WHITE;
	*peak = headroom > 1.01f ? headroom * *white : VK_HDR_DEFAULT_PEAK;
	return hdr;
}

static const char *VK_HdrModeName( vkHdrMode_t mode ) {
	switch ( mode ) {
	case VK_HDR_PQ: return "HDR10 (ST 2084)";
	case VK_HDR_SCRGB: return "scRGB (extended sRGB linear)";
	default: return "SDR";
	}
}

static const char *VK_OutputFormatName( VkFormat format ) {
	switch ( format ) {
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10";
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10";
	case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
	default: return va( "format %d", (int)format );
	}
}

static void VK_OutputLevels( void );

static qboolean VK_OutputPairFits( const VkSurfaceFormatKHR *f, vkHdrMode_t mode ) {
	if ( mode == VK_HDR_PQ ) {
		if ( f->colorSpace != VK_COLOR_SPACE_HDR10_ST2084_EXT
			|| ( f->format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 && f->format != VK_FORMAT_A2R10G10B10_UNORM_PACK32 ) ) {
			return qfalse;
		}
	} else if ( f->colorSpace != VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT || f->format != VK_FORMAT_R16G16B16A16_SFLOAT ) {
		return qfalse;
	}
	return VK_FormatHasFeatures( f->format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT );
}

static const VkSurfaceFormatKHR *VK_OutputFindPair( const VkSurfaceFormatKHR *formats, uint32_t count, vkHdrMode_t mode ) {
	uint32_t i;

	for ( i = 0; i < count; i++ ) {
		if ( VK_OutputPairFits( &formats[i], mode ) ) {
			return &formats[i];
		}
	}
	return NULL;
}

/*
===============
VK_OutputChooseFormat

The swapchain's format with the output pass (VK_ChooseSurfaceFormat): an
HDR pair when the surface has one and the window is in HDR (or r_hdrOutput
forces a mode); qfalse for an SDR swapchain (vko.mode VK_HDR_SDR, the
reason in vko.reason). Prints the surface's HDR pairs once, and the mode
when it changes
===============
*/
qboolean VK_OutputChooseFormat( const VkSurfaceFormatKHR *formats, uint32_t count, VkSurfaceFormatKHR *chosen ) {
	const VkSurfaceFormatKHR *pq = VK_OutputFindPair( formats, count, VK_HDR_PQ );
	const VkSurfaceFormatKHR *scrgb = VK_OutputFindPair( formats, count, VK_HDR_SCRGB );
	const VkSurfaceFormatKHR *pick = NULL;
	vkHdrMode_t mode = VK_HDR_SDR, lastMode = vko.mode;
	const char *reason = NULL, *lastReason = vko.reason;
	float white, peak;

	vko.windowHdr = VK_WindowHdr( &white, &peak );

	if ( !vko.listed ) {
		vko.listed = qtrue;
		ri.Printf( PRINT_ALL, "HDR output (r_hdrOutput): the window's surface offers %s%s%s%s; the window is%s in HDR\n",
			pq ? "HDR10 in " : "", pq ? VK_OutputFormatName( pq->format ) : "",
			pq && scrgb ? ", " : "", scrgb ? "scRGB in RGBA16F" : ( pq ? "" : "no HDR format" ),
			vko.windowHdr ? "" : " not" );
		lastMode = (vkHdrMode_t)-1;
	}

	switch ( r_hdrOutput->integer ) {
	case 3:
		pick = pq;
		reason = "the surface has no HDR10 format";
		break;
	case 4:
		pick = scrgb;
		reason = "the surface has no scRGB format";
		break;
	default:
		if ( !vko.windowHdr ) {
			reason = "the window's display is not in HDR";
		} else {
			pick = pq ? pq : scrgb;
			reason = "the surface has no HDR format (the window system or the driver can't take it)";
		}
		break;
	}

	if ( pick ) {
		mode = pick == pq ? VK_HDR_PQ : VK_HDR_SCRGB;
		*chosen = *pick;
		reason = NULL;
	}
	vko.mode = mode;
	vko.reason = reason;

	if ( mode != lastMode || reason != lastReason ) {
		if ( mode == VK_HDR_SDR ) {
			ri.Printf( PRINT_ALL, "HDR output: SDR, %s\n", reason );
		} else {
			VK_OutputLevels();
			ri.Printf( PRINT_ALL, "HDR output: %s in %s, paper white %.0f nits (%s), peak %.0f nits (%s); the system reports an SDR white level of %.2f and a headroom of %.2f\n",
				VK_HdrModeName( mode ), VK_OutputFormatName( chosen->format ),
				vko.paperWhite, r_hdrPaperWhite->value > 0.0f ? "r_hdrPaperWhite" : "the system's",
				vko.peakNits, r_hdrPeak->value > 0.0f ? "r_hdrPeak" : "the system's", vk_sdlLevel, vk_sdlHeadroom );
		}
	}
	return mode != VK_HDR_SDR;
}

/*
===============
VK_OutputSetMetadata

The display's luminance for the compositor (VK_EXT_hdr_metadata): content
in BT.709 primaries, mastered to the peak, its average about paper white
===============
*/
static void VK_OutputSetMetadata( void ) {
	VkHdrMetadataEXT metadata;

	if ( !vko.metadata || !qvkSetHdrMetadataEXT || !vk.swapchain || vko.mode == VK_HDR_SDR ) {
		return;
	}
	Com_Memset( &metadata, 0, sizeof( metadata ) );
	metadata.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;
	metadata.displayPrimaryRed.x = 0.64f;
	metadata.displayPrimaryRed.y = 0.33f;
	metadata.displayPrimaryGreen.x = 0.30f;
	metadata.displayPrimaryGreen.y = 0.60f;
	metadata.displayPrimaryBlue.x = 0.15f;
	metadata.displayPrimaryBlue.y = 0.06f;
	metadata.whitePoint.x = 0.3127f;
	metadata.whitePoint.y = 0.3290f;
	metadata.maxLuminance = vko.peakNits;
	metadata.minLuminance = 0.0f;
	metadata.maxContentLightLevel = vko.peakNits;
	metadata.maxFrameAverageLightLevel = vko.paperWhite;
	qvkSetHdrMetadataEXT( vk.device, 1, &vk.swapchain, &metadata );
	vko.metadataWhite = vko.paperWhite;
	vko.metadataPeak = vko.peakNits;
}

// this frame's paper white and peak, from the cvars and SDL
static void VK_OutputLevels( void ) {
	float white, peak;

	VK_WindowHdr( &white, &peak );
	vko.paperWhite = r_hdrPaperWhite->value > 0.0f ? r_hdrPaperWhite->value : white;
	vko.peakNits = r_hdrPeak->value > 0.0f ? r_hdrPeak->value : peak;
	if ( vko.peakNits < vko.paperWhite ) {
		vko.peakNits = vko.paperWhite;
	}
}

/*
===============
VK_OutputSwapchainCreated

After VK_CreateSwapchain (the submit thread drained): the metadata of the
new swapchain
===============
*/
void VK_OutputSwapchainCreated( void ) {
	VK_OutputLevels();
	vko.metadataWhite = vko.metadataPeak = 0.0f;
	VK_OutputSetMetadata();
}

/*
===============
VK_OutputUpdate

At the start of every frame's target choice (VK_SelectTarget): this frame's
levels; a change of the window's HDR state makes the swapchain again (out of
date), a change of the levels gives the compositor new metadata
===============
*/
void VK_OutputUpdate( void ) {
	float white, peak;

	if ( r_hdrOutput->integer == 2 && !vk_temporal_enabled() && !vko.noSceneNote ) {
		vko.noSceneNote = qtrue;
		ri.Printf( PRINT_ALL, "HDR look (r_hdrOutput 2): off, it needs r_temporal 1\n" );
	}
	if ( !vko.enabled ) {
		return;
	}
	if ( r_hdrOutput->integer == 1 && VK_WindowHdr( &white, &peak ) != vko.windowHdr ) {
		vk.swapchainOutOfDate = qtrue;
	}
	VK_OutputLevels();
	if ( vko.mode != VK_HDR_SDR && vko.metadata
		&& ( fabsf( vko.paperWhite - vko.metadataWhite ) > 0.5f || fabsf( vko.peakNits - vko.metadataPeak ) > 0.5f ) ) {
		// the submit thread presents to the swapchain
		VK_DrainSubmitThread();
		VK_OutputSetMetadata();
	}
	// the calibration screens' instructions, when one is chosen
	if ( r_hdrOutputTest->modified ) {
		r_hdrOutputTest->modified = qfalse;
		if ( vko.mode == VK_HDR_SDR && r_hdrOutputTest->integer ) {
			ri.Printf( PRINT_ALL, "HDR calibration: the window is SDR (%s), the screens mean nothing there\n", vko.reason ? vko.reason : "" );
		}
		switch ( r_hdrOutputTest->integer ) {
		case 1:
			ri.Printf( PRINT_ALL, "HDR calibration, black: change r_hdrBlack (now %g, -0.5 to 1) until the box in the middle is barely visible on the black: lower shows it, higher hides it. Then r_hdrOutputTest 2 for the peak, 0 to finish\n",
				r_hdrBlack->value );
			break;
		case 2:
			ri.Printf( PRINT_ALL, "HDR calibration, peak: raise r_hdrPeak (now %g nits) until the box in the middle can't be told from the background, and keep the lowest value where it is gone. Then r_hdrOutputTest 0\n",
				r_hdrPeak->value );
			break;
		case 3:
			ri.Printf( PRINT_ALL, "HDR test: patches of 100, 203, 400, 600, 1000, 1500, 2000 and 4000 nits, a ramp to twice r_hdrPeak marked at r_hdrPeak\n" );
			break;
		default:
			break;
		}
	}

	if ( !vk_temporal_enabled() && !vko.noSceneNote && vko.mode != VK_HDR_SDR ) {
		vko.noSceneNote = qtrue;
		ri.Printf( PRINT_ALL, "HDR output: the 3D view is shown at paper white; r_temporal 1 expands it (r_hdrExpand)\n" );
	}
}

// the display's peak over paper white in linear light, 1 for SDR
static float VK_OutputPeak( void ) {
	if ( vko.mode == VK_HDR_SDR || vko.paperWhite <= 0.0f ) {
		return 1.0f;
	}
	return vko.peakNits / vko.paperWhite;
}

void VK_OutputFillPush( vkOutputPush_t *push ) {
	Com_Memset( push, 0, sizeof( *push ) );
	push->mode = vko.mode;
	push->test = r_hdrOutputTest->integer;
	push->paperWhite = vko.paperWhite > 0.0f ? vko.paperWhite : VK_HDR_DEFAULT_WHITE;
	push->peak = VK_OutputPeak();
	push->invExtent[0] = vk.swapchainExtent.width ? 1.0f / vk.swapchainExtent.width : 0.0f;
	push->invExtent[1] = vk.swapchainExtent.height ? 1.0f / vk.swapchainExtent.height : 0.0f;
	push->black = vko.mode != VK_HDR_SDR ? r_hdrBlack->value : 0.0f;
}

/*
===============
VK_OutputExpansion

The composite's expansion of the 3D scene (temporal_composite.frag), in
linear light of paper white, applied to each pixel's largest channel: up to
the knee k unchanged, above it k + (1 - k)(t + b t^2) with t = (x - k) / (1 -
k): continuous in value and slope at k, and the game's white at H = 1 +
r_hdrExpand (P - 1), P = peak / paper white, so b = (H - 1) / (1 - k); the
sky with r_hdrExpand * r_hdrSkyBoost. What is above 1 (stacked additive
effects) is scaled by r_hdrEffectsBoost first. Clamped at P.
params: knee, b, the sky's b, P, effects boost, r_saturation, the mode (1
HDR, 2 the HDR look in SDR, 0 none), r_hdrBlack for the look (the output
pass applies it in HDR). HDR with an HDR swapchain, but in a frame that is
read back; else the look with r_hdrOutput 2: the expansion toward r_hdrPeak
(1000 for 0) over r_hdrPaperWhite (203 for 0), as with an HDR display (an
SDR display has no system levels), and P, where it stops and the shoulder
puts 1, the expanded white (the larger of the sky's and the rest's): white
stays white, what is above it clips as in SDR.
r_saturation applies with the temporal path always (SDR too)
===============
*/
void VK_OutputExpansion( float *params ) {
	qboolean hdr = vko.enabled && !vko.sdrFrame && VK_OutputPeak() > 1.0f;
	float k = r_hdrExpandKnee->value;
	float p, h, hs;

	Com_Memset( params, 0, 8 * sizeof( float ) );
	params[5] = r_saturation->value;
	if ( hdr ) {
		p = VK_OutputPeak();
	} else if ( r_hdrOutput->integer == 2 ) {
		p = ( r_hdrPeak->value > 0.0f ? r_hdrPeak->value : VK_HDR_DEFAULT_PEAK )
			/ ( r_hdrPaperWhite->value > 0.0f ? r_hdrPaperWhite->value : VK_HDR_DEFAULT_WHITE );
		p = Q_max( p, 1.01f );
	} else {
		return;
	}
	h = 1.0f + Com_Clamp( 0.0f, 1.0f, r_hdrExpand->value ) * ( p - 1.0f );
	hs = 1.0f + Com_Clamp( 0.0f, 1.0f, r_hdrExpand->value * r_hdrSkyBoost->value ) * ( p - 1.0f );
	params[0] = k;
	params[1] = ( h - 1.0f ) / ( 1.0f - k );
	params[2] = ( hs - 1.0f ) / ( 1.0f - k );
	params[3] = p;
	params[4] = r_hdrEffectsBoost->value;
	if ( !hdr ) {
		params[3] = Q_max( Q_max( h, hs ), 1.01f );
	}
	params[6] = hdr ? 1.0f : 2.0f;
	params[7] = hdr ? 0.0f : r_hdrBlack->value;
}

/*
===============
vk_sdr_frame

The frame being recorded is read back (a screenshot, a video frame): its 3D
scene is not expanded, so what is saved is the SDR frame; the HUD is
always at paper white
===============
*/
void vk_sdr_frame( void ) {
	vko.sdrFrame = qtrue;
}

/*
====================================================================

THE PASS

====================================================================
*/

static void VK_OutputCreateShared( void ) {
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo setInfo;
	VkPushConstantRange range;
	VkPipelineLayoutCreateInfo layoutInfo;
	VkDescriptorPoolSize poolSize;
	VkDescriptorPoolCreateInfo poolInfo;
	VkDescriptorSetAllocateInfo allocInfo;
	VkSamplerCreateInfo samplerInfo;

	if ( vko.layout ) {
		return;
	}

	Com_Memset( &binding, 0, sizeof( binding ) );
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	Com_Memset( &setInfo, 0, sizeof( setInfo ) );
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setInfo.bindingCount = 1;
	setInfo.pBindings = &binding;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &setInfo, NULL, &vko.setLayout ) );

	range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	range.offset = 0;
	range.size = sizeof( vkOutputPush_t );
	Com_Memset( &layoutInfo, 0, sizeof( layoutInfo ) );
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &vko.setLayout;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &range;
	VK_CHECK( qvkCreatePipelineLayout( vk.device, &layoutInfo, NULL, &vko.layout ) );

	poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSize.descriptorCount = 1;
	Com_Memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	VK_CHECK( qvkCreateDescriptorPool( vk.device, &poolInfo, NULL, &vko.pool ) );
	Com_Memset( &allocInfo, 0, sizeof( allocInfo ) );
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = vko.pool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &vko.setLayout;
	VK_CHECK( qvkAllocateDescriptorSets( vk.device, &allocInfo, &vko.set ) );

	// linear: the window may have another size than the frame (resized
	// before a vid_restart), as the blit's filter; at the same size it
	// samples texel centers, the texel itself
	Com_Memset( &samplerInfo, 0, sizeof( samplerInfo ) );
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.maxLod = 0.0f;
	VK_CHECK( qvkCreateSampler( vk.device, &samplerInfo, NULL, &vko.sampler ) );

	// a triangle over the target, the temporal path's
	vko.vertexShader = VK_CreateShaderModule( vk_spirv_temporal_composite_vert, vk_spirv_temporal_composite_vert_size );
	vko.fragmentShader = VK_CreateShaderModule( vk_spirv_output_frag, vk_spirv_output_frag_size );
}

// the render pass and the pipeline of the swapchain's format
static void VK_OutputCreatePipeline( VkFormat format ) {
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependency;
	VkRenderPassCreateInfo passInfo;
	VkPipelineShaderStageCreateInfo stages[2];
	VkPipelineVertexInputStateCreateInfo vertexInput;
	VkPipelineInputAssemblyStateCreateInfo inputAssembly;
	VkPipelineViewportStateCreateInfo viewportState;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineColorBlendAttachmentState blendAttachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkDynamicState dynamicStates[2];
	VkPipelineDynamicStateCreateInfo dynamicState;
	VkGraphicsPipelineCreateInfo info;

	if ( vko.pipeline && vko.passFormat == format ) {
		return;
	}
	if ( vko.pipeline ) {
		qvkDestroyPipeline( vk.device, vko.pipeline, NULL );
		qvkDestroyRenderPass( vk.device, vko.renderPass, NULL );
		vko.pipeline = VK_NULL_HANDLE;
		vko.renderPass = VK_NULL_HANDLE;
	}

	// the whole image is drawn: nothing loaded; ready to present after it
	Com_Memset( &attachment, 0, sizeof( attachment ) );
	attachment.format = format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	reference.attachment = 0;
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	Com_Memset( &subpass, 0, sizeof( subpass ) );
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;
	// the acquire semaphore is waited for at the color attachment stage
	// (VK_PresentCopyStage): the layout change after it
	Com_Memset( &dependency, 0, sizeof( dependency ) );
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.srcAccessMask = 0;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	Com_Memset( &passInfo, 0, sizeof( passInfo ) );
	passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	passInfo.attachmentCount = 1;
	passInfo.pAttachments = &attachment;
	passInfo.subpassCount = 1;
	passInfo.pSubpasses = &subpass;
	passInfo.dependencyCount = 1;
	passInfo.pDependencies = &dependency;
	VK_CHECK( qvkCreateRenderPass( vk.device, &passInfo, NULL, &vko.renderPass ) );

	Com_Memset( stages, 0, sizeof( stages ) );
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vko.vertexShader;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = vko.fragmentShader;
	stages[1].pName = "main";

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
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamicState;
	info.layout = vko.layout;
	info.renderPass = vko.renderPass;
	VK_CHECK( qvkCreateGraphicsPipelines( vk.device, vk.pipelineCache, 1, &info, NULL, &vko.pipeline ) );
	vko.passFormat = format;
}

/*
===============
VK_OutputCreateTargets

With VK_CreateSwapchainTargets (the swapchain or the render targets were
made): the pipeline of the swapchain's format, a view and a framebuffer per
image, the set with the frame's color image
===============
*/
void VK_OutputCreateTargets( void ) {
	VkImageViewCreateInfo viewInfo;
	VkFramebufferCreateInfo framebufferInfo;
	VkDescriptorImageInfo imageInfo;
	VkWriteDescriptorSet write;
	uint32_t i;

	VK_OutputDestroyTargets();
	if ( !vko.enabled || !vk.swapchain || !vk.colorView ) {
		return;
	}

	VK_OutputCreateShared();
	VK_OutputCreatePipeline( vk.swapchainFormat );

	if ( vko.setView != vk.colorView ) {
		imageInfo.sampler = vko.sampler;
		imageInfo.imageView = vk.colorView;
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		Com_Memset( &write, 0, sizeof( write ) );
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = vko.set;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &imageInfo;
		qvkUpdateDescriptorSets( vk.device, 1, &write, 0, NULL );
		vko.setView = vk.colorView;
	}

	for ( i = 0; i < vk.swapchainImageCount; i++ ) {
		Com_Memset( &viewInfo, 0, sizeof( viewInfo ) );
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = vk.swapchainImages[i];
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = vk.swapchainFormat;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		VK_CHECK( qvkCreateImageView( vk.device, &viewInfo, NULL, &vko.views[i] ) );

		Com_Memset( &framebufferInfo, 0, sizeof( framebufferInfo ) );
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferInfo.renderPass = vko.renderPass;
		framebufferInfo.attachmentCount = 1;
		framebufferInfo.pAttachments = &vko.views[i];
		framebufferInfo.width = vk.swapchainExtent.width;
		framebufferInfo.height = vk.swapchainExtent.height;
		framebufferInfo.layers = 1;
		VK_CHECK( qvkCreateFramebuffer( vk.device, &framebufferInfo, NULL, &vko.framebuffers[i] ) );
	}
}

// called when nothing uses them anymore
void VK_OutputDestroyTargets( void ) {
	int i;

	for ( i = 0; i < VK_MAX_SWAPCHAIN_IMAGES; i++ ) {
		if ( vko.framebuffers[i] ) {
			qvkDestroyFramebuffer( vk.device, vko.framebuffers[i], NULL );
			vko.framebuffers[i] = VK_NULL_HANDLE;
		}
		if ( vko.views[i] ) {
			qvkDestroyImageView( vk.device, vko.views[i], NULL );
			vko.views[i] = VK_NULL_HANDLE;
		}
	}
}

/*
===============
VK_OutputRecord

The output pass into a swapchain image (VK_RecordPresentCopy, also on the
submit thread): the frame's color image read in the fragment shader, the
swapchain image drawn whole and left ready to present
===============
*/
void VK_OutputRecord( const vkFrame_t *frame, VkCommandBuffer cmd, VkImage image ) {
	VkRenderPassBeginInfo beginInfo;
	VkViewport viewport;
	VkRect2D area;
	uint32_t i;

	for ( i = 0; i < vk.swapchainImageCount && vk.swapchainImages[i] != image; i++ ) {
	}
	if ( i == vk.swapchainImageCount || !vko.framebuffers[i] ) {
		return;
	}

	VK_ImageBarrier( cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );

	area.offset.x = area.offset.y = 0;
	area.extent = vk.swapchainExtent;
	Com_Memset( &beginInfo, 0, sizeof( beginInfo ) );
	beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	beginInfo.renderPass = vko.renderPass;
	beginInfo.framebuffer = vko.framebuffers[i];
	beginInfo.renderArea = area;
	qvkCmdBeginRenderPass( cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE );

	viewport.x = viewport.y = 0.0f;
	viewport.width = (float)vk.swapchainExtent.width;
	viewport.height = (float)vk.swapchainExtent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	qvkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vko.pipeline );
	qvkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vko.layout, 0, 1, &vko.set, 0, NULL );
	qvkCmdPushConstants( cmd, vko.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( frame->outputPush ), &frame->outputPush );
	qvkCmdSetViewport( cmd, 0, 1, &viewport );
	qvkCmdSetScissor( cmd, 0, 1, &area );
	qvkCmdDraw( cmd, 3, 1, 0, 0 );
	qvkCmdEndRenderPass( cmd );

	VK_ImageBarrier( cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );
}

// at vk_shutdown, the device idle
void VK_OutputShutdown( void ) {
	VK_OutputDestroyTargets();
	if ( vk.device ) {
		if ( vko.pipeline ) {
			qvkDestroyPipeline( vk.device, vko.pipeline, NULL );
		}
		if ( vko.renderPass ) {
			qvkDestroyRenderPass( vk.device, vko.renderPass, NULL );
		}
		if ( vko.fragmentShader ) {
			qvkDestroyShaderModule( vk.device, vko.fragmentShader, NULL );
		}
		if ( vko.vertexShader ) {
			qvkDestroyShaderModule( vk.device, vko.vertexShader, NULL );
		}
		if ( vko.sampler ) {
			qvkDestroySampler( vk.device, vko.sampler, NULL );
		}
		if ( vko.pool ) {
			qvkDestroyDescriptorPool( vk.device, vko.pool, NULL );
		}
		if ( vko.layout ) {
			qvkDestroyPipelineLayout( vk.device, vko.layout, NULL );
		}
		if ( vko.setLayout ) {
			qvkDestroyDescriptorSetLayout( vk.device, vko.setLayout, NULL );
		}
	}
	Com_Memset( &vko, 0, sizeof( vko ) );
}

/*
===============
VK_HalfToFloat
===============
*/
float VK_HalfToFloat( uint16_t h ) {
	int exponent = ( h >> 10 ) & 31;
	int mantissa = h & 1023;
	float f;

	if ( exponent == 0 ) {
		f = ldexpf( (float)mantissa, -24 );
	} else if ( exponent == 31 ) {
		f = mantissa ? 0.0f : 65504.0f;		// NaN as 0, infinity as the largest
	} else {
		f = ldexpf( (float)( mantissa | 1024 ), exponent - 25 );
	}
	return ( h & 0x8000 ) ? -f : f;
}
