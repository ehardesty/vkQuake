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

vec2 EmissiveBandlimitOffset (
	const uint surface_index, const float center_s, const float center_t, const uint light_index, const uint sample_index)
{
	uint key = EmissiveBandlimitHash (surface_index ^ (EMISSIVE_BANDLIMIT_VERSION * 0x9e3779b9u));
	key = EmissiveBandlimitHash (key ^ floatBitsToUint (center_s));
	key = EmissiveBandlimitHash (key ^ floatBitsToUint (center_t));
	key = EmissiveBandlimitHash (key ^ light_index);
	key = EmissiveBandlimitHash (key ^ sample_index);
	const vec2 jitter = vec2 (float (key & 0xFFFFu), float (key >> 16)) / 65536.0f;
	const vec2 stratum = vec2 (float (sample_index & 1u), float (sample_index >> 1u));
	const float half_extent = 0.5f / float (max (push_constants.coordinate_scale, 1u));
	return (stratum + jitter) * half_extent - half_extent;
}

#ifdef EMISSIVE_BANDLIMIT_DIAGNOSTICS
vec2 EmissiveBandlimitReferenceOffset (
	const uint surface_index, const float center_s, const float center_t, const uint light_index, const uint sample_index,
	const uint grid_size)
{
	uint key = EmissiveBandlimitHash (surface_index ^ (EMISSIVE_BANDLIMIT_VERSION * 0x9e3779b9u));
	key = EmissiveBandlimitHash (key ^ floatBitsToUint (center_s));
	key = EmissiveBandlimitHash (key ^ floatBitsToUint (center_t));
	key = EmissiveBandlimitHash (key ^ light_index);
	key = EmissiveBandlimitHash (key ^ sample_index);
	const vec2 jitter = vec2 (float (key & 0xFFFFu), float (key >> 16)) / 65536.0f;
	const vec2 stratum = vec2 (float (sample_index % grid_size), float (sample_index / grid_size));
	const float half_extent = 0.5f / float (max (push_constants.coordinate_scale, 1u));
	return (stratum + jitter) * (2.0f * half_extent / float (grid_size)) - half_extent;
}

float EmissiveBandlimitReferenceCoverage (
	const uint surface_index, const surface_t surf, const float center_s, const float center_t, const vec3 normal,
	const uint light_index, const emissive_light_t light, const uint grid_size)
{
	float total_weight = 0.0f;
	float visible_weight = 0.0f;
	const uint sample_count = grid_size * grid_size;
	for (uint sample_index = 0u; sample_index < sample_count; ++sample_index)
	{
		vec3 tangent;
		vec3 bitangent;
		const vec2 offset =
			EmissiveBandlimitReferenceOffset (surface_index, center_s, center_t, light_index, sample_index, grid_size);
		const vec3 world_pos = CalculateWorldPos (surf, center_s + offset.x, center_t + offset.y, tangent, bitangent);
		if (!EmissiveBandlimitPointOnSurface (surf, world_pos, normal))
			continue;
		const vec3 light_vec = light.origin - world_pos;
		const float dist = length (light_vec);
		if (dist >= light.radius)
			continue;
		const float receiver_lambert = max (dot (normal, light_vec / max (dist, 0.001f)), 0.0f);
		if (receiver_lambert <= 0.0f)
			continue;
		const float sample_weight = max (1.0f - dist / light.radius, 0.0f) * receiver_lambert;
		total_weight += sample_weight;
		if (!EmissiveBandlimitIsOccluded (world_pos, normal, light.origin))
			visible_weight += sample_weight;
	}
	return total_weight > 0.0f ? clamp (visible_weight / total_weight, 0.0f, 1.0f) : 0.0f;
}
#endif

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
		uint retained_visibility_mask = 0u;
#ifdef EMISSIVE_RETAINED_VISIBILITY
		if (retained_visibility)
		{
			visibility_mask = emissive_visibility[
				(first_source + i) * 64u * push_constants.coordinate_scale * push_constants.coordinate_scale + visibility_texel];
			retained_visibility_mask = visibility_mask;
		}
#endif
		float visible_weight = 0.0f;
		float total_weight = 0.0f;
		float sum_squared_weights = 0.0f;
		uint on_surface_mask = 0u;
		uint in_radius_mask = 0u;
		uint positive_lambert_mask = 0u;
		float diagnostic_weights[4] = float[4] (0.0f, 0.0f, 0.0f, 0.0f);
		for (uint sample_index = 0u; sample_index < EMISSIVE_BANDLIMIT_SAMPLES; ++sample_index)
		{
			vec3 tangent;
			vec3 bitangent;
			const vec2 offset = EmissiveBandlimitOffset (surface_index, center_s, center_t, light_index, sample_index);
			const vec3 world_pos = CalculateWorldPos (surf, center_s + offset.x, center_t + offset.y, tangent, bitangent);
			if (!EmissiveBandlimitPointOnSurface (surf, world_pos, normal))
				continue;
			on_surface_mask |= 1u << sample_index;
			const vec3 light_vec = light.origin - world_pos;
			const float dist = length (light_vec);
			if (dist >= light.radius)
				continue;
			in_radius_mask |= 1u << sample_index;
			const vec3 direction = light_vec / max (dist, 0.001f);
			const float receiver_lambert = max (dot (normal, direction), 0.0f);
			if (receiver_lambert <= 0.0f)
				continue;
			positive_lambert_mask |= 1u << sample_index;
			const float radial_falloff = max (1.0f - dist / light.radius, 0.0f);
			const float sample_weight = radial_falloff * receiver_lambert;
			diagnostic_weights[sample_index] = sample_weight;
			total_weight += sample_weight;
			sum_squared_weights += sample_weight * sample_weight;
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
#ifdef EMISSIVE_BANDLIMIT_DIAGNOSTICS
		const bool diagnostic_target = !transient_overlay &&
			all (lessThanEqual (abs (emissive_bandlimit_diag_coords - ivec2 (817, 344)), ivec2 (2)));
		if (diagnostic_target)
		{
			const uint record = atomicAdd (emissive_bandlimit_diag[1], 1u);
			if (record < 512u)
			{
				const uint base = 16u + record * 40u;
				emissive_bandlimit_diag[base + 0u] = surface_index;
				emissive_bandlimit_diag[base + 1u] = uint (emissive_bandlimit_diag_coords.x);
				emissive_bandlimit_diag[base + 2u] = uint (emissive_bandlimit_diag_coords.y);
				emissive_bandlimit_diag[base + 3u] = emissive_bandlimit_diag_tile;
				emissive_bandlimit_diag[base + 4u] = i;
				emissive_bandlimit_diag[base + 5u] = light_index;
				emissive_bandlimit_diag[base + 6u] = first_source;
				emissive_bandlimit_diag[base + 7u] = num_lights;
				emissive_bandlimit_diag[base + 8u] = on_surface_mask;
				emissive_bandlimit_diag[base + 9u] = in_radius_mask;
				emissive_bandlimit_diag[base + 10u] = positive_lambert_mask;
				emissive_bandlimit_diag[base + 11u] = visibility_mask;
				emissive_bandlimit_diag[base + 12u] = retained_visibility_mask;
				emissive_bandlimit_diag[base + 13u] = 0u;
				emissive_bandlimit_diag[base + 14u] = bitCount (positive_lambert_mask);
				emissive_bandlimit_diag[base + 15u] = 0u;
				for (uint sample_index = 0u; sample_index < 4u; ++sample_index)
					emissive_bandlimit_diag[base + 16u + sample_index] = floatBitsToUint (diagnostic_weights[sample_index]);
				emissive_bandlimit_diag[base + 20u] = floatBitsToUint (total_weight);
				emissive_bandlimit_diag[base + 21u] = floatBitsToUint (visible_weight);
				emissive_bandlimit_diag[base + 22u] = floatBitsToUint (sum_squared_weights);
				emissive_bandlimit_diag[base + 23u] = floatBitsToUint (coverage);
				emissive_bandlimit_diag[base + 24u] = floatBitsToUint (center_dist);
				emissive_bandlimit_diag[base + 25u] = floatBitsToUint (center_falloff);
				emissive_bandlimit_diag[base + 26u] = floatBitsToUint (center_lambert);
				emissive_bandlimit_diag[base + 27u] = floatBitsToUint (modulation);
				emissive_bandlimit_diag[base + 28u] = floatBitsToUint (light.intensity);
				emissive_bandlimit_diag[base + 29u] = floatBitsToUint (float (bitCount (positive_lambert_mask)) / 4.0f);
				emissive_bandlimit_diag[base + 30u] = floatBitsToUint (
					sum_squared_weights > 0.0f ? total_weight * total_weight / sum_squared_weights : 0.0f);
				const uint center_hit = EmissiveBandlimitOcclusionPrimitive (center_world_pos, normal, light.origin);
				emissive_bandlimit_diag[base + 31u] = floatBitsToUint (center_hit == 0xFFFFFFFFu ? 1.0f : 0.0f);
				emissive_bandlimit_diag[base + 32u] = floatBitsToUint (EmissiveBandlimitReferenceCoverage (
					surface_index, surf, center_s, center_t, normal, light_index, light, 4u));
				emissive_bandlimit_diag[base + 33u] = floatBitsToUint (EmissiveBandlimitReferenceCoverage (
					surface_index, surf, center_s, center_t, normal, light_index, light, 8u));
				emissive_bandlimit_diag[base + 34u] = floatBitsToUint (dot (unshadowed_center, vec3 (0.2126f, 0.7152f, 0.0722f)));
				emissive_bandlimit_diag[base + 35u] = floatBitsToUint (dot (emitter_contribution, vec3 (0.2126f, 0.7152f, 0.0722f)));
				emissive_bandlimit_diag[base + 36u] = floatBitsToUint (emitter_contribution.r);
				emissive_bandlimit_diag[base + 37u] = floatBitsToUint (emitter_contribution.g);
				emissive_bandlimit_diag[base + 38u] = floatBitsToUint (emitter_contribution.b);
				emissive_bandlimit_diag[base + 39u] = center_hit;
				emissive_bandlimit_diag[3] = EMISSIVE_BANDLIMIT_VERSION;
			}
			else
				atomicAdd (emissive_bandlimit_diag[2], 1u);
		}
#endif
		contribution += emitter_contribution;
	}
	return max (contribution, 0.0f);
}
