vec3 CalculateWorldPos (const surface_t surf, const float s, const float t, out vec3 tangent, out vec3 bitangent)
{
	const vec3 normal = vec3 (surf.normal_x, surf.normal_y, surf.normal_z);
	const vec3 pos_edge1 = (abs (normal.x) > abs (normal.z)) ? vec3 (-normal.y, normal.x, 0.0f) : vec3 (0.0f, -normal.z, normal.y);
	const vec3 pos_edge2 = cross (normal, pos_edge1);

	const vec2 st_edge1 = vec2 (dot (pos_edge1, surf.vecs[0].xyz), dot (pos_edge1, surf.vecs[1].xyz));
	const vec2 st_edge2 = vec2 (dot (pos_edge2, surf.vecs[0].xyz), dot (pos_edge2, surf.vecs[1].xyz));

	const float scale = 16.0f / ((st_edge1.x * st_edge2.y) - (st_edge2.x * st_edge1.y));
	tangent = scale * vec3 (
						  (st_edge2.y * pos_edge1.x) + (-st_edge1.y * pos_edge2.x), (st_edge2.y * pos_edge1.y) + (-st_edge1.y * pos_edge2.y),
						  (st_edge2.y * pos_edge1.z) + (-st_edge1.y * pos_edge2.z));
	bitangent = scale * vec3 (
							(-st_edge2.x * pos_edge1.x) + (st_edge1.x * pos_edge2.x), (-st_edge2.x * pos_edge1.y) + (st_edge1.x * pos_edge2.y),
							(-st_edge2.x * pos_edge1.z) + (st_edge1.x * pos_edge2.z));

	const world_vert_t vert = world_verts[surf.vbo_offset];
	const vec3		   vert1_pos = vec3 (vert.x, vert.y, vert.z);
	const float		   vert1_offset_s = -(dot (vert1_pos, surf.vecs[0].xyz) + surf.vecs[0].w) / 16.0f;
	const float		   vert1_offset_t = -(dot (vert1_pos, surf.vecs[1].xyz) + surf.vecs[1].w) / 16.0f;

	return vert1_pos + ((s + vert1_offset_s) * tangent) + ((t + vert1_offset_t) * bitangent);
}
