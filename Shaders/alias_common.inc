#ifndef ALIAS_ALPHA_TEST
#define ALIAS_ALPHA_TEST 0
#endif

vec4 AliasFragmentColor ()
{
	vec4 result = texture (diffuse_tex, in_texcoord.xy);

#if ALIAS_ALPHA_TEST
	if (result.a < 0.666f)
		discard;
#endif

	float original_diffuse_tex_a = result.a;
	result.a = 1.0;

	result *= in_color * 2.0f;

	if ((ubo.flags & 0x1) != 0)
		result += texture (fullbright_tex, in_texcoord.xy);

	result.a = original_diffuse_tex_a * ubo.entalpha;

	float fog = exp (-push_constants.fog_density * push_constants.fog_density * in_fog_frag_coord * in_fog_frag_coord);
	fog = clamp (fog, 0.0, 1.0);
	result.rgb = mix (push_constants.fog_color, result.rgb, fog);

#if defined(EMISSIVE_VOLUME) && !defined(WBOIT) && !defined(MBOIT)
	// Foreground air scattering at this fragment's own depth, after ordinary
	// fog. Alpha-tested fragments were already discarded above, so cutout
	// coverage is unchanged. MBOIT moment-only and WBOIT modules never take
	// this path (see RV6C).
	{
		const vec3 volume_scattering = EmissiveVolumeSample (
			gl_FragCoord.xy, in_fog_frag_coord, push_constants.volume_viewport, push_constants.volume_z_max);
		if (volume_scatter_only)
			result.rgb = volume_scattering;
		else
			result.rgb += volume_scattering;
	}
#endif

	return result;
}
