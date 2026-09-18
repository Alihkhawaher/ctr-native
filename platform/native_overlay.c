// Host-side overlay: for a couple of seconds after startup or a graphics
// option change, draws the active options straight to the default framebuffer
// (after the VRAM present, before the buffer swap). It never passes through
// the PSX pipeline, so nothing lands in emulated VRAM. When the FPS counter is
// enabled it also draws a small always-on frame-rate readout.

#include "platform/native_overlay.h"

#include <platform/native_glad.h>
#include <platform/native_log.h>
#include <platform/native_renderer.h>

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

// Public-domain 8x8 bitmap font bundled with SDL. Each glyph is 8 bytes, top
// row first; bit 0 of a byte is the leftmost pixel.
#include "../externals/SDL/src/render/SDL_render_debug_font.h"

extern int g_cfg_aspectRatio;
extern int g_cfg_bilinearFiltering;
extern int g_cfg_antialiasing;
extern int g_cfg_internalResolutionAuto;
extern int g_cfg_internalResolutionScale;
extern int g_windowHeight;
extern int g_windowWidth;
extern SDL_Window *g_window;

int g_cfg_showFps = 0;

#define NATIVE_OVERLAY_DURATION_SECONDS 2.0

#define NATIVE_OVERLAY_SCALE    2
#define NATIVE_OVERLAY_LINE_GAP 4
#define NATIVE_OVERLAY_PADDING  10
#define NATIVE_OVERLAY_MARGIN   14
#define NATIVE_OVERLAY_LINES    13

#define NATIVE_OVERLAY_ATLAS_COLUMNS 16
#define NATIVE_OVERLAY_ATLAS_ROWS    12
#define NATIVE_OVERLAY_ATLAS_WIDTH   (NATIVE_OVERLAY_ATLAS_COLUMNS * 8)
#define NATIVE_OVERLAY_ATLAS_HEIGHT  (NATIVE_OVERLAY_ATLAS_ROWS * 8)

#define NATIVE_OVERLAY_MAX_VERTICES 2048

typedef struct
{
	f32 x, y, u, v;
} NativeOverlayVertex;

global_variable GLuint s_overlayShader = 0;
global_variable GLuint s_overlayFontTexture = 0;
global_variable GLuint s_overlayVAO = 0;
global_variable GLuint s_overlayVBO = 0;
global_variable GLint s_overlayWinLoc = -1;
global_variable GLint s_overlayColorLoc = -1;
global_variable GLint s_overlaySolidLoc = -1;
global_variable GLint s_overlayFontLoc = -1;
global_variable int s_overlayInitialized = 0;
global_variable int s_overlayActive = 0;
global_variable int s_overlayRequested = 0;
global_variable u64 s_overlayStartCounter = 0;
global_variable u64 s_overlayFpsCounter = 0;
global_variable int s_overlayFpsFrames = 0;
global_variable f64 s_overlayFpsValue = 0.0;
global_variable NativeOverlayVertex s_overlayVertices[NATIVE_OVERLAY_MAX_VERTICES];

// NOTE: Compiled through NativeRenderer_Shader_Compile (native_renderer.c),
// which prepends the shared #version 140 preamble and binds the ShaderAttrib
// locations. Positions arrive in window pixels (y down); the atlas is sampled
// with NEAREST filtering so integer scales stay pixel-exact.
global_variable const char *ctr_overlay_shader = "#ifdef VERTEX\n"
                                                "attribute vec2 a_position;\n"
                                                "attribute vec2 a_texcoord;\n"
                                                "varying vec2 v_uv;\n"
                                                "uniform vec2 u_win;\n"
                                                "void main() {\n"
                                                "\tv_uv = a_texcoord;\n"
                                                "\tvec2 ndc = vec2((a_position.x / u_win.x) * 2.0 - 1.0, 1.0 - (a_position.y / u_win.y) * 2.0);\n"
                                                "\tgl_Position = vec4(ndc, 0.0, 1.0);\n"
                                                "}\n"
                                                "#endif\n"
                                                "#ifdef FRAGMENT\n"
                                                "varying vec2 v_uv;\n"
                                                "uniform sampler2D s_font;\n"
                                                "uniform vec4 u_color;\n"
                                                "uniform int u_solid;\n"
                                                "void main() {\n"
                                                "\tif (u_solid != 0) {\n"
                                                "\t\tfragColor = u_color;\n"
                                                "\t} else {\n"
                                                "\t\tfloat coverage = texture2D(s_font, v_uv).r;\n"
                                                "\t\tfragColor = vec4(u_color.rgb, u_color.a * coverage);\n"
                                                "\t}\n"
                                                "}\n"
                                                "#endif\n";

internal void NativeOverlay_PushVertex(int *count, f32 x, f32 y, f32 u, f32 v)
{
	if (*count >= NATIVE_OVERLAY_MAX_VERTICES)
	{
		return;
	}

	s_overlayVertices[*count].x = x;
	s_overlayVertices[*count].y = y;
	s_overlayVertices[*count].u = u;
	s_overlayVertices[*count].v = v;
	(*count)++;
}

internal void NativeOverlay_PushQuad(int *count, f32 x, f32 y, f32 width, f32 height, f32 u0, f32 v0, f32 u1, f32 v1)
{
	const f32 x1 = x + width;
	const f32 y1 = y + height;

	// Two triangles; screen y grows downwards, and the top edge samples v0 so
	// the first atlas row (the top of the glyph) stays on top.
	NativeOverlay_PushVertex(count, x, y, u0, v0);
	NativeOverlay_PushVertex(count, x, y1, u0, v1);
	NativeOverlay_PushVertex(count, x1, y1, u1, v1);

	NativeOverlay_PushVertex(count, x, y, u0, v0);
	NativeOverlay_PushVertex(count, x1, y1, u1, v1);
	NativeOverlay_PushVertex(count, x1, y, u1, v0);
}

internal void NativeOverlay_PushText(int *count, f32 x, f32 y, int scale, const char *text)
{
	const f32 glyphWidth = (f32)(8 * scale);
	const f32 glyphHeight = (f32)(8 * scale);
	f32 cursorX = x;

	while (*text != '\0')
	{
		const u8 character = (u8)*text;

		if ((character >= 0x21) && (character <= 0x7E) && ((*count + 6) <= NATIVE_OVERLAY_MAX_VERTICES))
		{
			const int glyphIndex = character - 0x21;
			const int cellColumn = glyphIndex % NATIVE_OVERLAY_ATLAS_COLUMNS;
			const int cellRow = glyphIndex / NATIVE_OVERLAY_ATLAS_COLUMNS;
			const f32 u0 = (f32)(cellColumn * 8) / NATIVE_OVERLAY_ATLAS_WIDTH;
			const f32 v0 = (f32)(cellRow * 8) / NATIVE_OVERLAY_ATLAS_HEIGHT;
			const f32 u1 = (f32)(cellColumn * 8 + 8) / NATIVE_OVERLAY_ATLAS_WIDTH;
			const f32 v1 = (f32)(cellRow * 8 + 8) / NATIVE_OVERLAY_ATLAS_HEIGHT;

			NativeOverlay_PushQuad(count, cursorX, y, glyphWidth, glyphHeight, u0, v0, u1, v1);
		}

		cursorX += glyphWidth;
		text++;
	}
}

void NativeOverlay_Init(void)
{
	u8 atlas[NATIVE_OVERLAY_ATLAS_WIDTH * NATIVE_OVERLAY_ATLAS_HEIGHT];
	int glyph;
	int row;
	int column;

	if (s_overlayInitialized != 0)
	{
		return;
	}

	SDL_memset(atlas, 0, sizeof(atlas));

	for (glyph = 0; glyph < SDL_DEBUG_FONT_NUM_GLYPHS; glyph++)
	{
		const u8 *source = SDL_RenderDebugTextFontData + (glyph * 8);
		const int cellColumn = glyph % NATIVE_OVERLAY_ATLAS_COLUMNS;
		const int cellRow = glyph / NATIVE_OVERLAY_ATLAS_COLUMNS;

		for (row = 0; row < 8; row++)
		{
			u8 *destination = atlas + ((cellRow * 8 + row) * NATIVE_OVERLAY_ATLAS_WIDTH) + (cellColumn * 8);

			for (column = 0; column < 8; column++)
			{
				destination[column] = ((source[row] >> column) & 1) != 0 ? 255 : 0;
			}
		}
	}

	glGenTextures(1, &s_overlayFontTexture);
	glBindTexture(GL_TEXTURE_2D, s_overlayFontTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, NATIVE_OVERLAY_ATLAS_WIDTH, NATIVE_OVERLAY_ATLAS_HEIGHT, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
	glBindTexture(GL_TEXTURE_2D, 0);

	s_overlayShader = NativeRenderer_Shader_Compile(ctr_overlay_shader, false);
	s_overlayWinLoc = glGetUniformLocation(s_overlayShader, "u_win");
	s_overlayColorLoc = glGetUniformLocation(s_overlayShader, "u_color");
	s_overlaySolidLoc = glGetUniformLocation(s_overlayShader, "u_solid");
	s_overlayFontLoc = glGetUniformLocation(s_overlayShader, "s_font");

	glUseProgram(s_overlayShader);
	glUniform1i(s_overlayFontLoc, 0);
	glUseProgram(0);

	glGenVertexArrays(1, &s_overlayVAO);
	glGenBuffers(1, &s_overlayVBO);
	glBindVertexArray(s_overlayVAO);
	glBindBuffer(GL_ARRAY_BUFFER, s_overlayVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(s_overlayVertices), NULL, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(a_position);
	glVertexAttribPointer(a_position, 2, GL_FLOAT, GL_FALSE, sizeof(NativeOverlayVertex), (void *)0);
	glEnableVertexAttribArray(a_texcoord);
	glVertexAttribPointer(a_texcoord, 2, GL_FLOAT, GL_FALSE, sizeof(NativeOverlayVertex), (void *)(2 * sizeof(f32)));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	s_overlayInitialized = 1;

	Platform_Log("[CTR Native] Overlay ready\n");
}

void NativeOverlay_Shutdown(void)
{
	if (s_overlayInitialized == 0)
	{
		return;
	}

	s_overlayInitialized = 0;
	s_overlayActive = 0;
	s_overlayRequested = 0;

	glDeleteTextures(1, &s_overlayFontTexture);
	s_overlayFontTexture = 0;
	glDeleteProgram(s_overlayShader);
	s_overlayShader = 0;
	glDeleteBuffers(1, &s_overlayVBO);
	s_overlayVBO = 0;
	glDeleteVertexArrays(1, &s_overlayVAO);
	s_overlayVAO = 0;
}

void NativeOverlay_Show(void)
{
	s_overlayRequested = 1;
}

void NativeOverlay_Draw(void)
{
	static const char *s_aspectNames[3] = {"Auto", "4:3", "16:9"};
	char labels[NATIVE_OVERLAY_LINES][24];
	char values[NATIVE_OVERLAY_LINES][24];
	char hints[NATIVE_OVERLAY_LINES][24];
	char fpsText[32];
	int labelColumn = 0;
	int valueColumn = 0;
	int hintColumn = 0;
	int scale = NATIVE_OVERLAY_SCALE;
	int vertexCount = 0;
	int solidVertexCount;
	int textVertexCount;
	int hintVertexCount;
	int panelVisible;
	int fpsVisible;
	int row;
	f32 advance;
	f32 panelX;
	f32 panelY;
	f32 panelWidth;
	f32 panelHeight;

	GLint previousViewport[4];
	GLint previousProgram;
	GLint previousVao;
	GLint previousTexture;
	GLint previousActiveTexture;
	GLint previousBlendSrcRgb;
	GLint previousBlendDstRgb;
	GLint previousBlendSrcAlpha;
	GLint previousBlendDstAlpha;
	GLint previousBlendEquationRgb;
	GLint previousBlendEquationAlpha;
	GLboolean blendWasEnabled;
	GLboolean depthWasEnabled;
	GLboolean stencilWasEnabled;

	if (s_overlayInitialized == 0)
	{
		return;
	}

	// Frame-rate accounting runs every frame so the readout is live even when
	// the options panel is hidden.
	{
		const u64 frequency = SDL_GetPerformanceFrequency();
		const u64 now = SDL_GetPerformanceCounter();

		if (s_overlayFpsCounter == 0)
		{
			s_overlayFpsCounter = now;
			s_overlayFpsFrames = 0;
		}
		else
		{
			s_overlayFpsFrames++;

			if (frequency > 0)
			{
				const f64 elapsed = (f64)(now - s_overlayFpsCounter) / (f64)frequency;

				if (elapsed >= 0.5)
				{
					s_overlayFpsValue = (f64)s_overlayFpsFrames / elapsed;
					s_overlayFpsCounter = now;
					s_overlayFpsFrames = 0;
				}
			}
		}
	}

	if (s_overlayRequested != 0)
	{
		s_overlayRequested = 0;
		s_overlayActive = 1;
		s_overlayStartCounter = SDL_GetPerformanceCounter();
	}

	if (s_overlayActive != 0)
	{
		const u64 frequency = SDL_GetPerformanceFrequency();
		const u64 now = SDL_GetPerformanceCounter();

		if ((frequency == 0) || ((f64)(now - s_overlayStartCounter) / (f64)frequency >= NATIVE_OVERLAY_DURATION_SECONDS))
		{
			s_overlayActive = 0;
		}
	}

	panelVisible = s_overlayActive;
	fpsVisible = (g_cfg_showFps != 0);

	if ((panelVisible == 0) && (fpsVisible == 0))
	{
		return;
	}

	if ((g_window == NULL) || (g_windowWidth <= 0) || (g_windowHeight <= 0))
	{
		return;
	}

	snprintf(labels[0], sizeof(labels[0]), "Internal res");
	if (g_cfg_internalResolutionAuto != 0)
	{
		snprintf(values[0], sizeof(values[0]), "Auto (%dx)", g_cfg_internalResolutionScale);
	}
	else
	{
		snprintf(values[0], sizeof(values[0]), "%dx", g_cfg_internalResolutionScale);
	}
	snprintf(hints[0], sizeof(hints[0]), "PgUp/PgDn");

	snprintf(labels[1], sizeof(labels[1]), "Bilinear");
	snprintf(values[1], sizeof(values[1]), "%s", (g_cfg_bilinearFiltering != 0) ? "ON" : "OFF");
	snprintf(hints[1], sizeof(hints[1]), "F3");

	snprintf(labels[2], sizeof(labels[2]), "Aspect");
	snprintf(values[2], sizeof(values[2]), "%s", s_aspectNames[(g_cfg_aspectRatio >= 0 && g_cfg_aspectRatio < 3) ? g_cfg_aspectRatio : 0]);
	snprintf(hints[2], sizeof(hints[2]), "Home");

	snprintf(labels[3], sizeof(labels[3]), "Window");
	snprintf(values[3], sizeof(values[3]), "%d x %d", g_windowWidth, g_windowHeight);
	snprintf(hints[3], sizeof(hints[3]), "End");

	snprintf(labels[4], sizeof(labels[4]), "Fullscreen");
	snprintf(values[4], sizeof(values[4]), "%s", ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0) ? "ON" : "OFF");
	snprintf(hints[4], sizeof(hints[4]), "F11 / Alt+Enter");

	snprintf(labels[5], sizeof(labels[5]), "FPS counter");
	snprintf(values[5], sizeof(values[5]), "%s", (g_cfg_showFps != 0) ? "ON" : "OFF");
	snprintf(hints[5], sizeof(hints[5]), "Insert");

	snprintf(labels[6], sizeof(labels[6]), "Anti-aliasing");
	snprintf(values[6], sizeof(values[6]), "%s", (g_cfg_antialiasing != 0) ? "ON" : "OFF");
	snprintf(hints[6], sizeof(hints[6]), "Tab");

	// Complete controls reference (all native buttons).
	snprintf(labels[7], sizeof(labels[7]), "Save / load state");
	values[7][0] = '\0';
	snprintf(hints[7], sizeof(hints[7]), "F5 / F8");

	snprintf(labels[8], sizeof(labels[8]), "Screenshot");
	values[8][0] = '\0';
	snprintf(hints[8], sizeof(hints[8]), "F12");

	snprintf(labels[9], sizeof(labels[9]), "VRAM dump");
	values[9][0] = '\0';
	snprintf(hints[9], sizeof(hints[9]), "F7");

	snprintf(labels[10], sizeof(labels[10]), "Replay rec / stop");
	values[10][0] = '\0';
	snprintf(hints[10], sizeof(hints[10]), "F9 / F10");

	snprintf(labels[11], sizeof(labels[11]), "Keyboard / pad assign");
	values[11][0] = '\0';
	snprintf(hints[11], sizeof(hints[11]), "F4 / F6");

	snprintf(labels[12], sizeof(labels[12]), "Wireframe / texless");
	values[12][0] = '\0';
	snprintf(hints[12], sizeof(hints[12]), "F1 / F2");

	snprintf(fpsText, sizeof(fpsText), "FPS: %.1f", s_overlayFpsValue);

	for (row = 0; row < NATIVE_OVERLAY_LINES; row++)
	{
		const int labelLength = (int)strlen(labels[row]);
		const int valueLength = (int)strlen(values[row]);
		const int hintLength = (int)strlen(hints[row]);

		if (labelLength > labelColumn)
		{
			labelColumn = labelLength;
		}
		if (valueLength > valueColumn)
		{
			valueColumn = valueLength;
		}
		if (hintLength > hintColumn)
		{
			hintColumn = hintLength;
		}
	}

	// Shrink the glyph scale when the panel would overflow a small window.
	while ((scale > 1) && ((((labelColumn + 2 + valueColumn + 2 + hintColumn) * 8 * scale) + (2 * NATIVE_OVERLAY_PADDING) + (2 * NATIVE_OVERLAY_MARGIN)) > g_windowWidth))
	{
		scale--;
	}

	advance = (f32)(8 * scale);
	panelX = (f32)NATIVE_OVERLAY_MARGIN;
	panelY = (f32)NATIVE_OVERLAY_MARGIN;
	panelWidth = (f32)((labelColumn + 2 + valueColumn + 2 + hintColumn) * 8 * scale) + (2 * NATIVE_OVERLAY_PADDING);
	panelHeight = (f32)((NATIVE_OVERLAY_LINES * 8 * scale) + ((NATIVE_OVERLAY_LINES - 1) * NATIVE_OVERLAY_LINE_GAP)) + (2 * NATIVE_OVERLAY_PADDING);

	// Solids: options panel + FPS readout box.
	if (panelVisible != 0)
	{
		NativeOverlay_PushQuad(&vertexCount, panelX, panelY, panelWidth, panelHeight, 0.0f, 0.0f, 0.0f, 0.0f);
	}

	if (fpsVisible != 0)
	{
		const f32 fpsWidth = (f32)(((int)strlen(fpsText) * 8 * scale) + (2 * NATIVE_OVERLAY_PADDING));
		const f32 fpsX = (f32)g_windowWidth - (f32)NATIVE_OVERLAY_MARGIN - fpsWidth;

		NativeOverlay_PushQuad(&vertexCount, fpsX, panelY, fpsWidth, (f32)((8 * scale) + (2 * NATIVE_OVERLAY_PADDING)), 0.0f, 0.0f, 0.0f, 0.0f);
	}

	solidVertexCount = vertexCount;

	// Text: options panel rows + the FPS readout text.
	if (panelVisible != 0)
	{
		for (row = 0; row < NATIVE_OVERLAY_LINES; row++)
		{
			const f32 rowY = panelY + NATIVE_OVERLAY_PADDING + (f32)(row * ((8 * scale) + NATIVE_OVERLAY_LINE_GAP));
			const f32 labelX = panelX + NATIVE_OVERLAY_PADDING;
			const f32 valueX = labelX + (f32)(labelColumn + 2) * advance;

			NativeOverlay_PushText(&vertexCount, labelX, rowY, scale, labels[row]);
			NativeOverlay_PushText(&vertexCount, valueX, rowY, scale, values[row]);
		}
	}

	if (fpsVisible != 0)
	{
		const f32 fpsWidth = (f32)(((int)strlen(fpsText) * 8 * scale) + (2 * NATIVE_OVERLAY_PADDING));
		const f32 fpsX = (f32)g_windowWidth - (f32)NATIVE_OVERLAY_MARGIN - fpsWidth;

		NativeOverlay_PushText(&vertexCount, fpsX + NATIVE_OVERLAY_PADDING, panelY + NATIVE_OVERLAY_PADDING, scale, fpsText);
	}

	textVertexCount = vertexCount - solidVertexCount;

	// Dimmed key hints.
	if (panelVisible != 0)
	{
		for (row = 0; row < NATIVE_OVERLAY_LINES; row++)
		{
			const f32 rowY = panelY + NATIVE_OVERLAY_PADDING + (f32)(row * ((8 * scale) + NATIVE_OVERLAY_LINE_GAP));
			const f32 hintX = panelX + NATIVE_OVERLAY_PADDING + ((f32)(labelColumn + 2 + valueColumn + 2) * advance);

			NativeOverlay_PushText(&vertexCount, hintX, rowY, scale, hints[row]);
		}
	}
	hintVertexCount = vertexCount - solidVertexCount - textVertexCount;

	// Save every piece of GL state the overlay touches. The renderer caches
	// several of these across frames, so they must be restored exactly.
	glGetIntegerv(GL_VIEWPORT, previousViewport);
	glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
	glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSrcRgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDstRgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSrcAlpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDstAlpha);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &previousBlendEquationRgb);
	glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &previousBlendEquationAlpha);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	blendWasEnabled = glIsEnabled(GL_BLEND);
	depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
	stencilWasEnabled = glIsEnabled(GL_STENCIL_TEST);

	glViewport(0, 0, g_windowWidth, g_windowHeight);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glEnable(GL_BLEND);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(s_overlayShader);
	glUniform2f(s_overlayWinLoc, (f32)g_windowWidth, (f32)g_windowHeight);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s_overlayFontTexture);
	glBindVertexArray(s_overlayVAO);
	glBindBuffer(GL_ARRAY_BUFFER, s_overlayVBO);
	glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(vertexCount * sizeof(NativeOverlayVertex)), s_overlayVertices);

	glUniform1i(s_overlaySolidLoc, 1);
	glUniform4f(s_overlayColorLoc, 0.0f, 0.0f, 0.0f, 0.65f);
	glDrawArrays(GL_TRIANGLES, 0, solidVertexCount);

	glUniform1i(s_overlaySolidLoc, 0);
	glUniform4f(s_overlayColorLoc, 1.0f, 1.0f, 1.0f, 1.0f);
	glDrawArrays(GL_TRIANGLES, solidVertexCount, textVertexCount);

	glUniform4f(s_overlayColorLoc, 0.72f, 0.72f, 0.72f, 1.0f);
	glDrawArrays(GL_TRIANGLES, solidVertexCount + textVertexCount, hintVertexCount);

	glBindVertexArray(previousVao);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, previousTexture);
	glActiveTexture(previousActiveTexture);
	glUseProgram(previousProgram);
	glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
	glBlendEquationSeparate(previousBlendEquationRgb, previousBlendEquationAlpha);
	glBlendFuncSeparate(previousBlendSrcRgb, previousBlendDstRgb, previousBlendSrcAlpha, previousBlendDstAlpha);

	if (blendWasEnabled != 0)
	{
		glEnable(GL_BLEND);
	}
	else
	{
		glDisable(GL_BLEND);
	}

	if (depthWasEnabled != 0)
	{
		glEnable(GL_DEPTH_TEST);
	}
	else
	{
		glDisable(GL_DEPTH_TEST);
	}

	if (stencilWasEnabled != 0)
	{
		glEnable(GL_STENCIL_TEST);
	}
	else
	{
		glDisable(GL_STENCIL_TEST);
	}
}
