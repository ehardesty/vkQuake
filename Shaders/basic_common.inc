vec4 BasicFragmentColor ()
{
	vec4 frag_color = in_color * texture (tex, in_texcoord.xy);
#if ALPHA_TEST
	if (frag_color.a < 0.666f)
		discard;
#endif
#if EMISSIVE_ADD
	frag_color.rgb += frag_color.rgb * max (push_constants.emissive_add.rgb, vec3 (0.0f));
#elif EMISSIVE_VERTEX
	frag_color.rgb += frag_color.rgb * max (in_emissive, vec3 (0.0f));
#endif

	float fog = exp (-push_constants.fog_density * push_constants.fog_density * in_fog_frag_coord * in_fog_frag_coord);
	fog = clamp (fog, 0.0, 1.0);
	frag_color.rgb = mix (push_constants.fog_color, frag_color.rgb, fog);

	return frag_color;
}
