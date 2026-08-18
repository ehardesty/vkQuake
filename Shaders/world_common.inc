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
	const vec2 emissive_uv_scale = vec2 (LMBLOCK_WIDTH, LMBLOCK_HEIGHT) / vec2 (textureSize (emissive_coarse_tex, 0));
	vec3	   emissive_selected;
#ifdef EMISSIVE_DETAIL
	if (emissive_detail_enabled)
	{
		const ivec2 emissive_detail_size = textureSize (emissive_detail_tex, 0);
		const ivec2 emissive_detail_coords =
			clamp (ivec2 (lightmap_uv * vec2 (LMBLOCK_WIDTH * 2, LMBLOCK_HEIGHT * 2)), ivec2 (0), emissive_detail_size - ivec2 (1));
		const vec4 emissive_detail = texelFetch (emissive_detail_tex, emissive_detail_coords, 0);
		if (emissive_debug_mode == 2)
			return vec4 (emissive_detail.rgb, 1.0);
		if (emissive_debug_mode == 3)
			return vec4 (vec3 (emissive_detail.a), 1.0);
		if (emissive_detail.a >= 0.5)
			emissive_selected = emissive_detail.rgb;
		else
			emissive_selected = texture (emissive_coarse_tex, lightmap_uv * emissive_uv_scale).rgb;
	}
	else
#endif
	{
		emissive_selected = texture (emissive_coarse_tex, lightmap_uv * emissive_uv_scale).rgb;
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

	float fog = exp (-push_constants.fog_density * push_constants.fog_density * in_fog_frag_coord * in_fog_frag_coord);
	fog = clamp (fog, 0.0, 1.0);
	frag_color.rgb = mix (push_constants.fog_color, frag_color.rgb, fog);

	if (use_alpha_blend)
		frag_color.a = push_constants.alpha;

	return frag_color;
}
