#version 430

layout(local_size_x=1, local_size_y=1, local_size_z=1) in;

struct oe_GroundCover_Biome {
    int firstObjectIndex;
    int numObjects;
    float density;
    float fill;
    vec2 maxWidthHeight;
};
void oe_GroundCover_getBiome(in int index, out oe_GroundCover_Biome biome);

struct oe_GroundCover_Object {
    int type;             // 0=billboard 
    int objectArrayIndex; // index into the typed object array 
};
void oe_GroundCover_getObject(in int index, out oe_GroundCover_Object object);

struct oe_GroundCover_Billboard {
    int atlasIndexSide;
    int atlasIndexTop;
    float width;
    float height;
    float sizeVariation;
};
void oe_GroundCover_getBillboard(in int index, out oe_GroundCover_Billboard bb);

// Generated in GroundCover.cpp
int oe_GroundCover_getBiomeIndex(in float code);


struct DrawElementsIndirectCommand
{
    uint count;
    uint instanceCount;
    uint firstIndex;
    uint baseVertex;
    uint baseInstance;
};

layout(binding=0, std430) buffer DrawCommandsBuffer
{
    DrawElementsIndirectCommand cmd[];
};

struct RenderData
{
    vec4 vertex;      // 16
    vec2 tilec;       // 8
    int sideIndex;    // 4
    int  topIndex;    // 4
    float width;      // 4
    float height;     // 4
    float fillEdge;   // 4
    float _padding;   // 4
};

layout(binding=1, std430) writeonly buffer RenderBuffer
{
    RenderData render[];
};

bool inFrustum(in vec4 vertex_view)
{
    vec4 clip = gl_ProjectionMatrix * vertex_view;
    clip.xyz /= clip.w;
    return abs(clip.x) <= 1.01 && clip.y < 1.0;
}

uniform sampler2D oe_GroundCover_noiseTex;
#define NOISE_SMOOTH   0
#define NOISE_RANDOM   1
#define NOISE_RANDOM_2 2
#define NOISE_CLUMPY   3

uniform vec3 oe_GroundCover_LL, oe_GroundCover_UR;
uniform vec2 oe_tile_elevTexelCoeff;
uniform sampler2D oe_tile_elevationTex;
uniform mat4 oe_tile_elevationTexMatrix;
uniform uint oe_GroundCover_tileNum;
uniform float oe_GroundCover_maxDistance;
uniform vec3 oe_Camera;
uniform float oe_GroundCover_colorMinSaturation;

#pragma import_defines(OE_LANDCOVER_TEX)
#pragma import_defines(OE_LANDCOVER_TEX_MATRIX)
uniform sampler2D OE_LANDCOVER_TEX;
uniform mat4 OE_LANDCOVER_TEX_MATRIX;

#pragma import_defines(OE_GROUNDCOVER_MASK_SAMPLER)
#pragma import_defines(OE_GROUNDCOVER_MASK_MATRIX)
#ifdef OE_GROUNDCOVER_MASK_SAMPLER
uniform sampler2D OE_GROUNDCOVER_MASK_SAMPLER;
uniform mat4 OE_GROUNDCOVER_MASK_MATRIX;
#endif

#pragma import_defines(OE_GROUNDCOVER_COLOR_SAMPLER)
#pragma import_defines(OE_GROUNDCOVER_COLOR_MATRIX)
#ifdef OE_GROUNDCOVER_COLOR_SAMPLER
  uniform sampler2D OE_GROUNDCOVER_COLOR_SAMPLER ;
  uniform mat4 OE_GROUNDCOVER_COLOR_MATRIX ;
#endif

#pragma import_defines(OE_GROUNDCOVER_PICK_NOISE_TYPE)
#ifdef OE_GROUNDCOVER_PICK_NOISE_TYPE
  int pickNoiseType = OE_GROUNDCOVER_PICK_NOISE_TYPE ;
#else
  int pickNoiseType = NOISE_RANDOM;
#endif

#ifdef OE_GROUNDCOVER_COLOR_SAMPLER

// https://stackoverflow.com/a/17897228/4218920
vec3 rgb2hsv(vec3 c)
{
    const vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    const float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

bool isLegalColor(in vec2 tilec)
{
    vec4 c = texture(OE_GROUNDCOVER_COLOR_SAMPLER, (OE_GROUNDCOVER_COLOR_MATRIX*vec4(tilec,0,1)).st);
    vec3 hsv = rgb2hsv(c.rgb);
    return hsv[1] > oe_GroundCover_colorMinSaturation;
}

#endif // OE_GROUNDCOVER_COLOR_SAMPLER

bool inRange(in vec4 vertex_view)
{
    float maxRange = oe_GroundCover_maxDistance / oe_Camera.z;
    return (-vertex_view.z <= oe_GroundCover_maxDistance);
}

float getElevation(in vec2 tilec)
{
    vec2 elevc = tilec
       * oe_tile_elevTexelCoeff.x * oe_tile_elevationTexMatrix[0][0] // scale
       + oe_tile_elevTexelCoeff.x * oe_tile_elevationTexMatrix[3].st // bias
       + oe_tile_elevTexelCoeff.y;
    return texture(oe_tile_elevationTex, elevc).r;
}

void main()
{
    const uint x = gl_GlobalInvocationID.x;
    const uint y = gl_GlobalInvocationID.y;

    vec2 offset = vec2(float(x), float(y));
    vec2 halfSpacing = 0.5 / vec2(gl_NumWorkGroups.xy);
    vec2 tilec = halfSpacing + offset / vec2(gl_NumWorkGroups.xy);

    vec4 noise = textureLod(oe_GroundCover_noiseTex, tilec, 0);

    vec2 shift = vec2(fract(noise[1]*1.5), fract(noise[2]*1.5))*2.0-1.0;
    tilec += shift * halfSpacing;
    vec4 tilec4 = vec4(tilec, 0, 1);

#ifdef OE_GROUNDCOVER_COLOR_SAMPLER
    if (!isLegalColor(tilec))
         return;
#endif

    // sample the landcover data
    float landCoverCode = textureLod(OE_LANDCOVER_TEX, (OE_LANDCOVER_TEX_MATRIX*tilec4).st, 0).r;
    int biomeIndex = oe_GroundCover_getBiomeIndex(landCoverCode);
    if ( biomeIndex < 0 )
        return;

    // If we're using a mask texture, sample it now:
#ifdef OE_GROUNDCOVER_MASK_SAMPLER
    float mask = texture(OE_GROUNDCOVER_MASK_SAMPLER, (OE_GROUNDCOVER_MASK_MATRIX*tilec4).st).a;
    if ( mask > 0.0 )
        return;
#endif

    // look up biome:
    oe_GroundCover_Biome biome;
    oe_GroundCover_getBiome(biomeIndex, biome);

    // discard instances based on noise value threshold (coverage). If it passes,
    // scale the noise value back up to [0..1]
    if (noise[NOISE_SMOOTH] > biome.fill)
        return;

    noise[NOISE_SMOOTH] /= biome.fill;

    vec4 vertex_model = vec4(
        mix(oe_GroundCover_LL.xy, oe_GroundCover_UR.xy, tilec),
        getElevation(tilec), 1);

#if 0 // Cannot view-cull when we're only computing on demand!

    vec4 vertex_view = gl_ModelViewMatrix * vertex_model;

    if (!inRange(vertex_view))
        return;

    // Cannot frustum cull when we're only computing on demand!
    if (!inFrustum(vertex_view))
        return;
#endif

    // It's a keeper. Populate the render buffer.
    uint start = oe_GroundCover_tileNum * gl_NumWorkGroups.y * gl_NumWorkGroups.x;
    uint slot = start + atomicAdd(cmd[oe_GroundCover_tileNum].instanceCount, 1);

    render[slot].fillEdge = 1.0;
    const float xx = 0.5;
    if (noise[NOISE_SMOOTH] > xx)
        render[slot].fillEdge = 1.0-((noise[NOISE_SMOOTH]-xx)/(1.0-xx));

    render[slot].vertex = vertex_model;
    render[slot].tilec = tilec;

    // select a billboard at random
    float pickNoise = 1.0-noise[pickNoiseType];
    int objectIndex = biome.firstObjectIndex + int(floor(pickNoise * float(biome.numObjects)));
    objectIndex = min(objectIndex, biome.firstObjectIndex + biome.numObjects - 1);

    // Recover the object we randomly picked:
    oe_GroundCover_Object object;
    oe_GroundCover_getObject(objectIndex, object);

    // for now, assume type == BILLBOARD.
    // Find the billboard associated with the object:
    oe_GroundCover_Billboard billboard;
    oe_GroundCover_getBillboard(object.objectArrayIndex, billboard);

    render[slot].sideIndex = billboard.atlasIndexSide;
    render[slot].topIndex = billboard.atlasIndexTop;

    // a pseudo-random scale factor to the width and height of a billboard
    float sizeScale = billboard.sizeVariation * (noise[NOISE_RANDOM_2]*2.0-1.0);
    render[slot].width = billboard.width + billboard.width*sizeScale;
    render[slot].height = billboard.height + billboard.height*sizeScale;
}
