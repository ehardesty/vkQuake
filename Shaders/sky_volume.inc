// sky_volume.inc — far-extent air-scattering sampling for sky color draws.
//
// Sky fragments have no meaningful rasterized depth, so they sample the
// accumulated foreground contribution (last volume boundary) in their screen
// column. Sky-mask-only passes never include this file. Including shaders
// define VOLUME_SAMPLER_SET before this include and append a vec4
// volume_viewport (framebuffer px, y down) to their push constants.

#ifdef EMISSIVE_VOLUME

layout (set = VOLUME_SAMPLER_SET, binding = 0) uniform sampler3D sky_volume_tex;
layout (constant_id = VOLUME_SCATTER_ID) const bool volume_scatter_only = false;

vec3 SkyVolumeFarScattering (const vec2 frag_xy, const vec4 viewport)
{
	const ivec3 volume_size = textureSize (sky_volume_tex, 0);
	if (volume_size.z <= 0)
		return vec3 (0.0f);
	const vec2	uv = (frag_xy - viewport.xy) / viewport.zw;
	const ivec2 column = ivec2 (clamp (floor (uv * vec2 (volume_size.xy)), vec2 (0.0f), vec2 (volume_size.xy) - vec2 (1.0f)));
	return texelFetch (sky_volume_tex, ivec3 (column, volume_size.z - 1), 0).rgb;
}

#endif
