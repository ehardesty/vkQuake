vec3 CalculateEmissiveContribution (const vec3 world_pos, const vec3 normal)
{
	vec3 contribution = vec3 (0.0f);
	for (uint i = 0; i < push_constants.num_lights; ++i)
	{
		const emissive_light_t light = emissive_lights[i];
		const vec3			   light_vec = light.origin - world_pos;
		const float			   dist = length (light_vec);
		if (dist >= light.radius)
			continue;
		const vec3  direction = light_vec / max (dist, 0.001f);
		const float receiver_lambert = max (dot (normal, direction), 0.0f);
		const float radial_falloff = max (1.0f - dist / light.radius, 0.0f);
		contribution += light.intensity * radial_falloff * receiver_lambert * light.color;
	}
	return max (contribution, 0.0f);
}
