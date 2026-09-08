#ifdef EMISSIVE_BANDLIMIT
vec4 SampleBandlimitedEmissiveDetail (const vec2 emissive_texel, const vec2 detail_uv)
{
	const ivec2 detail_size = textureSize (emissive_detail_tex, 0);
	const ivec2 surface_indices_size = textureSize (emissive_surface_indices_tex, 0);
	const int detail_scale = max (detail_size.x / surface_indices_size.x, 1);
	// Receiver and tap coordinates share the cropped emissive-atlas space: emissive_texel is
	// already offset-corrected (lightmap_uv * LMBLOCK - atlas_offset), so flooring it addresses
	// the same surface-index texel the coarse/detail builders wrote. Deriving the receiver from
	// lightmap_uv * surface_indices_size instead would rescale into the wrong chart whenever the
	// cropped atlas is smaller than the full lightmap block.
	const ivec2 receiver_coords = clamp (ivec2 (floor (emissive_texel)), ivec2 (0), surface_indices_size - ivec2 (1));
	const uint receiver_surface = texelFetch (emissive_surface_indices_tex, receiver_coords, 0).x;
	const vec2 detail_texel = detail_uv * vec2 (detail_size) - 0.5f;
	const ivec2 first_detail_coords = ivec2 (floor (detail_texel));
	const vec2 fraction = fract (detail_texel);

	vec3 weighted_rgb = vec3 (0.0f);
	vec3 minimum_supported_rgb = vec3 (0.0f);
	float supported_weight = 0.0f;
	bool have_supported_tap = false;
	for (int y = 0; y < 2; ++y)
	{
		for (int x = 0; x < 2; ++x)
		{
			const ivec2 detail_coords = clamp (first_detail_coords + ivec2 (x, y), ivec2 (0), detail_size - ivec2 (1));
			const ivec2 surface_coords = clamp (detail_coords / detail_scale, ivec2 (0), surface_indices_size - ivec2 (1));
			if (texelFetch (emissive_surface_indices_tex, surface_coords, 0).x != receiver_surface)
				continue;

			const vec2 axis_weight = mix (vec2 (1.0f) - fraction, fraction, vec2 (x, y));
			const float weight = axis_weight.x * axis_weight.y;
			const vec4 detail = texelFetch (emissive_detail_tex, detail_coords, 0);
			const float supported_tap_weight = detail.a * weight;
			weighted_rgb += detail.rgb * supported_tap_weight;
			supported_weight += supported_tap_weight;
			if (supported_tap_weight > 0.0f)
			{
				minimum_supported_rgb = have_supported_tap ? min (minimum_supported_rgb, detail.rgb) : detail.rgb;
				have_supported_tap = true;
			}
		}
	}

	const bool supported = supported_weight > 0.0f;
	if (emissive_debug_mode == 9)
		return vec4 (have_supported_tap ? minimum_supported_rgb : vec3 (0.0f), supported_weight);
	return vec4 (supported ? weighted_rgb / supported_weight : vec3 (0.0f), supported_weight);
}
#endif

vec4 WorldFragmentColor ()
{
	vec4 diffuse = texture (diffuse_tex, in_texcoords.xy);
	if (use_alpha_test && diffuse.a < 0.666f)
		discard;

	float lm_multiplier = scaled_lm ? 8.0f : 2.0f;
	vec2  lightmap_uv = in_texcoords.zw;

	if (quantize_lm)
	{
		ivec2 lm_size = ivec2 (LMBLOCK_WIDTH, LMBLOCK_HEIGHT);
		lightmap_uv = (floor ((lm_size * 16) * lightmap_uv) + 0.5) / (lm_size * 16);
	}
	vec3 light = texture (lightmap_tex, lightmap_uv).rgb * lm_multiplier;
#ifdef EMISSIVE_COARSE
	const vec2 emissive_texel = lightmap_uv * vec2 (LMBLOCK_WIDTH, LMBLOCK_HEIGHT) - vec2 (push_constants.emissive_atlas_offset);
	const vec2 emissive_uv = emissive_texel / vec2 (textureSize (emissive_coarse_tex, 0));
	vec3	   emissive_selected;
	if (emissive_debug_mode == 5)
		return vec4 (texture (emissive_coarse_tex, emissive_uv).rgb, 1.0);
#ifdef EMISSIVE_DETAIL
	if (emissive_detail_enabled)
	{
		const ivec2 emissive_detail_size = textureSize (emissive_detail_tex, 0);
		const int detail_scale = max (emissive_detail_size.x / textureSize (emissive_coarse_tex, 0).x, 1);
		const ivec2 emissive_detail_coords =
			clamp (ivec2 (emissive_texel * float (detail_scale)), ivec2 (0), emissive_detail_size - ivec2 (1));
		const vec4 stored_emissive_detail = texelFetch (emissive_detail_tex, emissive_detail_coords, 0);
		if (emissive_debug_mode == 8)
			return vec4 (stored_emissive_detail.rgb, 1.0f);
		vec4 emissive_detail = stored_emissive_detail;
#ifdef EMISSIVE_BANDLIMIT
		if (emissive_bandlimit_enabled)
			emissive_detail = SampleBandlimitedEmissiveDetail (emissive_texel, emissive_uv);
#endif
		if (emissive_debug_mode == 2 || emissive_debug_mode == 6)
			return vec4 (emissive_detail.rgb, 1.0);
		if (emissive_debug_mode == 3 || emissive_debug_mode == 7)
			return vec4 (vec3 (emissive_detail.a), 1.0);
		// Classic detail validity is binary and retains its established threshold.
		// The band-limited reconstruction reports its actual supported-tap coverage.
#ifdef EMISSIVE_BANDLIMIT
		if (emissive_detail.a > 0.0f)
#else
		if (emissive_detail.a >= 0.5f)
#endif
			emissive_selected = emissive_detail.rgb;
		else
			emissive_selected = texture (emissive_coarse_tex, emissive_uv).rgb;
	}
	else
#endif
	{
		emissive_selected = texture (emissive_coarse_tex, emissive_uv).rgb;
		if (emissive_debug_mode == 1)
			return vec4 (emissive_selected, 1.0);
	}
#ifdef EMISSIVE_DETAIL
	if (emissive_debug_mode == 4)
		return vec4 (emissive_selected, 1.0);
#endif
	light += emissive_selected;
#endif

	vec4 frag_color;
	frag_color.rgb = diffuse.rgb * light.rgb;

	if (use_fullbright)
	{
		vec3 fullbright = texture (fullbright_tex, in_texcoords.xy).rgb;
		frag_color.rgb += fullbright;
	}
#ifdef EMISSIVE_ADD
	frag_color.rgb += diffuse.rgb * push_constants.emissive_add;
#endif

	float fog = exp (-push_constants.fog_density * push_constants.fog_density * in_fog_frag_coord * in_fog_frag_coord);
	fog = clamp (fog, 0.0, 1.0);
	frag_color.rgb = mix (push_constants.fog_color, frag_color.rgb, fog);

	if (use_alpha_blend)
		frag_color.a = push_constants.alpha;

	return frag_color;
}
