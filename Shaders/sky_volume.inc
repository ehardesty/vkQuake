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

// RVQ1 selector shared with emissive_volume.inc: 1 filters the final
// cumulative plane bilinearly at the fragment's own UV; 0 preserves the
// pre-phase nearest-column fetch. See that file for the comparison policy.
#ifndef VOLUME_SPATIAL_RECONSTRUCTION
#define VOLUME_SPATIAL_RECONSTRUCTION 1
#endif

#if VOLUME_SPATIAL_RECONSTRUCTION
vec3 SkyVolumeFarScattering (const vec2 frag_xy, const vec4 viewport)
{
	const ivec3 volume_size = textureSize (sky_volume_tex, 0);
	if (volume_size.x <= 0 || volume_size.y <= 0 || volume_size.z <= 0)
		return vec3 (0.0f);
	if (isinf (viewport.x) || isnan (viewport.x) || isinf (viewport.y) || isnan (viewport.y) || isinf (viewport.z) ||
		isnan (viewport.z) || isinf (viewport.w) || isnan (viewport.w))
		return vec3 (0.0f);
	if (viewport.z <= 0.0f || viewport.w <= 0.0f)
		return vec3 (0.0f);
	if (isinf (frag_xy.x) || isnan (frag_xy.x) || isinf (frag_xy.y) || isnan (frag_xy.y))
		return vec3 (0.0f);

	// Actual fragment UV clamped to the half-texel inset, matching the
	// scene helper. The depth coordinate is the exact center of the last
	// stored plane: (N + 0.5) / B == (B - 0.5) / B.
	vec2 uv = (frag_xy - viewport.xy) / viewport.zw;
	const vec2 half_texel = vec2 (0.5f) / vec2 (float (volume_size.x), float (volume_size.y));
	uv = clamp (uv, half_texel, vec2 (1.0f) - half_texel);

	const float last_z = (float (volume_size.z) - 0.5f) / float (volume_size.z);
	return textureLod (sky_volume_tex, vec3 (uv, last_z), 0.0f).rgb;
}
#else
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

#endif
