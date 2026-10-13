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

// vk.c -- GPU backend
//
// Placeholder implementation of the interface in vk.h: it records nothing
// and draws nothing, which lets the renderer run without any graphics API.
// The Vulkan implementation replaces the bodies of these functions.

#include "tr_local.h"

static unsigned int vk_nextTexture;

/*
====================================================================

LIFECYCLE

====================================================================
*/

void vk_initialize( void ) {
	Q_strncpyz( glConfig.vendor_string, "OpenMoHAA", sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, "null backend (Vulkan not implemented yet)", sizeof( glConfig.renderer_string ) );
	Q_strncpyz( glConfig.version_string, "0", sizeof( glConfig.version_string ) );
	glConfig.extensions_string[0] = 0;

	// what the device supports; the renderer applies the r_ext_* cvars after
	glConfig.maxTextureSize = 2048;
	glConfig.numTextureUnits = 2;
	glConfig.textureCompression = TC_S3TC_ARB;
	glConfig.textureEnvAddAvailable = qtrue;

	textureFilterAnisotropic = qtrue;
	maxAnisotropy = 16;
	haveClampToEdge = qtrue;

	vk_nextTexture = 0;
}

void vk_shutdown( void ) {
}

void vk_begin_frame( void ) {
}

void vk_end_frame( void ) {
}

void vk_finish( void ) {
}

/*
====================================================================

TEXTURES

====================================================================
*/

void vk_create_texture( image_t *image, int width, int height, int mipLevels, texFormat_t format ) {
	if ( !image->texnum ) {
		image->texnum = ++vk_nextTexture;
	}
}

void vk_upload_texture( image_t *image, int mipLevel, int x, int y, int width, int height, const byte *rgba ) {
}

void vk_upload_compressed_texture( image_t *image, int mipLevel, int width, int height, const byte *data, int size ) {
}

void vk_destroy_texture( image_t *image ) {
	image->texnum = 0;
}

void vk_set_texture_filter( texFilter_t filter ) {
}

/*
====================================================================

RENDER STATE

====================================================================
*/

void vk_set_state( unsigned long stateBits ) {
}

void vk_bind_texture( int tmu, image_t *image ) {
}

void vk_set_texenv( int tmu, texEnv_t env ) {
}

void vk_set_cull( cullFace_t face ) {
}

void vk_set_depth_range( float zNear, float zFar ) {
}

void vk_set_polygon_offset( qboolean enable, float factor, float units ) {
}

void vk_set_color_mask( qboolean red, qboolean green, qboolean blue, qboolean alpha ) {
}

void vk_set_fog( qboolean enable ) {
}

void vk_set_fog_color( const vec4_t color ) {
}

void vk_set_fog_range( float start, float end ) {
}

void vk_set_clip_plane( const float *eyePlane ) {
}

void vk_set_stencil_test( qboolean enable ) {
}

void vk_set_stencil_func( stencilFunc_t func, int ref, unsigned int mask ) {
}

void vk_set_stencil_op( stencilOp_t stencilFail, stencilOp_t depthFail, stencilOp_t depthPass ) {
}

void vk_set_stencil_write_mask( unsigned int mask ) {
}

void vk_set_line_width( float width ) {
}

void vk_set_line_stipple( int factor, unsigned short pattern ) {
}

void vk_set_projection( const float *matrix ) {
}

void vk_set_modelview( const float *matrix ) {
}

void vk_set_viewport( int x, int y, int width, int height ) {
}

void vk_set_scissor( int x, int y, int width, int height ) {
}

/*
====================================================================

DRAWING

====================================================================
*/

void vk_draw( primType_t prim, const drawGeometry_t *geometry, int numIndexes, const glIndex_t *indexes ) {
}

void vk_clear( int buffers, const vec4_t color ) {
}

void vk_read_pixels( int x, int y, int width, int height, readFormat_t format, void *buffer ) {
	size_t pixelSize;

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

	Com_Memset( buffer, 0, pixelSize * width * height );
}
