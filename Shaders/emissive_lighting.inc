vec3 CalculateEmissiveContribution (
	const vec3 world_pos, const vec3 normal, const uint first_source, const uint num_lights, const uint visibility_texel)
{
	vec3 contribution = vec3 (0.0f);
	for (uint i = 0; i < num_lights; ++i)
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
		const vec3			   light_vec = light.origin - world_pos;
		const float			   dist = length (light_vec);
		if (dist >= light.radius)
			continue;
		const vec3  direction = light_vec / max (dist, 0.001f);
		const float receiver_lambert = max (dot (normal, direction), 0.0f);
		if (receiver_lambert <= 0.0f)
			continue;
		const float radial_falloff = max (1.0f - dist / light.radius, 0.0f);
#ifdef EMISSIVE_DETAIL_VISIBILITY
		const bool visible = !EmissiveIsOccluded (world_pos, normal, light.origin);
		if (cache_visibility)
			emissive_visibility[(first_source + i) * 64u * push_constants.coordinate_scale * push_constants.coordinate_scale + visibility_texel] =
				visible ? 1u : 0u;
		if (!visible)
			continue;
#elif defined(EMISSIVE_RETAINED_VISIBILITY)
		if (retained_visibility &&
			emissive_visibility[(first_source + i) * 64u * push_constants.coordinate_scale * push_constants.coordinate_scale + visibility_texel] == 0u)
			continue;
#endif
		contribution += modulation * light.intensity * radial_falloff * receiver_lambert * light.color;
	}
	return max (contribution, 0.0f);
}
