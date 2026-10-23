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
	mbColor4ubv(backEnd.color2D);
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
	int	format;
	int		w, h;

	w = glConfig.vidWidth;
	h = glConfig.vidHeight;

	R_IssuePendingRenderCommands();
	mbFinish();

	if (bgr) {
		format = MB_BGR_EXT;
	}
	else {
		format = MB_RGB;
	}

	R_Bind(tr.scratchImage);

	if (cols == tr.scratchImage->width && rows == tr.scratchImage->height && format == tr.scratchImage->internalFormat)
	{
		mbTexSubImage2D(MB_TEXTURE_2D, 0, 0, 0, cols, rows, format, MB_UNSIGNED_BYTE, data);
	}
	else
	{
		tr.scratchImage->uploadWidth = cols;
		tr.scratchImage->uploadHeight = rows;
		tr.scratchImage->internalFormat = format;
		mbTexImage2D(MB_TEXTURE_2D, 0, MB_RGB, cols, rows, 0, format, MB_UNSIGNED_BYTE, data);
		mbTexParameterf(MB_TEXTURE_2D, MB_TEXTURE_MIN_FILTER, MB_LINEAR);
		mbTexParameterf(MB_TEXTURE_2D, MB_TEXTURE_MAG_FILTER, MB_LINEAR);
	}

	mbDisable(MB_CULL_FACE);
	mbDisable(MB_DEPTH_TEST);
	mbEnable(MB_TEXTURE_2D);

	mbBegin(MB_QUADS);

	mbTexCoord2f(0.5 / (float)cols, ((float)rows - 0.5) / rows);
	mbVertex2f(0, 0);

	mbTexCoord2f(((float)cols - 0.5) / cols, ((float)rows - 0.5) / rows);
	mbVertex2f(w, 0);

	mbTexCoord2f(((float)cols - 0.5) / cols, 0.5 / (float)rows);
	mbVertex2f(w, h);

	mbTexCoord2f(0.5 / (float)rows, 0.5 / (float)rows);
	mbVertex2f(0, h);

	mbEnd();
}

/*
================
RE_DrawBackground_DrawPixels
================
*/
void RE_DrawBackground_DrawPixels(int cols, int rows, int bgr, byte* data) {
	R_IssuePendingRenderCommands();

	R_State(0);
	mbDisable(MB_TEXTURE_2D);

	mbPixelZoom(glConfig.vidWidth / rows, glConfig.vidHeight / cols);

	if (bgr) {
		mbDrawPixels(cols, rows, MB_BGR, MB_UNSIGNED_BYTE, data);
	} else {
		mbDrawPixels(cols, rows, MB_RGB, MB_UNSIGNED_BYTE, data);
	}

	mbPixelZoom(1.0, 1.0);

	mbEnable(MB_TEXTURE_2D);
}

/*
================
AddBox
================
*/
void AddBox(float x, float y, float w, float h) {
	R_IssuePendingRenderCommands();

	mbColor4ubv(backEnd.color2D);
	mbDisable(MB_TEXTURE_2D);
	R_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

	mbBegin(MB_QUADS);

	mbVertex2f(x, y);
	mbVertex2f(x + w, y);
	mbVertex2f(x + w, y + h);
	mbVertex2f(x, y + h);

	mbEnd();

	mbEnable(MB_TEXTURE_2D);
}

/*
================
DrawBoxes

DrawBox for count boxes in one draw, after the stretch pics before it (the Metal port's crosshair)
================
*/
void DrawBoxes(const float (*boxes)[4], int count, const float *color) {
	int i;

	R_IssuePendingRenderCommands();
	if (tess.numIndexes) {
		RB_EndSurface();	// the stretch pics before it, in the 2D color of now
	}

	mbColor4f(color[0], color[1], color[2], color[3]);
	mbDisable(MB_TEXTURE_2D);
	R_State(GLS_DEPTHTEST_DISABLE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_SRCBLEND_SRC_ALPHA);

	mbBegin(MB_QUADS);
	for (i = 0; i < count; i++) {
		const float *b = boxes[i];

		mbVertex2f(b[0], b[1]);
		mbVertex2f(b[0] + b[2], b[1]);
		mbVertex2f(b[0] + b[2], b[1] + b[3]);
		mbVertex2f(b[0], b[1] + b[3]);
	}
	mbEnd();

	mbColor4ubv(backEnd.color2D);	// as DrawBox leaves it
	mbEnable(MB_TEXTURE_2D);
}

/*
================
DrawBox
================
*/
void DrawBox(float x, float y, float w, float h) {
	R_IssuePendingRenderCommands();

	mbColor4ubv(backEnd.color2D);
	mbDisable(MB_TEXTURE_2D);
	R_State(GLS_DEPTHTEST_DISABLE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_SRCBLEND_SRC_ALPHA);

	mbBegin(MB_QUADS);

	mbVertex2f(x, y);
	mbVertex2f(x + w, y);
	mbVertex2f(x + w, y + h);
	mbVertex2f(x, y + h);

	mbEnd();

	mbEnable(MB_TEXTURE_2D);
}

/*
================
DrawLineLoop
================
*/
void DrawLineLoop(const vec2_t* points, int count, int stipple_factor, int stipple_mask) {
	int		i;

	R_IssuePendingRenderCommands();

	mbDisable(MB_TEXTURE_2D);

	if (stipple_factor) {
		mbEnable(MB_LINE_STIPPLE);
		mbLineStipple(stipple_factor, stipple_mask);
	}

	mbBegin(MB_LINE_LOOP);

	for (i = 0; i < count; i++) {
		mbVertex2f(points[i][0], points[i][1]);
	}

	mbEnd();

	mbEnable(MB_TEXTURE_2D);

	if (stipple_factor) {
		mbDisable(MB_LINE_STIPPLE);
	}
}

/*
================
Set2DWindow
================
*/
void Set2DWindow(int x, int y, int w, int h, float left, float right, float bottom, float top, float n, float f) {
	R_IssuePendingRenderCommands();
	mbViewport(x, y, w, h);
	mbScissor(x, y, w, h);
	mbMatrixMode(MB_PROJECTION);
	mbLoadIdentity();
	mbOrtho(left, right, bottom, top, n, f);
	mbMatrixMode(MB_MODELVIEW);

	mbLoadIdentity();
	R_State(GLS_DEPTHTEST_DISABLE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_SRCBLEND_SRC_ALPHA);
	mbEnable(MB_BLEND);
	mbDisable(MB_CULL_FACE);
	mbDisable(MB_CLIP_PLANE0);
	// Make sure to disable the fog to avoid messing up with the UI
	mbDisable(MB_FOG);
	mbFogf(MB_FOG_START, 0);

	if (r_reset_tc_array->integer) {
		mbDisableClientState(MB_TEXTURE_COORD_ARRAY);
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
	mbEnable(MB_SCISSOR_TEST);
	mbScissor(x, y, width, height);
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
