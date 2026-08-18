/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// r_light.c

#include "quakedef.h"

int r_dlightframecount;

// dlight origins in the space of the model currently having its lightmaps built: world space for the world model,
// entity space for brush models (R_DrawBrushModel updates these while marking its surfaces)
vec3_t lightmap_dlight_origins[MAX_DLIGHTS];

extern cvar_t r_flatlightstyles; // johnfitz
extern cvar_t r_lerplightstyles;
extern cvar_t r_gpulightmapupdate;

cvar_t r_emissive_rt = {"r_emissive_rt", "0", CVAR_NONE};
cvar_t r_emissive_rt_debug = {"r_emissive_rt_debug", "0", CVAR_NONE};

/*
=============================================================================

RT EMISSIVE LIGHTS

=============================================================================
*/

typedef enum emissive_proxy_e
{
	EMISSIVE_PROXY_POINT
} emissive_proxy_t;

typedef struct emissive_texture_def_s
{
	const char		*texture;
	float			 radius;
	float			 intensity;
	float			 normal_offset;
	emissive_proxy_t proxy;
	qboolean		 shadows;
	qboolean		 two_sided;
	qboolean		 derive_color;
	vec3_t			 color;
} emissive_texture_def_t;

typedef struct emissive_world_surface_s
{
	msurface_t					 *surface;
	const emissive_texture_def_t *definition;
	int							  fixture_index;
} emissive_world_surface_t;

typedef struct emissive_world_fixture_s
{
	const emissive_texture_def_t *definition;
	vec3_t						  origin;
	vec3_t						  normal;
	vec3_t						  color;
	float						  geometric_area;
	float						  luminous_area;
	int							  num_surfaces;
} emissive_world_fixture_t;

static const emissive_texture_def_t emissive_texture_defs[] = {
	{"TLIGHT01", 192.0f, 1.6f, 16.0f, EMISSIVE_PROXY_POINT, true, false, true, {0.0f, 0.0f, 0.0f}},
	{"TLIGHT11", 192.0f, 4.8f, 8.0f, EMISSIVE_PROXY_POINT, true, false, true, {0.0f, 0.0f, 0.0f}},
};

static emissive_world_surface_t *emissive_world_surfaces;
static int						 num_emissive_world_surfaces;
static emissive_world_fixture_t *emissive_world_fixtures;
static int						 num_emissive_world_fixtures;
static qmodel_t					*emissive_surface_worldmodel;
static uint32_t					 emissive_prepare_time_us;

static qboolean R_ResolveEmissiveTextureColor (const emissive_texture_def_t *definition, const gltexture_t *fullbright, vec3_t color)
{
	const vec3_t *const source = definition->derive_color ? &fullbright->fullbright_color : &definition->color;
	for (int channel = 0; channel < 3; ++channel)
		color[channel] = q_max (0.0f, (*source)[channel]);
	const float max_component = q_max (color[0], q_max (color[1], color[2]));
	if (max_component <= 0.0f)
		return false;
	VectorScale (color, 1.0f / max_component, color);
	return true;
}

static const emissive_texture_def_t *R_CacheableEmissiveTextureDef (const texture_t *texture)
{
	if (!texture || !texture->fullbright || texture->anim_total || texture->alternate_anims)
		return NULL;

	for (int i = 0; i < countof (emissive_texture_defs); ++i)
		if (!q_strcasecmp (texture->name, emissive_texture_defs[i].texture))
		{
			const emissive_texture_def_t *const definition = &emissive_texture_defs[i];
			vec3_t								color;
			if (texture->fullbright->fullbright_coverage <= 0.0f || !R_ResolveEmissiveTextureColor (definition, texture->fullbright, color))
				return NULL;
			return definition;
		}
	return NULL;
}

static const emissive_texture_def_t *R_CacheableWorldEmissiveSurfaceDef (const msurface_t *surface)
{
	if (surface->numedges < 3 || !surface->texinfo || (surface->flags & SURF_DRAWTILED))
		return NULL;
	return R_CacheableEmissiveTextureDef (surface->texinfo->texture);
}

static void R_ClearEmissiveWorldSurfaces (void)
{
	SAFE_FREE (emissive_world_surfaces);
	SAFE_FREE (emissive_world_fixtures);
	num_emissive_world_surfaces = 0;
	num_emissive_world_fixtures = 0;
	emissive_surface_worldmodel = NULL;
	emissive_prepare_time_us = 0;
}

static const vec3_t *R_EmissiveWorldSurfaceVertex (const qmodel_t *model, const msurface_t *surface, int vertex)
{
	const int surfedge = model->surfedges[surface->firstedge + vertex];
	const int vertex_index = surfedge >= 0 ? model->edges[surfedge].v[0] : model->edges[-surfedge].v[1];
	return &model->vertexes[vertex_index].position;
}

static qboolean R_EmissiveWorldSurfacesShareEdge (const qmodel_t *model, const msurface_t *a, const msurface_t *b)
{
	for (int a_edge_index = 0; a_edge_index < a->numedges; ++a_edge_index)
	{
		const medge_t *const a_edge = &model->edges[abs (model->surfedges[a->firstedge + a_edge_index])];
		for (int b_edge_index = 0; b_edge_index < b->numedges; ++b_edge_index)
		{
			const medge_t *const b_edge = &model->edges[abs (model->surfedges[b->firstedge + b_edge_index])];
			if ((a_edge->v[0] == b_edge->v[0] && a_edge->v[1] == b_edge->v[1]) || (a_edge->v[0] == b_edge->v[1] && a_edge->v[1] == b_edge->v[0]))
				return true;
		}
	}
	return false;
}

static qboolean R_EmissiveWorldSurfacesShareFixture (const qmodel_t *model, const emissive_world_surface_t *a, const emissive_world_surface_t *b)
{
	return a->definition == b->definition && R_EmissiveWorldSurfacesShareEdge (model, a->surface, b->surface);
}

static void R_EmissiveWorldSurfaceGeometry (const qmodel_t *model, const msurface_t *surface, vec3_t center, vec3_t normal, float *area)
{
	const vec3_t *const first = R_EmissiveWorldSurfaceVertex (model, surface, 0);
	vec3_t				weighted_center = {0.0f, 0.0f, 0.0f};
	float				total_area = 0.0f;

	for (int vertex = 1; vertex < surface->numedges - 1; ++vertex)
	{
		const vec3_t *const second = R_EmissiveWorldSurfaceVertex (model, surface, vertex);
		const vec3_t *const third = R_EmissiveWorldSurfaceVertex (model, surface, vertex + 1);
		vec3_t				edge1, edge2, cross, triangle_center;
		VectorSubtract (*second, *first, edge1);
		VectorSubtract (*third, *first, edge2);
		CrossProduct (edge1, edge2, cross);
		const float triangle_area = 0.5f * VectorLength (cross);
		VectorAdd (*first, *second, triangle_center);
		VectorAdd (triangle_center, *third, triangle_center);
		VectorMA (weighted_center, triangle_area / 3.0f, triangle_center, weighted_center);
		total_area += triangle_area;
	}

	if (total_area > 0.0f)
		VectorScale (weighted_center, 1.0f / total_area, center);
	else
		VectorCopy (*first, center);
	VectorCopy (surface->plane->normal, normal);
	if (surface->flags & SURF_PLANEBACK)
		VectorScale (normal, -1.0f, normal);
	*area = total_area;
}

static int R_EmissiveWorldFixtureRoot (int *surface_groups, int surface_index)
{
	int root = surface_index;
	while (surface_groups[root] != root)
		root = surface_groups[root];
	while (surface_groups[surface_index] != surface_index)
	{
		const int parent = surface_groups[surface_index];
		surface_groups[surface_index] = root;
		surface_index = parent;
	}
	return root;
}

static void R_BuildEmissiveWorldFixtures (qmodel_t *worldmodel)
{
	int *const surface_groups = Mem_Alloc (num_emissive_world_surfaces * sizeof (*surface_groups));
	for (int i = 0; i < num_emissive_world_surfaces; ++i)
		surface_groups[i] = i;

	for (int i = 0; i < num_emissive_world_surfaces; ++i)
		for (int j = 0; j < i; ++j)
		{
			if (!R_EmissiveWorldSurfacesShareFixture (worldmodel, &emissive_world_surfaces[i], &emissive_world_surfaces[j]))
				continue;
			const int group_i = R_EmissiveWorldFixtureRoot (surface_groups, i);
			const int group_j = R_EmissiveWorldFixtureRoot (surface_groups, j);
			if (group_i != group_j)
				surface_groups[q_max (group_i, group_j)] = q_min (group_i, group_j);
		}
	for (int i = 0; i < num_emissive_world_surfaces; ++i)
		surface_groups[i] = R_EmissiveWorldFixtureRoot (surface_groups, i);

	for (int i = 0; i < num_emissive_world_surfaces; ++i)
		if (surface_groups[i] == i)
			++num_emissive_world_fixtures;
	if (!num_emissive_world_fixtures)
	{
		Mem_Free (surface_groups);
		return;
	}

	emissive_world_fixtures = Mem_Alloc (num_emissive_world_fixtures * sizeof (*emissive_world_fixtures));
	memset (emissive_world_fixtures, 0, num_emissive_world_fixtures * sizeof (*emissive_world_fixtures));
	int fixture_index = 0;
	for (int group = 0; group < num_emissive_world_surfaces; ++group)
	{
		if (surface_groups[group] != group)
			continue;

		const int						current_fixture = fixture_index++;
		emissive_world_fixture_t *const fixture = &emissive_world_fixtures[current_fixture];
		vec3_t							weighted_origin = {0.0f, 0.0f, 0.0f};
		vec3_t							weighted_normal = {0.0f, 0.0f, 0.0f};
		vec3_t							fallback_origin = {0.0f, 0.0f, 0.0f};
		vec3_t							fallback_normal = {0.0f, 0.0f, 1.0f};
		float							largest_luminous_area = 0.0f;
		fixture->definition = emissive_world_surfaces[group].definition;
		const gltexture_t *const fullbright = emissive_world_surfaces[group].surface->texinfo->texture->fullbright;
		R_ResolveEmissiveTextureColor (fixture->definition, fullbright, fixture->color);

		for (int i = group; i < num_emissive_world_surfaces; ++i)
		{
			if (surface_groups[i] != group)
				continue;
			emissive_world_surfaces[i].fixture_index = current_fixture;
			vec3_t center, normal;
			float  area;
			R_EmissiveWorldSurfaceGeometry (worldmodel, emissive_world_surfaces[i].surface, center, normal, &area);
			const float luminous_area = area * fullbright->fullbright_coverage;
			if (!fixture->num_surfaces)
				VectorCopy (center, fallback_origin);
			VectorMA (weighted_origin, luminous_area, center, weighted_origin);
			VectorMA (weighted_normal, luminous_area, normal, weighted_normal);
			fixture->geometric_area += area;
			fixture->luminous_area += luminous_area;
			++fixture->num_surfaces;
			if (luminous_area > largest_luminous_area)
			{
				largest_luminous_area = luminous_area;
				VectorCopy (normal, fallback_normal);
			}
		}

		if (fixture->luminous_area > 0.0f)
			VectorScale (weighted_origin, 1.0f / fixture->luminous_area, fixture->origin);
		else
			VectorCopy (fallback_origin, fixture->origin);
		if (VectorNormalize (weighted_normal) > 0.0f)
			VectorCopy (weighted_normal, fixture->normal);
		else
			VectorCopy (fallback_normal, fixture->normal);
		VectorMA (fixture->origin, fixture->definition->normal_offset, fixture->normal, fixture->origin);
	}
	assert (fixture_index == num_emissive_world_fixtures);
	for (int i = 0; i < num_emissive_world_surfaces; ++i)
		assert (emissive_world_surfaces[i].fixture_index >= 0 && emissive_world_surfaces[i].fixture_index < num_emissive_world_fixtures);
	Mem_Free (surface_groups);
}

static void R_UploadEmissiveCoarseLights (void)
{
	if (!num_emissive_world_fixtures)
		return;

	emissive_coarse_light_t *const lights = Mem_Alloc (num_emissive_world_fixtures * sizeof (*lights));
	for (int i = 0; i < num_emissive_world_fixtures; ++i)
	{
		const emissive_world_fixture_t *const fixture = &emissive_world_fixtures[i];
		VectorCopy (fixture->origin, lights[i].origin);
		lights[i].radius = fixture->definition->radius;
		VectorCopy (fixture->color, lights[i].color);
		lights[i].intensity = fixture->definition->intensity;
	}
	R_SetEmissiveCoarseLights (lights, num_emissive_world_fixtures);
	Mem_Free (lights);
}

static void R_BuildEmissiveWorldSurfaces (void)
{
	if (r_emissive_rt.value <= 0.0f || !cl.worldmodel)
		return;
	if (emissive_surface_worldmodel == cl.worldmodel)
	{
		if (num_emissive_world_fixtures)
			R_AllocateEmissiveLightmaps ();
		return;
	}
	const double prepare_start = Sys_DoubleTime ();

	R_ClearEmissiveWorldSurfaces ();

	qmodel_t *const	  worldmodel = cl.worldmodel;
	msurface_t *const first_surface = &worldmodel->surfaces[worldmodel->firstmodelsurface];
	for (int i = 0; i < worldmodel->nummodelsurfaces; ++i)
		if (R_CacheableWorldEmissiveSurfaceDef (&first_surface[i]))
			++num_emissive_world_surfaces;

	if (num_emissive_world_surfaces)
	{
		emissive_world_surfaces = Mem_Alloc (num_emissive_world_surfaces * sizeof (*emissive_world_surfaces));

		int surface_index = 0;
		for (int i = 0; i < worldmodel->nummodelsurfaces; ++i)
		{
			const emissive_texture_def_t *definition = R_CacheableWorldEmissiveSurfaceDef (&first_surface[i]);
			if (!definition)
				continue;
			emissive_world_surfaces[surface_index].surface = &first_surface[i];
			emissive_world_surfaces[surface_index].definition = definition;
			++surface_index;
		}
		assert (surface_index == num_emissive_world_surfaces);
		R_BuildEmissiveWorldFixtures (worldmodel);
		if (num_emissive_world_fixtures)
		{
			R_AllocateEmissiveLightmaps ();
			R_UploadEmissiveCoarseLights ();
		}
	}

	emissive_surface_worldmodel = worldmodel;
	emissive_prepare_time_us = (uint32_t)((Sys_DoubleTime () - prepare_start) * 1000000.0);
	Con_DPrintf (
		"RT emissives: %d cacheable world surface%s, %d fixture prox%s (%u bytes)\n", num_emissive_world_surfaces, num_emissive_world_surfaces == 1 ? "" : "s",
		num_emissive_world_fixtures, num_emissive_world_fixtures == 1 ? "y" : "ies",
		(unsigned)(num_emissive_world_surfaces * sizeof (*emissive_world_surfaces) + num_emissive_world_fixtures * sizeof (*emissive_world_fixtures)));
}

void R_EmissiveRTNewMap (void)
{
	R_ClearEmissiveWorldSurfaces ();
	R_BuildEmissiveWorldSurfaces ();
}

void R_EmissiveRTChanged_f (cvar_t *var)
{
	if (var->value > 0.0f)
		R_BuildEmissiveWorldSurfaces ();
}

void R_EmissiveRTStats_f (void)
{
	int		  coarse_lightmaps;
	uint64_t coarse_logical_bytes;
	uint64_t coarse_allocated_bytes;
	int		 coarse_lights;
	uint64_t coarse_light_bytes;
	qboolean coarse_pending;
	R_EmissiveLightmapStats (&coarse_lightmaps, &coarse_logical_bytes, &coarse_allocated_bytes);
	R_EmissiveCoarseLightStats (&coarse_lights, &coarse_light_bytes, &coarse_pending);
	const char *coarse_gpu_time = rs_emissive_coarse_gputime_valid ? va ("%.3f ms", (double)rs_emissive_coarse_gputime_us / 1000.0) : "unavailable";
	const char *coarse_state = !coarse_lightmaps ? "unavailable" : coarse_pending ? "pending" : "ready";
	Con_Printf (
		"RT emissives: %s, %d cacheable world surface%s, %d fixture prox%s, %u CPU bytes, %d coarse lightmap%s, %" PRIu64 " logical GPU bytes, %" PRIu64
		" allocated GPU bytes, %d uploaded coarse light%s, %" PRIu64 " light-buffer bytes, %.3f ms CPU prepare, last GPU coarse %s, coarse %s, debug view %s\n",
		r_emissive_rt.value > 0.0f ? "enabled" : "disabled",
		num_emissive_world_surfaces, num_emissive_world_surfaces == 1 ? "" : "s", num_emissive_world_fixtures, num_emissive_world_fixtures == 1 ? "y" : "ies",
		(unsigned)(num_emissive_world_surfaces * sizeof (*emissive_world_surfaces) + num_emissive_world_fixtures * sizeof (*emissive_world_fixtures)), coarse_lightmaps,
		coarse_lightmaps == 1 ? "" : "s", coarse_logical_bytes, coarse_allocated_bytes, coarse_lights, coarse_lights == 1 ? "" : "s", coarse_light_bytes,
		(double)emissive_prepare_time_us / 1000.0, coarse_gpu_time, coarse_state, r_emissive_rt_debug.value > 0.0f ? "coarse" : "off");
}

/*
==================
R_AnimateLight
==================
*/
void R_AnimateLight (void)
{
	int	   i, j, k, n;
	double f;

	//
	// light animations
	// 'm' is normal light, 'a' is no light, 'z' is double bright
	i = f = cl.time * 10;
	for (j = 0; j < MAX_LIGHTSTYLES; j++)
	{
		if (!cl_lightstyle[j].length)
		{
			d_lightstylevalue[j] = 256; // should be 264 ?
			continue;
		}
		// johnfitz -- r_flatlightstyles
		if (r_flatlightstyles.value == 2)
			k = n = cl_lightstyle[j].peak - 'a';
		else if (r_flatlightstyles.value == 1 || !r_dynamic.value)
			k = n = cl_lightstyle[j].average - 'a';
		else
		{
			k = cl_lightstyle[j].map[i % cl_lightstyle[j].length] - 'a';
			n = cl_lightstyle[j].map[(i + 1) % cl_lightstyle[j].length] - 'a';
		}
		if (!r_gpulightmapupdate.value || !r_lerplightstyles.value || (r_lerplightstyles.value < 2 && abs (n - k) >= ('m' - 'a') / 2))
			n = k;
		d_lightstylevalue[j] = (k + (n - k) * (f - i)) * 22;
		// johnfitz
	}
}

/*
=============================================================================

DYNAMIC LIGHTS

=============================================================================
*/

/*
=============
R_MarkLights -- johnfitz -- rewritten to use LordHavoc's lighting speedup
=============
*/
void R_MarkLights (dlight_t *light, int num, mnode_t *node)
{
	mplane_t	*splitplane;
	msurface_t	*surf;
	vec3_t		 impact;
	float		 dist, l, maxdist;
	unsigned int i;
	int			 j, s, t;

start:

	if (node->contents < 0)
		return;

	splitplane = node->plane;
	if (splitplane->type < 3)
		dist = light->origin[splitplane->type] - splitplane->dist;
	else
		dist = DotProduct (light->origin, splitplane->normal) - splitplane->dist;

	if (dist > light->radius)
	{
		node = node->children[0];
		goto start;
	}
	if (dist < -light->radius)
	{
		node = node->children[1];
		goto start;
	}

	maxdist = light->radius * light->radius;
	// mark the polygons
	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i = 0; i < node->numsurfaces; i++, surf++)
	{
		for (j = 0; j < 3; j++)
			impact[j] = light->origin[j] - surf->plane->normal[j] * dist;
		// clamp center of light to corner and check brightness
		l = DotProduct (impact, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3] - surf->texturemins[0];
		s = l + 0.5;
		if (s < 0)
			s = 0;
		else if (s > surf->extents[0])
			s = surf->extents[0];
		s = l - s;
		l = DotProduct (impact, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3] - surf->texturemins[1];
		t = l + 0.5;
		if (t < 0)
			t = 0;
		else if (t > surf->extents[1])
			t = surf->extents[1];
		t = l - t;
		// compare to minimum light
		if ((s * s + t * t + dist * dist) < maxdist)
		{
			if (surf->dlightframe != r_dlightframecount) // not dynamic until now
			{
				surf->dlightbits[num >> 5] = 1U << (num & 31);
				surf->dlightframe = r_dlightframecount;
			}
			else // already dynamic
				surf->dlightbits[num >> 5] |= 1U << (num & 31);
		}
	}

	if (node->children[0]->contents >= 0)
		R_MarkLights (light, num, node->children[0]);
	if (node->children[1]->contents >= 0)
		R_MarkLights (light, num, node->children[1]);
}

/*
=============
R_PushDlights
=============
*/
void R_PushDlights (void)
{
	int		  i;
	dlight_t *l;

	r_dlightframecount = r_framecount;

	l = cl_dlights;

	for (i = 0; i < MAX_DLIGHTS; i++, l++)
	{
		if (l->die < cl.time || !l->radius)
			continue;
		VectorCopy (l->origin, lightmap_dlight_origins[i]);
		R_MarkLights (l, i, cl.worldmodel->nodes);
	}
}

/*
=============================================================================

2021 RERELEASE DYNAMIC LIGHT ENTITIES

Dimension of the Machine places "dynamiclight" entities for shadow casting
dynamic lights, e.g. behind the fans in hub and start. Their QC spawn function
is empty ("No special functionality, just to hold dynamic shadowcasting lights
for KEX"), the KEX engine reads them directly from the entity lump, so do the
same and turn them into persistent dlights.

The KEX shading math is public: QuakeEX.kpf ships the shader source, and
ComputeLightShading/ComputeSpotLight in progs/includes/QuakeClusteredShading.inc
add color * intensity * (range - dist) / range * saturate(N.L), spotlights
scaled by a linear falloff from cone axis to edge, all multiplied by a shadow
map term. KEX only renders them while its shadow system is enabled
(r_staticshadows 0 removes the lights entirely), so they are tied to the ray
traced occlusion path here as well.

The updated id1 and ctf maps also carry "_shadowlight 1" keys on info_null
entities, but those sit in areas whose lighting is fully baked into the
lightmaps (the same maps have to look right in the classic renderer). Adding
their light on top just double brightens the baked lighting, so they are
deliberately not parsed.

=============================================================================
*/

#define MAX_ENTITY_DLIGHTS 64
#define ENTITY_DLIGHT_KEY  0x40000000 // dlight key space for entity dlights, outside entity indices

extern cvar_t r_rtshadows;

// scales the intensity of the rerelease dynamiclight entities; the KEX intensity
// units don't map 1:1 onto lightmap space, so this is calibrated visually
cvar_t r_entdlightscale = {"r_entdlightscale", "1", CVAR_NONE};

typedef struct entity_dlight_s
{
	vec3_t origin;
	vec3_t color;
	float  radius;
	float  intensity;
	vec3_t cone_dir;
	float  cone_cos; // <= -1: not a spotlight
	float  start_fade_distance;
	float  end_fade_distance;
	int	   style;
} entity_dlight_t;

static entity_dlight_t entity_dlights[MAX_ENTITY_DLIGHTS];
static int			   num_entity_dlights;

/*
=============
R_ParseEntityDlights -- called at map load

Parses "dynamiclight" entities out of the entity lump. The spot direction comes
from an "angle" key or, in a second pass, the origin of the targeted entity
=============
*/
void R_ParseEntityDlights (void)
{
	char		key[128], value[4096];
	const char *data;
	char		targets[MAX_ENTITY_DLIGHTS][64];
	float		cone_angles[MAX_ENTITY_DLIGHTS];

	num_entity_dlights = 0;
	if (!cl.worldmodel || !cl.worldmodel->entities)
		return;

	const qboolean coop_game = (cl.maxclients > 1) && (cl.gametype == GAME_COOP);
	qboolean	   any_targets = false;

	data = cl.worldmodel->entities;
	while (num_entity_dlights < MAX_ENTITY_DLIGHTS)
	{
		data = COM_Parse (data);
		if (!data || com_token[0] != '{')
			break;

		entity_dlight_t *l = &entity_dlights[num_entity_dlights];
		memset (l, 0, sizeof (*l));
		l->color[0] = l->color[1] = l->color[2] = 1.0f;
		l->radius = 300.0f;
		l->intensity = 10.0f;
		l->cone_cos = -2.0f;

		qboolean is_dynamiclight = false;
		qboolean has_angle = false;
		float	 angle = 0.0f;
		float	 cone_angle = 0.0f;
		int		 spawnflags = 0;
		targets[num_entity_dlights][0] = 0;

		while (1)
		{
			data = COM_Parse (data);
			if (!data)
				return;
			if (com_token[0] == '}')
				break;
			q_strlcpy (key, com_token, sizeof (key));
			while (key[0] && key[strlen (key) - 1] == ' ') // remove trailing spaces
				key[strlen (key) - 1] = 0;
			data = COM_ParseEx (data, CPE_ALLOWTRUNC);
			if (!data)
				return;
			q_strlcpy (value, com_token, sizeof (value));

			if (!strcmp (key, "classname"))
				is_dynamiclight = !strcmp (value, "dynamiclight");
			else if (!strcmp (key, "origin"))
				sscanf (value, "%f %f %f", &l->origin[0], &l->origin[1], &l->origin[2]);
			else if (!strcmp (key, "_color"))
				sscanf (value, "%f %f %f", &l->color[0], &l->color[1], &l->color[2]);
			else if (!strcmp (key, "_shadowlightradius"))
				l->radius = atof (value);
			else if (!strcmp (key, "_shadowlightintensity"))
				l->intensity = atof (value);
			else if (!strcmp (key, "_shadowlightconeangle"))
				cone_angle = atof (value);
			else if (!strcmp (key, "_shadowlightstartfadedistance"))
				l->start_fade_distance = atof (value);
			else if (!strcmp (key, "_shadowlightendfadedistance"))
				l->end_fade_distance = atof (value);
			else if (!strcmp (key, "_shadowlightstyle"))
				l->style = CLAMP (0, atoi (value), MAX_LIGHTSTYLES - 1);
			else if (!strcmp (key, "angle"))
			{
				angle = atof (value);
				has_angle = true;
			}
			else if (!strcmp (key, "target"))
				q_strlcpy (targets[num_entity_dlights], value, sizeof (targets[0]));
			else if (!strcmp (key, "spawnflags"))
				spawnflags = atoi (value);
		}

		if (!is_dynamiclight)
			continue;
		if (coop_game && (spawnflags & 1)) // DYNAMICLIGHT_NOT_IN_COOP
			continue;

		if (has_angle && (cone_angle > 0.0f))
		{
			if (angle == -1.0f) // up
			{
				l->cone_dir[0] = l->cone_dir[1] = 0.0f;
				l->cone_dir[2] = 1.0f;
			}
			else if (angle == -2.0f) // down
			{
				l->cone_dir[0] = l->cone_dir[1] = 0.0f;
				l->cone_dir[2] = -1.0f;
			}
			else
			{
				l->cone_dir[0] = cosf (DEG2RAD (angle));
				l->cone_dir[1] = sinf (DEG2RAD (angle));
				l->cone_dir[2] = 0.0f;
			}
			// _shadowlightconeangle is the full apex angle: KEX visibly lights surfaces
			// sideways from the axis (e.g. the fan walls), impossible with a half angle read
			l->cone_cos = cosf (DEG2RAD (cone_angle));
		}
		cone_angles[num_entity_dlights] = cone_angle;
		any_targets = any_targets || (targets[num_entity_dlights][0] != 0);

		++num_entity_dlights;
	}

	if (num_entity_dlights > 0)
		Con_DPrintf ("%d entity dlights\n", num_entity_dlights);

	if (!any_targets)
		return;

	// resolve spot directions from targeted entities, "target" wins over "angle"
	data = cl.worldmodel->entities;
	while (1)
	{
		data = COM_Parse (data);
		if (!data || com_token[0] != '{')
			break;

		char   targetname[64] = "";
		vec3_t origin = {0.0f, 0.0f, 0.0f};

		while (1)
		{
			data = COM_Parse (data);
			if (!data)
				return;
			if (com_token[0] == '}')
				break;
			q_strlcpy (key, com_token, sizeof (key));
			while (key[0] && key[strlen (key) - 1] == ' ') // remove trailing spaces
				key[strlen (key) - 1] = 0;
			data = COM_ParseEx (data, CPE_ALLOWTRUNC);
			if (!data)
				return;
			q_strlcpy (value, com_token, sizeof (value));

			if (!strcmp (key, "targetname"))
				q_strlcpy (targetname, value, sizeof (targetname));
			else if (!strcmp (key, "origin"))
				sscanf (value, "%f %f %f", &origin[0], &origin[1], &origin[2]);
		}

		if (!targetname[0])
			continue;
		for (int i = 0; i < num_entity_dlights; i++)
		{
			if ((cone_angles[i] <= 0.0f) || strcmp (targets[i], targetname) != 0)
				continue;
			vec3_t dir;
			VectorSubtract (origin, entity_dlights[i].origin, dir);
			if (VectorLength (dir) > 0.0f)
			{
				VectorNormalize (dir);
				VectorCopy (dir, entity_dlights[i].cone_dir);
				entity_dlights[i].cone_cos = cosf (DEG2RAD (cone_angles[i]));
			}
		}
	}
}

/*
=============
R_UpdateEntityDlights -- called every frame, keeps the parsed entity dlights alive
=============
*/
void R_UpdateEntityDlights (void)
{
	// KEX ties these lights to its shadow system (r_staticshadows 0 removes them
	// entirely), so require the ray traced occlusion path here as well. They light
	// whole rooms that get retraced every frame, which is much more expensive than
	// the transient dlights, so r_rtshadows 1 (low) leaves them off
	if (!vulkan_globals.ray_query || (r_rtshadows.value < 2.0f) || !r_gpulightmapupdate.value)
		return;

	for (int i = 0; i < num_entity_dlights; i++)
	{
		entity_dlight_t *l = &entity_dlights[i];
		float			 intensity = ((float)d_lightstylevalue[l->style] / 256.0f) * l->intensity * r_entdlightscale.value;
		if (l->end_fade_distance > 0.0f)
		{
			vec3_t offset;
			VectorSubtract (l->origin, r_refdef.vieworg, offset);
			const float view_distance = VectorLength (offset);
			if (view_distance >= l->end_fade_distance)
				continue;
			if ((view_distance > l->start_fade_distance) && (l->end_fade_distance > l->start_fade_distance))
				intensity *= (l->end_fade_distance - view_distance) / (l->end_fade_distance - l->start_fade_distance);
		}
		if (intensity <= 0.0f)
			continue;

		dlight_t *dl = CL_AllocDlight (ENTITY_DLIGHT_KEY + i);
		VectorCopy (l->origin, dl->origin);
		dl->radius = l->radius;
		dl->die = cl.time + 0.001f;
		VectorCopy (l->color, dl->color);
		VectorCopy (l->cone_dir, dl->cone_dir);
		dl->cone_cos = l->cone_cos;
		dl->kex_intensity = intensity;
	}
}

/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/

static void InterpolateLightmap (vec3_t color, msurface_t *surf, int ds, int dt)
{
	byte *lightmap;
	int	  maps, line3, dsfrac = ds & 15, dtfrac = dt & 15, r00 = 0, g00 = 0, b00 = 0, r01 = 0, g01 = 0, b01 = 0, r10 = 0, g10 = 0, b10 = 0, r11 = 0, g11 = 0,
					 b11 = 0;
	int scale;
	line3 = ((surf->extents[0] >> 4) + 1) * 3;

	lightmap = surf->samples + ((dt >> 4) * ((surf->extents[0] >> 4) + 1) + (ds >> 4)) * 3; // LordHavoc: *3 for color

	for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
	{
		scale = d_lightstylevalue[surf->styles[maps]];
		r00 += lightmap[0] * scale;
		g00 += lightmap[1] * scale;
		b00 += lightmap[2] * scale;
		r01 += lightmap[3] * scale;
		g01 += lightmap[4] * scale;
		b01 += lightmap[5] * scale;
		r10 += lightmap[line3 + 0] * scale;
		g10 += lightmap[line3 + 1] * scale;
		b10 += lightmap[line3 + 2] * scale;
		r11 += lightmap[line3 + 3] * scale;
		g11 += lightmap[line3 + 4] * scale;
		b11 += lightmap[line3 + 5] * scale;
		lightmap += ((surf->extents[0] >> 4) + 1) * ((surf->extents[1] >> 4) + 1) * 3; // LordHavoc: *3 for colored lighting
	}

	color[0] = ((((((((r11 - r10) * dsfrac) >> 4) + r10) - ((((r01 - r00) * dsfrac) >> 4) + r00)) * dtfrac) >> 4) + ((((r01 - r00) * dsfrac) >> 4) + r00)) *
			   (1.f / 256.f);
	color[1] = ((((((((g11 - g10) * dsfrac) >> 4) + g10) - ((((g01 - g00) * dsfrac) >> 4) + g00)) * dtfrac) >> 4) + ((((g01 - g00) * dsfrac) >> 4) + g00)) *
			   (1.f / 256.f);
	color[2] = ((((((((b11 - b10) * dsfrac) >> 4) + b10) - ((((b01 - b00) * dsfrac) >> 4) + b00)) * dtfrac) >> 4) + ((((b01 - b00) * dsfrac) >> 4) + b00)) *
			   (1.f / 256.f);
}

/*
=============
RecursiveLightPoint -- johnfitz -- replaced entire function for lit support via lordhavoc
=============
*/
int RecursiveLightPoint (lightcache_t *cache, mnode_t *node, vec3_t rayorg, vec3_t start, vec3_t end, float *maxdist)
{
	float  front, back, frac;
	vec3_t mid;

loc0:
	if (node->contents < 0)
		return false; // didn't hit anything

	// calculate mid point
	if (node->plane->type < 3)
	{
		front = start[node->plane->type] - node->plane->dist;
		back = end[node->plane->type] - node->plane->dist;
	}
	else
	{
		front = DotProduct (start, node->plane->normal) - node->plane->dist;
		back = DotProduct (end, node->plane->normal) - node->plane->dist;
	}

	// LordHavoc: optimized recursion
	if ((back < 0) == (front < 0))
	//		return RecursiveLightPoint (cache, node->children[front < 0], rayorg, start, end, maxdist);
	{
		node = node->children[front < 0];
		goto loc0;
	}

	frac = front / (front - back);
	mid[0] = start[0] + (end[0] - start[0]) * frac;
	mid[1] = start[1] + (end[1] - start[1]) * frac;
	mid[2] = start[2] + (end[2] - start[2]) * frac;

	// go down front side
	if (RecursiveLightPoint (cache, node->children[front < 0], rayorg, start, mid, maxdist))
		return true; // hit something
	else
	{
		unsigned int i;
		int			 ds, dt;
		msurface_t	*surf;

		surf = cl.worldmodel->surfaces + node->firstsurface;
		for (i = 0; i < node->numsurfaces; i++, surf++)
		{
			float  sfront, sback, dist;
			vec3_t raydelta;

			if (surf->flags & SURF_DRAWTILED)
				continue; // no lightmaps

			// ericw -- added double casts to force 64-bit precision.
			// Without them the zombie at the start of jam3_ericw.bsp was
			// incorrectly being lit up in SSE builds.
			ds = (int)((double)DoublePrecisionDotProduct (mid, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3]);
			dt = (int)((double)DoublePrecisionDotProduct (mid, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3]);

			if (ds < surf->texturemins[0] || dt < surf->texturemins[1])
				continue;

			ds -= surf->texturemins[0];
			dt -= surf->texturemins[1];

			if (ds > surf->extents[0] || dt > surf->extents[1])
				continue;

			if (surf->plane->type < 3)
			{
				sfront = rayorg[surf->plane->type] - surf->plane->dist;
				sback = end[surf->plane->type] - surf->plane->dist;
			}
			else
			{
				sfront = DotProduct (rayorg, surf->plane->normal) - surf->plane->dist;
				sback = DotProduct (end, surf->plane->normal) - surf->plane->dist;
			}
			VectorSubtract (end, rayorg, raydelta);
			dist = sfront / (sfront - sback) * VectorLength (raydelta);

			if (!surf->samples)
			{
				// We hit a surface that is flagged as lightmapped, but doesn't have actual lightmap info.
				// Instead of just returning black, we'll keep looking for nearby surfaces that do have valid samples.
				// This fixes occasional pitch-black models in otherwise well-lit areas in DOTM (e.g. mge1m1, mge4m1)
				// caused by overlapping surfaces with mixed lighting data.
				const float nearby = 8.f;
				dist += nearby;
				*maxdist = q_min (*maxdist, dist);
				continue;
			}

			if (dist < *maxdist)
			{
				cache->surfidx = surf - cl.worldmodel->surfaces + 1;
				cache->ds = ds;
				cache->dt = dt;
			}
			else
			{
				cache->surfidx = -1;
			}

			return true; // success
		}

		// go down back side
		return RecursiveLightPoint (cache, node->children[front >= 0], rayorg, mid, end, maxdist);
	}
}

/*
=============
R_LightPoint -- johnfitz -- replaced entire function for lit support via lordhavoc
=============
*/
int R_LightPoint (vec3_t p, float ofs, lightcache_t *cache, vec3_t *lightcolor)
{
	vec3_t start, end;
	float  maxdist = 8192.f; // johnfitz -- was 2048

	if (!cl.worldmodel->lightdata)
	{
		(*lightcolor)[0] = (*lightcolor)[1] = (*lightcolor)[2] = 255;
		return 255;
	}

	start[0] = p[0];
	start[1] = p[1];
	start[2] = p[2] + ofs;
	end[0] = start[0];
	end[1] = start[1];
	end[2] = start[2] - maxdist;

	(*lightcolor)[0] = (*lightcolor)[1] = (*lightcolor)[2] = 0;

	if (cache->surfidx <= 0 // no cache or pitch black
		|| cache->surfidx > cl.worldmodel->numsurfaces || fabsf (cache->pos[0] - p[0]) >= 1.f || fabsf (cache->pos[1] - p[1]) >= 1.f ||
		fabsf (cache->pos[2] - p[2]) >= 1.f)
	{
		cache->surfidx = 0;
		VectorCopy (p, cache->pos);
		RecursiveLightPoint (cache, cl.worldmodel->nodes, start, start, end, &maxdist);
	}

	if (cache && cache->surfidx > 0)
		InterpolateLightmap (*lightcolor, cl.worldmodel->surfaces + cache->surfidx - 1, cache->ds, cache->dt);

	return (((*lightcolor)[0] + (*lightcolor)[1] + (*lightcolor)[2]) * (1.0f / 3.0f));
}
