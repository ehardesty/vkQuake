#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_GOOGLE_include_directive : enable

layout (push_constant) uniform PushConsts
{
	mat4  mvp;
	vec3  fog_color;
	float fog_density;
	vec4  volume_viewport;
}
push_constants;

#ifdef EMISSIVE_VOLUME
#define VOLUME_SAMPLER_SET 1
#define VOLUME_SCATTER_ID 0
#include "sky_volume.inc"
#endif

layout (set = 0, binding = 0) uniform sampler2D tex;

layout (location = 0) in vec4 in_texcoord;
layout (location = 1) in vec4 in_color;
layout (location = 2) in float in_fog_frag_coord;

layout (location = 0) out vec4 out_frag_color;

void main ()
{
	out_frag_color = in_color * texture (tex, in_texcoord.xy);
	if (push_constants.fog_density > 0.0f)
		out_frag_color.rgb = (out_frag_color.rgb * (1.0f - push_constants.fog_density)) + (push_constants.fog_color * push_constants.fog_density);

#ifdef EMISSIVE_VOLUME
	{
		const vec3 volume_scattering = SkyVolumeFarScattering (gl_FragCoord.xy, push_constants.volume_viewport);
		if (volume_scatter_only)
			out_frag_color.rgb = volume_scattering;
		else
			out_frag_color.rgb += volume_scattering;
	}
#endif
}
