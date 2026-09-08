const uint EMISSIVE_BANDLIMIT_SAMPLES = 4u;
const uint EMISSIVE_BANDLIMIT_VERSION = 2u;

uint EmissiveBandlimitHash (uint value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	return value ^ (value >> 16);
}

bool EmissiveBandlimitPointOnSurface (const surface_t surf, const vec3 world_pos, const vec3 normal)
{
	const uint first_vertex = surf.vbo_offset;
	const uint num_vertices = surf.packed_tex_edgecount >> 16;
	float winding = 0.0f;
	for (uint vertex = 0u; vertex < num_vertices; ++vertex)
	{
		const world_vert_t first = world_verts[first_vertex + vertex];
		const world_vert_t second = world_verts[first_vertex + (vertex + 1u) % num_vertices];
		const vec3 first_pos = vec3 (first.x, first.y, first.z);
		const vec3 edge = vec3 (second.x, second.y, second.z) - first_pos;
		const float side = dot (cross (edge, world_pos - first_pos), normal);
		const float epsilon = 0.01f * length (edge);
		if (abs (side) <= epsilon)
			continue;
		if (winding == 0.0f)
			winding = sign (side);
		else if (side * winding < -epsilon)
			return false;
	}
	return true;
}

/*
 * The sample key must survive source-list compaction: transient light array
 * positions shift when unrelated sources are added or removed, so hash the
 * source's stable transport identity (origin and radius) instead of its
 * packed index. Radiance terms stay out of the key; two sources sharing an
 * exact origin and radius share a pattern.
 */
vec2 EmissiveBandlimitOffset (
	const uint surface_index, const float center_s, const float center_t, const vec3 light_origin, const float light_radius,
	const uint sample_index)
{
	uint key = EmissiveBandlimitHash (surface_index ^ (EMISSIVE_BANDLIMIT_VERSION * 0x9e3779b9u));
	key = EmissiveBandlimitHash (key ^ floatBitsToUint (center_s));
	key = EmissiveBandlimitHash (key ^ floatBitsToUint (center_t));
	key = EmissiveBandlimitHash (key ^ floatBitsToUint (light_origin.x));
	key = EmissiveBandlimitHash (key ^ floatBitsToUint (light_origin.y));
	key = EmissiveBandlimitHash (key ^ floatBitsToUint (light_origin.z));
	key = EmissiveBandlimitHash (key ^ floatBitsToUint (light_radius));
	key = EmissiveBandlimitHash (key ^ sample_index);
	const vec2 jitter = vec2 (float (key & 0xFFFFu), float (key >> 16)) / 65536.0f;
	const vec2 stratum = vec2 (float (sample_index & 1u), float (sample_index >> 1u));
	const float half_extent = 0.5f / float (max (push_constants.coordinate_scale, 1u));
	return (stratum + jitter) * half_extent - half_extent;
}

vec3 CalculateBandlimitedEmissiveContribution (
	const uint surface_index, const surface_t surf, const float center_s, const float center_t, const vec3 normal, const uint first_source,
	const uint num_lights, const uint visibility_texel)
{
	vec3 contribution = vec3 (0.0f);
	for (uint i = 0u; i < num_lights; ++i)
	{
#ifdef EMISSIVE_SOURCE_INDICES
		const uint light_index = emissive_source_indices[first_source + i];
#elif defined(EMISSIVE_OPTIONAL_SOURCE_INDICES)
		const uint light_index = indexed_sources ? emissive_source_indices[first_source + i] : first_source + i;
#else
		const uint light_index = i;
#endif
		const emissive_light_t light = emissive_lights[light_index];
		const float modulation = modulated_sources ? emissive_modulations[light_index] : 1.0f;
		if (modulation <= 0.0f && !cache_visibility)
			continue;

		uint visibility_mask = 0u;
#ifdef EMISSIVE_RETAINED_VISIBILITY
		if (retained_visibility)
			visibility_mask = emissive_visibility[
				(first_source + i) * 64u * push_constants.coordinate_scale * push_constants.coordinate_scale + visibility_texel];
#endif
		float visible_weight = 0.0f;
		float total_weight = 0.0f;
		for (uint sample_index = 0u; sample_index < EMISSIVE_BANDLIMIT_SAMPLES; ++sample_index)
		{
			vec3 tangent;
			vec3 bitangent;
			const vec2 offset = EmissiveBandlimitOffset (surface_index, center_s, center_t, light.origin, light.radius, sample_index);
			const vec3 world_pos = CalculateWorldPos (surf, center_s + offset.x, center_t + offset.y, tangent, bitangent);
			if (!EmissiveBandlimitPointOnSurface (surf, world_pos, normal))
				continue;
			const vec3 light_vec = light.origin - world_pos;
			const float dist = length (light_vec);
			if (dist >= light.radius)
				continue;
			const vec3 direction = light_vec / max (dist, 0.001f);
			const float receiver_lambert = max (dot (normal, direction), 0.0f);
			if (receiver_lambert <= 0.0f)
				continue;
			const float radial_falloff = max (1.0f - dist / light.radius, 0.0f);
			const float sample_weight = radial_falloff * receiver_lambert;
			total_weight += sample_weight;
#ifdef EMISSIVE_DETAIL_VISIBILITY
			const bool visible = !EmissiveBandlimitIsOccluded (world_pos, normal, light.origin);
			if (visible)
				visibility_mask |= 1u << sample_index;
			if (!visible)
				continue;
#elif defined(EMISSIVE_RETAINED_VISIBILITY)
			if (retained_visibility && (visibility_mask & (1u << sample_index)) == 0u)
				continue;
#endif
			visible_weight += sample_weight;
		}
#ifdef EMISSIVE_DETAIL_VISIBILITY
		if (cache_visibility)
			emissive_visibility[
				(first_source + i) * 64u * push_constants.coordinate_scale * push_constants.coordinate_scale + visibility_texel] = visibility_mask;
#endif
		vec3 center_tangent;
		vec3 center_bitangent;
		const vec3 center_world_pos = CalculateWorldPos (surf, center_s, center_t, center_tangent, center_bitangent);
		const vec3 center_light_vec = light.origin - center_world_pos;
		const float center_dist = length (center_light_vec);
		const vec3 center_direction = center_light_vec / max (center_dist, 0.001f);
		const float center_lambert = max (dot (normal, center_direction), 0.0f);
		const float center_falloff = max (1.0f - center_dist / light.radius, 0.0f);
		const float coverage = total_weight > 0.0f ? clamp (visible_weight / total_weight, 0.0f, 1.0f) : 0.0f;
		const vec3 unshadowed_center =
			center_dist < light.radius ? light.intensity * center_falloff * center_lambert * light.color : vec3 (0.0f);
		const vec3 emitter_contribution = modulation * unshadowed_center * coverage;
		contribution += emitter_contribution;
	}
	return max (contribution, 0.0f);
}
