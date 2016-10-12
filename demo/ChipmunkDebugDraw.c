/* Copyright (c) 2007 Scott Lembcke
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <limits.h>
#include <string.h>

#include "Photon.h"

#include "chipmunk/chipmunk_private.h"
#include "ChipmunkDebugDraw.h"

#include "VeraMoBd.ttf_sdf.h"


#define TextScale 0.70f
#define TextLineHeight (18.0f*TextScale)

float ChipmunkDebugDrawScaleFactor = 1.0f;
cpTransform ChipmunkDebugDrawProjection = {1, 0, 0, 1, 0, 0};
cpTransform ChipmunkDebugDrawCamera = {1, 0, 0, 1, 0, 0};

static pvec4 Palette[16];

static PhotonRenderer *Renderer = NULL;
static PhotonRenderState *FontState = NULL;
static PhotonRenderState *PrimitiveState = NULL;

// char -> glyph indexes generated by the lonesock tool.
static int glyph_indexes[256];

static const char *PrimitiveVShader = PHOTON_GLSL(
	in vec4 PhotonAttributePosition;
	in vec2 PhotonAttributeUV1;
	in vec2 PhotonAttributeUV2;
	in vec4 PhotonAttributeColor;
	
	out vec2 PhotonFragUV1;
	out vec2 PhotonFragUV2;
	out vec4 PhotonFragColor;
	
	layout(std140) uniform;
	uniform PhotonGlobals {
		mat4 u_P;
		mat4 u_MVP;
		
		vec4 u_Palette[16];
		float u_OutlineWidth;
	};
	
	void main(void){
		gl_Position = u_MVP*PhotonAttributePosition;
		PhotonFragUV1 = PhotonAttributeUV1;
		PhotonFragUV2 = PhotonAttributeUV2;
		PhotonFragColor = PhotonAttributeColor*PhotonAttributeColor.a;
	}
);

static const char *PrimitiveFShader = PHOTON_GLSL(
	in vec2 PhotonFragUV1;
	in vec2 PhotonFragUV2;
	in vec4 PhotonFragColor;
	
	out vec4 PhotonFragOut;
	
	layout(std140) uniform;
	uniform PhotonGlobals {
		mat4 u_P;
		mat4 u_MVP;
		
		vec4 u_Palette[16];
		float u_OutlineWidth;
	};
	
	void main(void){
		float r1 = PhotonFragUV2[0];
		float r2 = PhotonFragUV2[1];
		
		float l = length(PhotonFragUV1);
		float fw = fwidth(l);
		
		// Fill/outline color.
		float outlineWidth = fw*u_OutlineWidth;
		vec4 outlineColor = u_Palette[15];
		float outline = smoothstep(r1, r1 - fw, l);
		
		// Use pre-multiplied alpha.
		vec4 color = mix(outlineColor, PhotonFragColor, outline);
		float mask = smoothstep(r2, r2 - fw, l);
		PhotonFragOut = color*mask;
	}
);

static const char *FontVShader = PHOTON_GLSL(
	in vec4 PhotonAttributePosition;
	in vec2 PhotonAttributeUV1;
	in vec4 PhotonAttributeColor;
	
	out vec2 PhotonFragUV1;
	out vec4 PhotonFragColor;
	
	layout(std140) uniform;
	uniform PhotonGlobals {
		mat4 u_P;
		mat4 u_MVP;
		
		vec4 u_Palette[16];
	};
	
	void main(void){
		gl_Position = u_P*PhotonAttributePosition;
		PhotonFragUV1 = PhotonAttributeUV1;
		PhotonFragColor = PhotonAttributeColor;
	}
);

static const char *FontFShader = PHOTON_GLSL(
	in vec2 PhotonFragUV1;
	in vec2 PhotonFragUV2;
	in vec4 PhotonFragColor;
	
	out vec4 PhotonFragOut;
	
	uniform sampler2D u_FontAtlas;
	
	void main(void){
		float sdf = texture(u_FontAtlas, PhotonFragUV1).r;
		float fw = 0.5*fwidth(sdf);
		float mask = smoothstep(0.5 - fw, 0.5 + fw, sdf);
		
		PhotonFragOut = PhotonFragColor*mask;
	}
);

void
ChipmunkDebugDrawInit(void)
{
	Renderer = PhotonRendererNew();
	
	const pvec4 palette[] = {
		{{ 20,  12,  28, 255}},
		{{ 68,  36,  52, 255}},
		{{ 48,  52, 109, 255}},
		{{ 78,  74,  78, 255}},
		{{133,  76,  48, 255}},
		{{ 52, 101,  36, 255}},
		{{208,  70,  72, 255}},
		{{117, 113,  97, 255}},
		{{ 89, 125, 206, 255}},
		{{210, 125,  44, 255}},
		{{133, 149, 161, 255}},
		{{109, 170,  44, 255}},
		{{210, 170, 153, 255}},
		{{109, 194, 202, 255}},
		{{218, 212,  94, 255}},
		{{222, 238, 214, 255}},
	};
	
	for(int i = 0; i < 16; i++){
		Palette[i] = pvec4Mult(palette[i], 1.0/255.0);
	}
	
	PhotonShader *primitiveShader = PhotonShaderNew(PrimitiveVShader, PrimitiveFShader);
	PhotonUniforms *primitiveUniforms = PhotonUniformsNew(primitiveShader);
	
	PrimitiveState = PhotonRenderStateNew(&PhotonBlendModePremultipliedAlpha, primitiveUniforms);
	
	PhotonTextureOptions fontAtlasOptions = PhotonTextureOptionsDefault;
	fontAtlasOptions.format = PhotonTextureFormatR8;
	PhotonTexture *fontAtlas = PhotonTextureNew(sdf_tex_width, sdf_tex_height, sdf_data, &fontAtlasOptions);
	
	PhotonShader *fontShader = PhotonShaderNew(FontVShader, FontFShader);
	PhotonUniforms *fontUniforms = PhotonUniformsNew(fontShader);
	PhotonUniformsSetTexture(fontUniforms, "u_FontAtlas", fontAtlas);
	
	FontState = PhotonRenderStateNew(&PhotonBlendModePremultipliedAlpha, fontUniforms);
	
	// Fill in the glyph index array.
	for(int i=0; i<sdf_num_chars; i++){
		int char_index = sdf_spacing[i*8];
		glyph_indexes[char_index] = i;
	}
}

static inline pvec4 MakeColor(cpSpaceDebugColor c){return (pvec4){{c.r, c.g, c.b, c.a}};}

static void
DrawCircle(pvec2 p, float r1, float r2, pvec4 color)
{
	pvec2 attribs = {r1, cpfmax(r2, 1)};
	
	PhotonRenderBuffers buffers = PhotonRendererEnqueueTriangles(Renderer, 2, 4, PrimitiveState);
	PhotonVertexPush(buffers.vertexes + 0, (pvec4){{p.x - r2, p.y - r2, 0, 1}}, (pvec2){-r2, -r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 1, (pvec4){{p.x - r2, p.y + r2, 0, 1}}, (pvec2){-r2,  r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 2, (pvec4){{p.x + r2, p.y + r2, 0, 1}}, (pvec2){ r2,  r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 3, (pvec4){{p.x + r2, p.y - r2, 0, 1}}, (pvec2){ r2, -r2}, attribs, color);
	PhotonRenderBuffersCopyIndexes(&buffers, (PhotonIndex[]){0, 1, 2, 2, 3, 0}, 0, 6);
}

void
ChipmunkDebugDrawDot(cpFloat size, cpVect pos, cpSpaceDebugColor fill)
{
	float r = size*0.5f*ChipmunkDebugDrawScaleFactor;
	DrawCircle((pvec2){pos.x, pos.y}, r + 1, r, MakeColor(fill));
}

void
ChipmunkDebugDrawCircle(cpVect pos, cpFloat angle, cpFloat radius, cpSpaceDebugColor outline, cpSpaceDebugColor fill)
{
	cpFloat r = radius + 1.0f/ChipmunkDebugDrawScaleFactor;
	DrawCircle((pvec2){pos.x, pos.y}, r - 1, r, MakeColor(fill));
	ChipmunkDebugDrawSegment(pos, cpvadd(pos, cpvmult(cpvforangle(angle), radius - ChipmunkDebugDrawScaleFactor*0.5f)), outline);
}

static void
DrawSegment(cpVect a, cpVect b, float r1, float r2, pvec4 color)
{
	cpVect t = cpvmult(cpvnormalize(cpvsub(b, a)), r2);
	pvec2 attribs = {r1, cpfmax(r2, 1)};
	
	PhotonRenderBuffers buffers = PhotonRendererEnqueueTriangles(Renderer, 6, 8, PrimitiveState);
	PhotonVertexPush(buffers.vertexes + 0, (pvec4){{b.x - t.y + t.x, b.y + t.x + t.y, 0, 1}}, (pvec2){ r2, -r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 1, (pvec4){{b.x + t.y + t.x, b.y - t.x + t.y, 0, 1}}, (pvec2){ r2,  r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 2, (pvec4){{b.x - t.y      , b.y + t.x      , 0, 1}}, (pvec2){  0, -r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 3, (pvec4){{b.x + t.y      , b.y - t.x      , 0, 1}}, (pvec2){  0,  r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 4, (pvec4){{a.x - t.y      , a.y + t.x      , 0, 1}}, (pvec2){  0, -r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 5, (pvec4){{a.x + t.y      , a.y - t.x      , 0, 1}}, (pvec2){  0,  r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 6, (pvec4){{a.x - t.y - t.x, a.y + t.x - t.y, 0, 1}}, (pvec2){-r2, -r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 7, (pvec4){{a.x + t.y - t.x, a.y - t.x - t.y, 0, 1}}, (pvec2){-r2,  r2}, attribs, color);
	PhotonRenderBuffersCopyIndexes(&buffers, (PhotonIndex[]){0, 1, 2, 3, 1, 2, 3, 4, 2, 3, 4, 5, 6, 4, 5, 6, 7, 5}, 0, 18);
}

void
ChipmunkDebugDrawSegment(cpVect a, cpVect b, cpSpaceDebugColor color)
{
	DrawSegment(a, b, 2, 1, MakeColor(color));
}

void
ChipmunkDebugDrawFatSegment(cpVect a, cpVect b, cpFloat radius, cpSpaceDebugColor outline, cpSpaceDebugColor fill)
{
	float r = fmaxf(radius + 1/ChipmunkDebugDrawScaleFactor, 1);
	DrawSegment(a, b, r - 1, r, MakeColor(fill));
}

extern cpVect ChipmunkDemoMouse;

void
ChipmunkDebugDrawPolygon(int count, const cpVect *verts, cpFloat radius, cpSpaceDebugColor outline, cpSpaceDebugColor fill)
{
	pvec2 attribs = {1, 1};
	pvec4 color = MakeColor(fill);
	
	PhotonRenderBuffers buffers = PhotonRendererEnqueueTriangles(Renderer, count - 2, count, PrimitiveState);
	
	for(int i = 0; i < count; i++){
		buffers.vertexes = PhotonVertexPush(buffers.vertexes, (pvec4){{verts[i].x, verts[i].y, 0, 1}}, PVEC2_0, attribs, color);
	}
	
	for(int i = 0; i < count - 2; i++){
		buffers.indexes = PhotonIndexesCopy(buffers.indexes, (PhotonIndex[]){0, i + 1, i + 2}, 0, 3, buffers.batchOffset);
	}
}

/*
	struct ExtrudeVerts {cpVect offset, n;};
	size_t bytes = sizeof(struct ExtrudeVerts)*count;
	struct ExtrudeVerts *extrude = (struct ExtrudeVerts *)alloca(bytes);
	memset(extrude, 0, bytes);
	
	for(int i=0; i<count; i++){
		cpVect v0 = verts[(i-1+count)%count];
		cpVect v1 = verts[i];
		cpVect v2 = verts[(i+1)%count];
		
		cpVect n1 = cpvnormalize(cpvrperp(cpvsub(v1, v0)));
		cpVect n2 = cpvnormalize(cpvrperp(cpvsub(v2, v1)));
		
		cpVect offset = cpvmult(cpvadd(n1, n2), 1.0/(cpvdot(n1, n2) + 1.0f));
		struct ExtrudeVerts v = {offset, n2}; extrude[i] = v;
	}
	
//	Triangle *triangles = PushTriangles(6*count);
	Triangle *triangles = PushTriangles(5*count - 2);
	Triangle *cursor = triangles;
	
	cpFloat inset = -cpfmax(0.0f, 1.0f/ChipmunkDebugDrawPointLineScale - radius);
	for(int i=0; i<count-2; i++){
		struct v2f v0 = v2f(cpvadd(verts[  0], cpvmult(extrude[  0].offset, inset)));
		struct v2f v1 = v2f(cpvadd(verts[i+1], cpvmult(extrude[i+1].offset, inset)));
		struct v2f v2 = v2f(cpvadd(verts[i+2], cpvmult(extrude[i+2].offset, inset)));
		
		Triangle t = {{v0, v2f0, fillColor, fillColor}, {v1, v2f0, fillColor, fillColor}, {v2, v2f0, fillColor, fillColor}}; *cursor++ = t;
	}
	
	cpFloat outset = 1.0f/ChipmunkDebugDrawPointLineScale + radius - inset;
	for(int i=0, j=count-1; i<count; j=i, i++){
		cpVect vA = verts[i];
		cpVect vB = verts[j];
		
		cpVect nA = extrude[i].n;
		cpVect nB = extrude[j].n;
		
		cpVect offsetA = extrude[i].offset;
		cpVect offsetB = extrude[j].offset;
		
		cpVect innerA = cpvadd(vA, cpvmult(offsetA, inset));
		cpVect innerB = cpvadd(vB, cpvmult(offsetB, inset));
		
		// Admittedly my variable naming sucks here...
		struct v2f inner0 = v2f(innerA);
		struct v2f inner1 = v2f(innerB);
		struct v2f outer0 = v2f(cpvadd(innerA, cpvmult(nB, outset)));
		struct v2f outer1 = v2f(cpvadd(innerB, cpvmult(nB, outset)));
		struct v2f outer2 = v2f(cpvadd(innerA, cpvmult(offsetA, outset)));
		struct v2f outer3 = v2f(cpvadd(innerA, cpvmult(nA, outset)));
		
		struct v2f n0 = v2f(nA);
		struct v2f n1 = v2f(nB);
		struct v2f offset0 = v2f(offsetA);
		
		Triangle t0 = {{inner0, v2f0, fillColor, outlineColor}, {inner1,    v2f0, fillColor, outlineColor}, {outer1,      n1, fillColor, outlineColor}}; *cursor++ = t0;
		Triangle t1 = {{inner0, v2f0, fillColor, outlineColor}, {outer0,      n1, fillColor, outlineColor}, {outer1,      n1, fillColor, outlineColor}}; *cursor++ = t1;
		Triangle t2 = {{inner0, v2f0, fillColor, outlineColor}, {outer0,      n1, fillColor, outlineColor}, {outer2, offset0, fillColor, outlineColor}}; *cursor++ = t2;
		Triangle t3 = {{inner0, v2f0, fillColor, outlineColor}, {outer2, offset0, fillColor, outlineColor}, {outer3,      n0, fillColor, outlineColor}}; *cursor++ = t3;
	}
*/

void
ChipmunkDebugDrawBB(cpBB bb, cpSpaceDebugColor color)
{
	cpVect verts[] = {
		cpv(bb.r, bb.b),
		cpv(bb.r, bb.t),
		cpv(bb.l, bb.t),
		cpv(bb.l, bb.b),
	};
	ChipmunkDebugDrawPolygon(4, verts, 0.0f, color, LAColor(0, 0));
}


static float
PushChar(int character, float x, float y, pvec4 color)
{
	int i = glyph_indexes[character];
	float w = sdf_tex_width;
	float h = sdf_tex_height;
	
	float gw = sdf_spacing[i*8 + 3];
	float gh = sdf_spacing[i*8 + 4];
	
	float txmin = sdf_spacing[i*8 + 1]/w;
	float tymin = sdf_spacing[i*8 + 2]/h;
	float txmax = txmin + gw/w;
	float tymax = tymin + gh/h;
	
	float s = TextScale/scale_factor;
	float xmin = x + sdf_spacing[i*8 + 5]/scale_factor*TextScale;
	float ymin = y + (sdf_spacing[i*8 + 6]/scale_factor - gh)*TextScale;
	float xmax = xmin + gw*TextScale;
	float ymax = ymin + gh*TextScale;
	
	PhotonRenderBuffers buffers = PhotonRendererEnqueueTriangles(Renderer, 2, 4, FontState);
	PhotonVertexPush(buffers.vertexes + 0, (pvec4){{xmin, ymin, 0, 1}}, (pvec2){txmin, tymax}, PVEC2_0, color);
	PhotonVertexPush(buffers.vertexes + 1, (pvec4){{xmin, ymax, 0, 1}}, (pvec2){txmin, tymin}, PVEC2_0, color);
	PhotonVertexPush(buffers.vertexes + 2, (pvec4){{xmax, ymax, 0, 1}}, (pvec2){txmax, tymin}, PVEC2_0, color);
	PhotonVertexPush(buffers.vertexes + 3, (pvec4){{xmax, ymin, 0, 1}}, (pvec2){txmax, tymax}, PVEC2_0, color);
	PhotonRenderBuffersCopyIndexes(&buffers, (PhotonIndex[]){0, 1, 2, 0, 2, 3}, 0, 6);
	
	return sdf_spacing[i*8 + 7]*s;
	return 0;
}

void
ChipmunkDebugDrawText(cpVect pos, char const *str)
{
	float x = pos.x, y = pos.y;
	
	for(size_t i=0, len=strlen(str); i<len; i++){
		if(str[i] == '\n'){
			y -= TextLineHeight;
			x = pos.x;
		} else {
			x += PushChar(str[i], x, y, Palette[14]);
		}
	}
}

void
ChipmunkDebugDrawBegin(int width, int height)
{
	// TODO Need to make a set of renderers.
	while(!PhotonRendererWait(Renderer, 1)){
		printf("Sync on renderer\n");
	}
	
	PhotonRendererPrepare(Renderer, (pvec2){width, height});
	
	cpTransform p = ChipmunkDebugDrawProjection;
	cpTransform mvp = cpTransformMult(ChipmunkDebugDrawProjection, cpTransformInverse(ChipmunkDebugDrawCamera));
	
	struct {
		float u_P[16];
		float u_MVP[16];
		pvec4 u_Palette[16];
		float u_OutlineWidth;
	} globals = {
		{
			p.a , p.b , 0, 0,
			p.c , p.d , 0, 0,
			0   ,   0 , 1, 0,
			p.tx, p.ty, 0, 1,
		},
		{
			mvp.a , mvp.b , 0, 0,
			mvp.c , mvp.d , 0, 0,
			    0 ,     0 , 1, 0,
			mvp.tx, mvp.ty, 0, 1,
		},
		{},
		ChipmunkDebugDrawScaleFactor,
	};
	
	memcpy(&globals.u_Palette, Palette, sizeof(Palette));
	
	PhotonRendererSetGlobals(Renderer, &globals, sizeof(globals));
	
	PhotonRendererBindRenderTexture(Renderer, NULL, PhotonLoadActionClear, PhotonStoreActionDontCare, Palette[2]);
}

void
ChipmunkDebugDrawFlush(void)
{
	PhotonRendererFlush(Renderer);
}
