// emissive_volume.inc — fragment sampling and additive composition for the
// RT-emissive air-scattering volume.
//
// Included by scene fragment shaders that participate as air receivers.
// Sampling is gated on the drawing pipeline, never on map fog: a fog-free
// fragment still receives volumetrics. The volume stores cumulative
// scattering at depth boundaries (layer 0 is zero at the camera plane);
// sampling interpolates by actual endpoint depths so a constant
// unattenuated integrand reproduces along depth within float tolerance.
// Fragments beyond the represented extent retain the last value.

// Moment-only MBOIT passes never sample the volume; WBOIT accumulation and
// MBOIT composite (color-producing stages) do, following each path's
// existing weighting/compositing convention. The final OIT resolves never
// reference the volume, so nothing is added twice.
#if defined(EMISSIVE_VOLUME) && !(defined(MBOIT) && !defined(MBOIT_COMPOSITE))

// Including shaders define VOLUME_SAMPLER_SET (descriptor set holding the
// volume texture) and VOLUME_SCATTER_ID (scatter-only specialization id)
// before this include. World reuses its layout's set 7 slot, unbound in
// every non-bandlimit variant; alias/MD5/sky families use their own
// volume-layout sets. Bandlimit + volume stays explicitly unsupported.
layout (set = VOLUME_SAMPLER_SET, binding = 0) uniform sampler3D emissive_volume_tex;
layout (constant_id = VOLUME_SCATTER_ID) const bool volume_scatter_only = false;

vec3 EmissiveVolumeSample (const vec2 frag_xy, const float fwd_depth, const vec4 viewport, const float z_max)
{
	const ivec3 volume_size = textureSize (emissive_volume_tex, 0);
	const vec2	grid_xy = vec2 (volume_size.xy);
	const int	segments = volume_size.z - 1;
	if (segments <= 0 || z_max <= 0.0f)
		return vec3 (0.0f);
	if (fwd_depth <= 0.0f)
		return vec3 (0.0f);

	const vec2 uv = (frag_xy - viewport.xy) / viewport.zw;
	const ivec2 column = ivec2 (clamp (floor (uv * grid_xy), vec2 (0.0f), grid_xy - vec2 (1.0f)));

	if (fwd_depth >= z_max)
		return texelFetch (emissive_volume_tex, ivec3 (column, segments), 0).rgb;

	const float f = sqrt (fwd_depth / z_max) * float (segments);
	const int	i0 = clamp (int (floor (f)), 0, segments - 1);
	const vec3	s0 = texelFetch (emissive_volume_tex, ivec3 (column, i0), 0).rgb;
	const vec3	s1 = texelFetch (emissive_volume_tex, ivec3 (column, i0 + 1), 0).rgb;
	const float t0 = float (i0) / float (segments);
	const float t1 = float (i0 + 1) / float (segments);
	const float z0 = z_max * t0 * t0;
	const float z1 = z_max * t1 * t1;
	const float w = (fwd_depth - z0) / max (z1 - z0, 1e-6f);
	return mix (s0, s1, clamp (w, 0.0f, 1.0f));
}

#endif
