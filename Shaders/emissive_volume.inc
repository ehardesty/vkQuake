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

// RVQ1 spatial reconstruction selector (test-build definition, not a
// pipeline axis). 1 is the corrected filtered reconstruction below; 0
// preserves the pre-phase nearest-column helper byte-for-byte for
// reference/candidate comparisons. Reference builds pass
// -DVOLUME_SPATIAL_RECONSTRUCTION=0 with identical inputs. The reference
// uses texelFetch and ignores sampler filtering; the candidate requires
// the volume module's clamp-to-edge linear sampler.
#ifndef VOLUME_SPATIAL_RECONSTRUCTION
#define VOLUME_SPATIAL_RECONSTRUCTION 1
#endif

#if VOLUME_SPATIAL_RECONSTRUCTION
vec3 EmissiveVolumeSample (const vec2 frag_xy, const float fwd_depth, const vec4 viewport, const float z_max)
{
	const ivec3 volume_size = textureSize (emissive_volume_tex, 0);
	if (volume_size.x <= 0 || volume_size.y <= 0 || volume_size.z <= 1)
		return vec3 (0.0f);
	// B is the stored plane count, N the segment count. No hidden
	// assumption that B is 65: every depth below derives from these.
	const int segments = volume_size.z - 1;
	const int boundaries = volume_size.z;
	if (segments < 1 || isinf (z_max) || isnan (z_max) || z_max <= 0.0f)
		return vec3 (0.0f);
	if (isinf (fwd_depth) || isnan (fwd_depth) || fwd_depth <= 0.0f)
		return vec3 (0.0f);
	if (isinf (viewport.x) || isnan (viewport.x) || isinf (viewport.y) || isnan (viewport.y) || isinf (viewport.z) || isnan (viewport.z) ||
		isinf (viewport.w) || isnan (viewport.w))
		return vec3 (0.0f);
	if (viewport.z <= 0.0f || viewport.w <= 0.0f)
		return vec3 (0.0f);

	// Actual fragment UV, clamped to the half-texel inset so the filtered
	// lookup never wraps or bleeds the opposite edge. Column centers
	// reproduce exactly; intermediate positions are continuous bilinear
	// combinations evaluated at this fragment's own forward depth.
	vec2 uv = (frag_xy - viewport.xy) / viewport.zw;
	const vec2 half_texel = vec2 (0.5f) / vec2 (float (volume_size.x), float (volume_size.y));
	uv = clamp (uv, half_texel, vec2 (1.0f) - half_texel);

	if (fwd_depth >= z_max)
	{
		const float last_z = (float (segments) + 0.5f) / float (boundaries);
		return textureLod (emissive_volume_tex, vec3 (uv, last_z), 0.0f).rgb;
	}

	// Preserve the nonlinear depth convention: interpolate by position
	// between the actual quadratic boundary depths, then map the
	// i+w plane address back to normalized texel-center coordinates.
	// Passing sqrt(z/Z) directly to the sampler would interpolate in
	// slice-parameter space and introduce depth bias. The denominator
	// is the plane count B (N+1), not N.
	const float s = sqrt (clamp (fwd_depth / z_max, 0.0f, 1.0f)) * float (segments);
	const int	i0 = clamp (int (floor (s)), 0, segments - 1);
	const float t0 = float (i0) / float (segments);
	const float t1 = float (i0 + 1) / float (segments);
	const float z0 = z_max * t0 * t0;
	const float z1 = z_max * t1 * t1;
	const float w = clamp ((fwd_depth - z0) / max (z1 - z0, 1e-6f), 0.0f, 1.0f);
	const float texture_z = (float (i0) + w + 0.5f) / float (boundaries);
	return textureLod (emissive_volume_tex, vec3 (uv, texture_z), 0.0f).rgb;
}
#else
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

#endif
