#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_GOOGLE_include_directive : enable

// Compiled in multiple variants: WBOIT writes weighted blended OIT accumulation,
// MBOIT writes power moments, MBOIT + MBOIT_COMPOSITE reconstructs transmittance
// from them and MSAA reads multisampled moment buffers

// keep in sync with glquake.h
#define LMBLOCK_WIDTH  1024
#define LMBLOCK_HEIGHT 1024

layout (push_constant) uniform PushConsts
{
	mat4  mvp;
	vec3  fog_color;
	float fog_density;
	float alpha;
	uint  instance_base;
	ivec2 emissive_atlas_offset;
	vec3  emissive_add;
	// RT-emissive volume sampling rect (framebuffer px, y down) and forward-depth extent.
	// Appended after the established 27 floats so every existing push offset is unchanged.
	// The scalar precedes the vector: vec4 requires 16-byte alignment, so it
	// lives at byte 112 while z_max fills the padding slot at byte 108.
	// The host uploads z_max at float 27 and the rect at floats 28-31.
	float volume_z_max;
	vec4  volume_viewport;
}
push_constants;

layout (set = 0, binding = 0) uniform sampler2D diffuse_tex;
layout (set = 1, binding = 0) uniform sampler2D lightmap_tex;
layout (set = 2, binding = 0) uniform sampler2D fullbright_tex;
#ifdef EMISSIVE_COARSE
// Surface-emissive trio in world set 5 (coarse, detail, and — under
// bandlimit — the surface-index texture). One 3-binding set fits the
// frame-varying resolved triple where three fixed sets cannot; the
// frame-dependent volume lives alone in set 6 so bandlimit and
// volumetrics no longer share set 7.
layout (set = 5, binding = 0) uniform sampler2D emissive_coarse_tex;
#ifdef EMISSIVE_DETAIL
layout (set = 5, binding = 1) uniform sampler2D emissive_detail_tex;
#ifdef EMISSIVE_BANDLIMIT
layout (set = 5, binding = 2) uniform usampler2D emissive_surface_indices_tex;
#endif
#endif
#endif

layout (location = 0) in vec4 in_texcoords;
layout (location = 1) in float in_fog_frag_coord;

layout (constant_id = 0) const bool use_fullbright = false;
layout (constant_id = 1) const bool use_alpha_test = false;
layout (constant_id = 2) const bool use_alpha_blend = false;
layout (constant_id = 3) const bool quantize_lm = false;
layout (constant_id = 4) const bool scaled_lm = false;
#ifdef EMISSIVE_COARSE
layout (constant_id = 5) const uint emissive_debug_mode = 0;
#ifdef EMISSIVE_DETAIL
layout (constant_id = 6) const bool emissive_detail_enabled = false;
layout (constant_id = 7) const bool emissive_bandlimit_enabled = false;
#endif
#endif

#ifdef EMISSIVE_VOLUME
#define VOLUME_SAMPLER_SET 6
#define VOLUME_SCATTER_ID 8
#include "emissive_volume.inc"
#endif
#include "world_common.inc"

#if MBOIT
#define MBOIT_INPUT_SET 3
#include "mboit.inc"
#elif WBOIT
#include "wboit.inc"
#else
layout (location = 0) out vec4 out_frag_color;
#endif

void main ()
{
#if MBOIT
	MBOITWrite (WorldFragmentColor ());
#elif WBOIT
	WBOITWrite (WorldFragmentColor ());
#else
	out_frag_color = WorldFragmentColor ();
#endif
}
