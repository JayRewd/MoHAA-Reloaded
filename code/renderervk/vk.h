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

// vk.h -- interface between the renderer and its GPU backend
//
// The renderer (tr_*.c) contains no graphics API calls: every GPU operation
// goes through the functions declared here, and the vk*.c files implement
// them with Vulkan (see vk_local.h).
//
// The interface mirrors the fixed-function state the original GL1 renderer was
// written against, so rendering stays identical to it. State setters only
// record state; vk_draw() and vk_clear() consume the state current at the time
// of the call. A backend is expected to turn the recorded state into cached
// pipelines (see README.md in this directory for the full contract).
//
// Conventions (inherited from the GL1 renderer):
//  - matrices are 4x4, column-major; projection matrices produce OpenGL clip
//    space (y up, z in [-w, w])
//  - window coordinates (viewport, scissor, read pixels) are in pixels with
//    the origin at the bottom-left corner of the window
//  - texture data is uploaded top row first; t = 0 is the first row
//  - front faces are counter-clockwise in window space

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
====================================================================

PLATFORM WINDOW (sdl/sdl_vkimp.c)

====================================================================
*/

void		VKimp_Init( void );			// create the window and fill glConfig video fields
void		VKimp_Shutdown( void );		// destroy the window
void		VKimp_EndFrame( void );		// handle window changes (fullscreen toggle) after presenting

/*
====================================================================

LIFECYCLE

====================================================================
*/

// called once the window exists; must fill glConfig with what the device
// supports: maxTextureSize, numTextureUnits (2), textureEnvAddAvailable,
// textureCompression (TC_S3TC_ARB when BC formats are supported),
// textureFilterAnisotropic/maxAnisotropy, the vendor/renderer/version/
// extensions strings, and depthBits/stencilBits if they differ from what
// VKimp_Init requested. The renderer then turns features off according to
// r_allowExtensions and the r_ext_* cvars, like the GL1 renderer did.
// The backend should also honour r_swapInterval (present mode) and
// r_ext_multisample.
void		vk_initialize( void );
// release every GPU resource; called before the window is destroyed
void		vk_shutdown( void );

// a frame is everything recorded between two vk_end_frame() calls;
// vk_begin_frame() is called at the start of each frame, but drawing,
// clearing and reading may also happen before it and must then start
// the frame implicitly
void		vk_begin_frame( void );
// submit the frame and present it
void		vk_end_frame( void );
// block until the GPU has finished all submitted work
void		vk_finish( void );

/*
====================================================================

TEXTURES

====================================================================
*/

typedef enum {
	TEXFMT_RGBA8,
	TEXFMT_RGB8,		// alpha is ignored and samples as 1
	TEXFMT_RGBA4,		// 16-bit storage requested by r_texturebits; RGBA8 is acceptable
	TEXFMT_RGB5,		// 16-bit storage requested by r_texturebits; RGB8 is acceptable
	TEXFMT_BC1_RGB,		// DXT1, pre-compressed data
	TEXFMT_BC1_RGBA,	// DXT1 with 1-bit alpha, pre-compressed data
	TEXFMT_BC2,			// DXT3, pre-compressed data
	TEXFMT_BC3			// DXT5, pre-compressed data
} texFormat_t;

// texture wrap mode, stored in image_t::wrapClampModeX/Y
#define WRAP_REPEAT		1
#define WRAP_CLAMP		2	// clamp to edge

// global filter for mipmapped textures (r_textureMode); textures without
// mipmaps always use linear filtering
typedef enum {
	TEXFILTER_NEAREST,
	TEXFILTER_LINEAR,
	TEXFILTER_NEAREST_MIPMAP_NEAREST,
	TEXFILTER_LINEAR_MIPMAP_NEAREST,
	TEXFILTER_NEAREST_MIPMAP_LINEAR,
	TEXFILTER_LINEAR_MIPMAP_LINEAR
} texFilter_t;

// (re)create the storage of an image: all mip levels, undefined contents;
// sets image->texnum to a non-zero backend handle, never reused for another
// image (the renderer caches bound handles). Calling it again on the
// same image replaces the previous storage (draws already recorded keep
// using the old one).
// Sampling uses image->wrapClampModeX/Y, the global texture filter and,
// for mipmapped textures, anisotropy from r_ext_texture_filter_anisotropic
// and r_ext_max_anisotropy.
void		vk_create_texture( image_t *image, int width, int height, int mipLevels, texFormat_t format );
// upload RGBA8 pixels (rows tightly packed, top row first) to a region of
// one mip level. Uploads may happen in the middle of a frame: draws recorded
// before the upload see the old contents, draws after it the new ones.
void		vk_upload_texture( image_t *image, int mipLevel, int x, int y, int width, int height, const byte *rgba );
// upload one complete mip level of a BC-compressed texture
void		vk_upload_compressed_texture( image_t *image, int mipLevel, int width, int height, const byte *data, int size );
// release the storage of an image and reset image->texnum to 0
void		vk_destroy_texture( image_t *image );
// minification filter of mipmapped textures; magnification is nearest for the
// TEXFILTER_NEAREST* modes and linear otherwise
void		vk_set_texture_filter( texFilter_t filter );

/*
====================================================================

RENDER STATE

All setters only record state; it applies to subsequent draws and clears.
GL_SetDefaultState() sets every piece of state once the backend is
initialized, so the backend's own initial values do not matter.

====================================================================
*/

// shaderStage_t::multitextureEnv uses 0 for "no second texture", so the
// values start at 1
typedef enum {
	TEXENV_MODULATE = 1,	// result = previous * texture
	TEXENV_REPLACE,		// result = texture
	TEXENV_DECAL,		// result.rgb = mix(previous.rgb, texture.rgb, texture.a), result.a = previous.a
	TEXENV_ADD,			// result.rgb = previous.rgb + texture.rgb, result.a = previous.a * texture.a
	// GLS_MULTITEXTURE: rgb = vertex.rgb * vertex.a + texture.rgb * (1 - vertex.a),
	// a = previous.a * texture.a (no shader currently produces it)
	TEXENV_COMBINE_INTERPOLATE,
	// GLS_MULTITEXTURE_ENV (NV_texture_env_combine4, removed in MOHAA 2.0;
	// no shader currently produces it)
	TEXENV_COMBINE4
} texEnv_t;

typedef enum {
	CULL_NONE,
	CULL_FRONT,			// discard front faces
	CULL_BACK			// discard back faces
} cullFace_t;

typedef enum {
	STENCILFUNC_ALWAYS,
	STENCILFUNC_NOTEQUAL
} stencilFunc_t;

typedef enum {
	STENCILOP_KEEP,
	STENCILOP_INCR,		// increment and clamp
	STENCILOP_DECR		// decrement and clamp
} stencilOp_t;

// GLS_* bits (tr_local.h) for blending, depth test/func/write, polygon line
// mode and alpha test. The backend must honour GLS_SRCBLEND_BITS,
// GLS_DSTBLEND_BITS, GLS_DEPTHMASK_TRUE, GLS_DEPTHTEST_DISABLE,
// GLS_DEPTHFUNC_EQUAL (otherwise LEQUAL), GLS_POLYMODE_LINE and GLS_ATEST_BITS
// (the FOLIAGE thresholds come from r_alpha_foliage1/2), and should clamp
// both texture units to the edge while GLS_CLAMP_EDGE is set.
// Fog, color mask and multitexture bits are applied through the dedicated
// setters below and can be ignored here.
void		vk_set_state( unsigned long stateBits );

// texture units: unit 0 is combined with the vertex color, unit 1 with the
// result of unit 0. A unit only contributes when the geometry passed to
// vk_draw() has texture coordinates for it.
void		vk_bind_texture( int tmu, image_t *image );
void		vk_set_texenv( int tmu, texEnv_t env );

void		vk_set_cull( cullFace_t face );
void		vk_set_depth_range( float zNear, float zFar );
// depth bias for filled polygons only (glPolygonOffset with GL_POLYGON_OFFSET_FILL)
void		vk_set_polygon_offset( qboolean enable, float factor, float units );
void		vk_set_color_mask( qboolean red, qboolean green, qboolean blue, qboolean alpha );

// linear fog on eye-space depth, applied to the final fragment color
void		vk_set_fog( qboolean enable );
void		vk_set_fog_color( const vec4_t color );
void		vk_set_fog_range( float start, float end );

// user clip plane in eye space: keep fragments where dot(plane, eyePos) >= 0;
// NULL disables clipping
void		vk_set_clip_plane( const float *eyePlane );

void		vk_set_stencil_test( qboolean enable );
void		vk_set_stencil_func( stencilFunc_t func, int ref, unsigned int mask );
void		vk_set_stencil_op( stencilOp_t stencilFail, stencilOp_t depthFail, stencilOp_t depthPass );
void		vk_set_stencil_write_mask( unsigned int mask );

void		vk_set_line_width( float width );
// factor 0 disables stippling
void		vk_set_line_stipple( int factor, unsigned short pattern );

void		vk_set_projection( const float *matrix );
void		vk_set_modelview( const float *matrix );
void		vk_set_viewport( int x, int y, int width, int height );
// the scissor test is always enabled
void		vk_set_scissor( int x, int y, int width, int height );

/*
====================================================================

DRAWING

====================================================================
*/

typedef enum {
	PRIM_TRIANGLES,
	PRIM_LINES
} primType_t;

typedef struct {
	int				numVertexes;
	const float		*xyz;				// 3 floats per vertex
	int				xyzStride;			// in bytes
	const byte		*colors;			// RGBA8 per vertex, 4 bytes apart; NULL to use color
	float			color[4];			// constant color (0 to 1) when colors is NULL
	const float		*st[NUM_TEXTURE_BUNDLES];	// 2 floats per vertex; NULL disables the unit
	int				stStride[NUM_TEXTURE_BUNDLES];	// in bytes
} drawGeometry_t;

// draw indexed primitives with the current state; the arrays are only read
// during the call
void		vk_draw( primType_t prim, const drawGeometry_t *geometry, int numIndexes, const glIndex_t *indexes );

#define CLEAR_COLOR		1
#define CLEAR_DEPTH		2	// to 1.0
#define CLEAR_STENCIL	4	// to 0

// clear the current scissor rectangle; like glClear it honours the color
// mask, the depth write bit of the current state and the stencil write mask.
// color may be NULL when CLEAR_COLOR is not requested
void		vk_clear( int buffers, const vec4_t color );

typedef enum {
	READ_RGB8,			// 3 bytes per pixel
	READ_DEPTH_FLOAT,	// window-space depth in [0, 1], one float per pixel
	READ_STENCIL8		// 1 byte per pixel
} readFormat_t;

// read back what has been drawn so far in the current frame; rows are
// tightly packed and stored bottom row first
void		vk_read_pixels( int x, int y, int width, int height, readFormat_t format, void *buffer );

#ifdef __cplusplus
}
#endif
