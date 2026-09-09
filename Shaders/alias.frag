#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_GOOGLE_include_directive : enable

// Compiled in multiple variants: WBOIT writes weighted blended OIT accumulation,
// MBOIT writes power moments, MBOIT + MBOIT_COMPOSITE reconstructs transmittance
// from them and MSAA reads multisampled moment buffers

layout (push_constant) uniform PushConsts
{
	mat4  mvp;
	vec3  fog_color;
	float fog_density;
	// RT-emissive volume sampling rect (framebuffer px, y down) and forward-depth extent.
	vec4  volume_viewport;
	float volume_z_max;
}
push_constants;

layout (set = 0, binding = 0) uniform sampler2D diffuse_tex;
layout (set = 1, binding = 0) uniform sampler2D fullbright_tex;

struct EmissiveClusteredLight
{
	vec4 direction;
	vec4 color;
};

layout (set = 2, binding = 0) uniform UBO
{
	mat4  model_matrix;
	vec3  shade_vector;
	float blend_factor;
	vec3  light_color;
	float entalpha;
	uint  flags;
	uint  num_emissive_lights;
	EmissiveClusteredLight emissive_lights[4];
}
ubo;

layout (location = 0) in vec2 in_texcoord;
layout (location = 1) in vec4 in_color;
layout (location = 2) in float in_fog_frag_coord;

#ifndef ALIAS_ALPHA_TEST
#define ALIAS_ALPHA_TEST 0
#endif
#ifdef EMISSIVE_VOLUME
// Alias volume pipelines bind the volume texture at set 4; MD5 volume
// pipelines reuse this source with -DVOLUME_SAMPLER_SET=5 (their set 4 is
// the MBOIT input). Scatter selection is specialization id 0 (alias.frag
// otherwise uses none).
#ifndef VOLUME_SAMPLER_SET
#define VOLUME_SAMPLER_SET 4
#endif
#define VOLUME_SCATTER_ID 0
#include "emissive_volume.inc"
#endif
#include "alias_common.inc"

#if MBOIT
#ifndef MBOIT_INPUT_SET
#define MBOIT_INPUT_SET 3
#endif
#include "mboit.inc"
#elif WBOIT
#include "wboit.inc"
#else
layout (location = 0) out vec4 out_frag_color;
#endif

void main ()
{
#if MBOIT
	MBOITWrite (AliasFragmentColor ());
#elif WBOIT
	WBOITWrite (AliasFragmentColor ());
#else
	out_frag_color = AliasFragmentColor ();
#endif
}
