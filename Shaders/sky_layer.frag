#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_GOOGLE_include_directive : enable

layout (push_constant) uniform PushConsts
{
	mat4  mvp;
	vec3  fog_color;
	float fog_density;
	vec3  eye_pos;
	float time;
	float alpha;
	vec4  volume_viewport;
}
push_constants;

#ifdef EMISSIVE_VOLUME
#define VOLUME_SAMPLER_SET 2
#define VOLUME_SCATTER_ID 0
#include "sky_volume.inc"
#endif

layout (set = 0, binding = 0) uniform sampler2D solid_tex;
layout (set = 1, binding = 0) uniform sampler2D alpha_tex;

layout (location = 0) in vec4 in_texcoord;

layout (location = 0) out vec4 out_frag_color;

void main ()
{
	vec2 uv = normalize (in_texcoord).xy * (189.0 / 64.0);

	vec4 solid_layer = texture (solid_tex, uv + push_constants.time / 16.0f);
	vec4 alpha_layer = texture (alpha_tex, uv + push_constants.time / 8.0f);

	alpha_layer.a *= push_constants.alpha;
	out_frag_color = vec4 ((solid_layer.rgb * (1.0f - alpha_layer.a) + alpha_layer.rgb * alpha_layer.a), 1.0f);

	if (push_constants.fog_density > 0.0f)
		out_frag_color.rgb = (out_frag_color.rgb * (1.0f - push_constants.fog_density)) + (push_constants.fog_color * push_constants.fog_density);

#ifdef EMISSIVE_VOLUME
	// Accumulated foreground air scattering at the far extent, after ordinary
	// fog. Sky-mask-only passes never include this file.
	{
		const vec3 volume_scattering = SkyVolumeFarScattering (gl_FragCoord.xy, push_constants.volume_viewport);
		if (volume_scatter_only)
			out_frag_color.rgb = volume_scattering;
		else
			out_frag_color.rgb += volume_scattering;
	}
#endif
}
