/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_draw.c -- drawing

#include "tr_local.h"

vec4_t r_colorWhite = { 1.0, 1.0, 1.0, 1.0 };

/*
================
Draw_SetColor
================
*/
void Draw_SetColor(const vec4_t rgba) {
#if 1
	if (!rgba) {
		rgba = r_colorWhite;
	}

	backEnd.color2D[0] = (byte)(rgba[0] * tr.identityLightByte);
	backEnd.color2D[1] = (byte)(rgba[1] * tr.identityLightByte);
	backEnd.color2D[2] = (byte)(rgba[2] * tr.identityLightByte);
	backEnd.color2D[3] = (byte)(rgba[3] * 255.0);
	RB_SetColor4ubv(backEnd.color2D);
#else
	RE_SetColor(rgba);
#endif
}

/*
================
Draw_StretchPic
================
*/
void Draw_StretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader) {
#if 1
	shader_t* shader;

	R_IssuePendingRenderCommands();

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	if (w <= 0) {
		w = shader->unfoggedStages[0]->bundle[0].image[0]->width;
		h = shader->unfoggedStages[0]->bundle[0].image[0]->height;
	}

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);
	RB_BeginSurface(shader);

	RB_Texcoord2f(s1, t1);
	RB_Vertex2f(x, y);

	RB_Texcoord2f(s2, t1);
	RB_Vertex2f(x + w, y);

	RB_Texcoord2f(s1, t2);
	RB_Vertex2f(x, y + h);

	RB_Texcoord2f(s2, t2);
	RB_Vertex2f(x + w, y + h);

	RB_StreamEnd();
#else
	RE_StretchPic(x, y, w, h, s1, t1, s2, t2, hShader);
#endif
}

/*
================
Draw_StretchPic2
================
*/
void Draw_StretchPic2(float x, float y, float w, float h, float s1, float t1, float s2, float t2, float sx, float sy, qhandle_t hShader) {
	shader_t* shader;
	float halfWidth, halfHeight;
	float scaledWidth1, scaledHeight1;
	float scaledWidth2, scaledHeight2;

	R_IssuePendingRenderCommands();

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	if (w <= 0) {
		w = shader->unfoggedStages[0]->bundle[0].image[0]->width;
		h = shader->unfoggedStages[0]->bundle[0].image[0]->height;
	}

	halfWidth = w * 0.5f;
	halfHeight = h * 0.5f;
	scaledWidth1 = halfWidth * sy;
	scaledHeight1 = halfHeight * sx;
	scaledWidth2 = halfWidth * sx;
	scaledHeight2 = halfHeight * sy;

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);
	RB_BeginSurface(shader);

	RB_Texcoord2f(s1, t1);
	RB_Vertex3f(x + halfWidth - (scaledWidth2 + -scaledHeight2), y + halfWidth - scaledHeight1 - scaledWidth1, 0);

	RB_Texcoord2f(t2, t1);
	RB_Vertex3f(scaledWidth2 - -scaledHeight2 + x + halfWidth, scaledWidth1 - scaledHeight1 + y + halfWidth, 0);

	RB_Texcoord2f(s1, s2);
	RB_Vertex3f(x+ halfWidth - (scaledWidth2 + scaledHeight2), scaledHeight1 - scaledWidth1 + y + halfWidth, 0);

	RB_Texcoord2f(t2, s2);
	RB_Vertex3f(scaledWidth2 - scaledHeight2 + x + halfWidth, scaledWidth1 + scaledHeight1 + y + halfWidth, 0);

	RB_StreamEnd();
}


/*
================
Draw_TilePic
================
*/
void Draw_TilePic(float x, float y, float w, float h, qhandle_t hShader) {
	shader_t* shader;
	float		picw, pich;

	R_IssuePendingRenderCommands();

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	if (w <= 0) {
		w = shader->unfoggedStages[0]->bundle[0].image[0]->width;
		h = shader->unfoggedStages[0]->bundle[0].image[0]->height;
	}

	picw = shader->unfoggedStages[0]->bundle[0].image[0]->uploadWidth;
	pich = shader->unfoggedStages[0]->bundle[0].image[0]->uploadHeight;

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);

	RB_StreamBegin(shader);

	RB_Texcoord2f(x / picw, y / pich);
	RB_Vertex2f(x, y);

	RB_Texcoord2f((x + w) / picw, y / pich);
	RB_Vertex2f(x + w, y);

	RB_Texcoord2f(x / picw, (y + h) / pich);
	RB_Vertex2f(x, y + h);

	RB_Texcoord2f((x + w) / picw, (y + h) / pich);
	RB_Vertex2f(x + w, y + h);

	RB_StreamEnd();
}

/*
================
Draw_TilePicOffset
================
*/
void Draw_TilePicOffset(float x, float y, float w, float h, qhandle_t hShader, int offsetX, int offsetY) {
	shader_t* shader;
	float		picw, pich;

	R_IssuePendingRenderCommands();

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	if (w <= 0) {
		w = shader->unfoggedStages[0]->bundle[0].image[0]->width;
		h = shader->unfoggedStages[0]->bundle[0].image[0]->height;
	}

	picw = shader->unfoggedStages[0]->bundle[0].image[0]->uploadWidth;
	pich = shader->unfoggedStages[0]->bundle[0].image[0]->uploadHeight;

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);

	RB_StreamBegin(shader);

	RB_Texcoord2f(x / picw, y / pich);
	RB_Vertex2f(x + offsetX, y + offsetY);

	RB_Texcoord2f((x + w) / picw, y / pich);
	RB_Vertex2f(x + offsetX + w, y + offsetY);

	RB_Texcoord2f(x / picw, (y + h) / pich);
	RB_Vertex2f(x + offsetX, y + offsetY + h);

	RB_Texcoord2f((x + w) / picw, (y + h) / pich);
	RB_Vertex2f(x + offsetX + w, y + offsetY + h);

	RB_StreamEnd();
}

/*
================
Draw_TrianglePic
================
*/
void Draw_TrianglePic(const vec2_t vPoints[3], const vec2_t vTexCoords[3], qhandle_t hShader) {
	int			i;
	shader_t* shader;

	R_IssuePendingRenderCommands();

	if (hShader) {
		shader = R_GetShaderByHandle(hShader);
	}
	else {
		shader = tr.defaultShader;
	}

	// draw the pic
	RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);

	RB_BeginSurface(shader);

	for (i = 0; i < 3; i++) {
		RB_Texcoord2f(vTexCoords[i][0], vTexCoords[i][1]);
		RB_Vertex2f(vPoints[i][0], vPoints[i][1]);
	}

	RB_StreamEnd();
}

/*
================
RE_DrawBackground_TexSubImage
================
*/
void RE_DrawBackground_TexSubImage(int cols, int rows, int bgr, byte* data) {
	int		w, h;
	int		i;
	byte	*rgba;

	w = glConfig.vidWidth;
	h = glConfig.vidHeight;

	R_IssuePendingRenderCommands();
	vk_finish();

	// expand the 24-bit RGB or BGR data to RGBA
	rgba = ri.Hunk_AllocateTempMemory(cols * rows * 4);
	for (i = 0; i < cols * rows; i++) {
		if (bgr) {
			rgba[i * 4 + 0] = data[i * 3 + 2];
			rgba[i * 4 + 2] = data[i * 3 + 0];
		} else {
			rgba[i * 4 + 0] = data[i * 3 + 0];
			rgba[i * 4 + 2] = data[i * 3 + 2];
		}
		rgba[i * 4 + 1] = data[i * 3 + 1];
		rgba[i * 4 + 3] = 255;
	}

	GL_Bind(tr.scratchImage);
	R_UploadScratchImage(tr.scratchImage, cols, rows, rgba, qtrue);
	ri.Hunk_FreeTempMemory(rgba);

	// bypasses the tracked state, like the original
	vk_set_cull(CULL_NONE);
	vk_set_state(glState.glStateBits | GLS_DEPTHTEST_DISABLE);
	GL_EnableTexture(qtrue);

	RB_ImmBegin(IMM_QUADS);

	RB_ImmTexCoord2f(0.5 / (float)cols, ((float)rows - 0.5) / rows);
	RB_ImmVertex2f(0, 0);

	RB_ImmTexCoord2f(((float)cols - 0.5) / cols, ((float)rows - 0.5) / rows);
	RB_ImmVertex2f(w, 0);

	RB_ImmTexCoord2f(((float)cols - 0.5) / cols, 0.5 / (float)rows);
	RB_ImmVertex2f(w, h);

	RB_ImmTexCoord2f(0.5 / (float)rows, 0.5 / (float)rows);
	RB_ImmVertex2f(0, h);

	RB_ImmEnd();
}

/*
================
RE_DrawBackground_DrawPixels

There is no pixel drawing path, the image is drawn as a textured quad
================
*/
void RE_DrawBackground_DrawPixels(int cols, int rows, int bgr, byte* data) {
	RE_DrawBackground_TexSubImage(cols, rows, bgr, data);
}

/*
================
AddBox
================
*/
void AddBox(float x, float y, float w, float h) {
	R_IssuePendingRenderCommands();

	RB_SetColor4ubv(backEnd.color2D);
	GL_EnableTexture(qfalse);
	GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

	RB_ImmBegin(IMM_QUADS);

	RB_ImmVertex2f(x, y);
	RB_ImmVertex2f(x + w, y);
	RB_ImmVertex2f(x + w, y + h);
	RB_ImmVertex2f(x, y + h);

	RB_ImmEnd();

	GL_EnableTexture(qtrue);
}

/*
================
DrawBox
================
*/
void DrawBox(float x, float y, float w, float h) {
	R_IssuePendingRenderCommands();

	RB_SetColor4ubv(backEnd.color2D);
	GL_EnableTexture(qfalse);
	GL_State(GLS_DEPTHTEST_DISABLE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_SRCBLEND_SRC_ALPHA);

	RB_ImmBegin(IMM_QUADS);

	RB_ImmVertex2f(x, y);
	RB_ImmVertex2f(x + w, y);
	RB_ImmVertex2f(x + w, y + h);
	RB_ImmVertex2f(x, y + h);

	RB_ImmEnd();

	GL_EnableTexture(qtrue);
}

/*
================
DrawLineLoop
================
*/
void DrawLineLoop(const vec2_t* points, int count, int stipple_factor, int stipple_mask) {
	int		i;

	R_IssuePendingRenderCommands();

	GL_EnableTexture(qfalse);

	if (stipple_factor) {
		vk_set_line_stipple(stipple_factor, stipple_mask);
	}

	RB_ImmBegin(IMM_LINE_LOOP);

	for (i = 0; i < count; i++) {
		RB_ImmVertex2f(points[i][0], points[i][1]);
	}

	RB_ImmEnd();

	GL_EnableTexture(qtrue);

	if (stipple_factor) {
		vk_set_line_stipple(0, 0xFFFF);
	}
}

/*
================
Set2DWindow
================
*/
void Set2DWindow(int x, int y, int w, int h, float left, float right, float bottom, float top, float n, float f) {
	float	matrix[16];

	R_IssuePendingRenderCommands();
	RB_SetViewport(x, y, w, h);
	RB_SetScissor(x, y, w, h);
	RB_OrthoMatrix(matrix, left, right, bottom, top, n, f);
	RB_SetProjection(matrix);

	RB_LoadIdentity(matrix);
	RB_SetModelview(matrix);
	GL_State(GLS_DEPTHTEST_DISABLE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_SRCBLEND_SRC_ALPHA);
	vk_set_cull(CULL_NONE);
	vk_set_clip_plane(NULL);
	// Make sure to disable the fog to avoid messing up with the UI
	vk_set_fog(qfalse);
	// GL_State can enable it again for 2D stages; with a start of 0 the
	// fog has no effect at the 2D depth
	RB_SetFogRange(0, glState.fogEnd);

	if (r_reset_tc_array->integer) {
		RB_EnableTexCoordArray(qfalse);
	}

	if (!backEnd.in2D)
	{
		backEnd.refdef.time = ri.Milliseconds();
		backEnd.in2D = qtrue;
		backEnd.refdef.floatTime = backEnd.refdef.time / 1000.0;
        backEnd.shaderStartTime = 0; 
	}
}

/*
================
RE_Scissor
================
*/
void RE_Scissor(int x, int y, int width, int height) {
	RB_SetScissor(x, y, width, height);
}

/*
================
Set2DInitialShaderTime
================
*/
void Set2DInitialShaderTime(float startTime)
{
	if (backEnd.in2D)
	{
		backEnd.shaderStartTime = startTime;
	}
}
