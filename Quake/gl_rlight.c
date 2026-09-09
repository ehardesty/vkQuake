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
extern cvar_t r_rtshadows;
extern cvar_t gl_fullbrights;

cvar_t r_emissive_rt = {"r_emissive_rt", "0", CVAR_NONE};
cvar_t r_emissive_rt_resolution = {"r_emissive_rt_resolution", "2", CVAR_NONE};
cvar_t r_emissive_rt_occluders = {"r_emissive_rt_occluders", "0", CVAR_NONE};
cvar_t r_emissive_rt_external_bsp = {"r_emissive_rt_external_bsp", "0", CVAR_NONE};
cvar_t r_emissive_rt_liquid_receivers = {"r_emissive_rt_liquid_receivers", "0", CVAR_NONE};
cvar_t r_emissive_rt_translucent_receivers = {"r_emissive_rt_translucent_receivers", "0", CVAR_NONE};
cvar_t r_emissive_rt_sprite_receivers = {"r_emissive_rt_sprite_receivers", "0", CVAR_NONE};
cvar_t r_emissive_rt_particle_receivers = {"r_emissive_rt_particle_receivers", "0", CVAR_NONE};
cvar_t r_emissive_rt_model_emitters = {"r_emissive_rt_model_emitters", "0", CVAR_NONE};
cvar_t r_emissive_rt_debug = {"r_emissive_rt_debug", "0", CVAR_NONE};
cvar_t r_emissive_rt_bandlimit = {"r_emissive_rt_bandlimit", "0", CVAR_NONE};
cvar_t r_emissive_rt_bounce = {"r_emissive_rt_bounce", "1", CVAR_NONE};
cvar_t r_emissive_rt_bounce_strength = {"r_emissive_rt_bounce_strength", "0.65", CVAR_NONE};
cvar_t r_emissive_rt_bounce_reflectance = {"r_emissive_rt_bounce_reflectance", "1", CVAR_NONE};
cvar_t r_emissive_rt_bounce_rays = {"r_emissive_rt_bounce_rays", "8", CVAR_NONE};
cvar_t r_emissive_rt_bounce_resolution = {"r_emissive_rt_bounce_resolution", "0", CVAR_NONE};
cvar_t r_emissive_rt_model_lights = {"r_emissive_rt_model_lights", "2", CVAR_NONE};

/*
=============================================================================

RT EMISSIVE LIGHTS

=============================================================================
*/

typedef enum emissive_proxy_e
{
	EMISSIVE_PROXY_POINT
} emissive_proxy_t;

typedef enum emissive_fixture_family_e
{
	EMISSIVE_FIXTURE_FAMILY_NONE,
	EMISSIVE_FIXTURE_FAMILY_SLIPGATE
} emissive_fixture_family_t;

typedef struct emissive_texture_def_s
{
	const char				 *texture;
	float					  radius;
	float					  intensity;
	float					  normal_offset;
	emissive_proxy_t		  proxy;
	emissive_fixture_family_t fixture_family;
	qboolean				  shadows;
	qboolean				  two_sided;
	qboolean				  derive_color;
	qboolean				  adjacent_inline_brush_owns_source;
	vec3_t					  emission_direction;
	vec3_t					  color;
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
	vec3_t						  mins;
	vec3_t						  maxs;
	float						  geometric_area;
	float						  luminous_area;
	int							  num_surfaces;
	qboolean					  transient_brush_owned;
} emissive_world_fixture_t;

typedef struct emissive_brush_source_s
{
	const emissive_texture_def_t *definition;
	vec3_t						  origin;
	vec3_t						  normal;
	vec3_t						  color;
	vec3_t						  mins;
	vec3_t						  maxs;
} emissive_brush_source_t;

typedef enum emissive_entity_fixture_family_e
{
	EMISSIVE_ENTITY_FIXTURE_WALL_TORCH,
	EMISSIVE_ENTITY_FIXTURE_LARGE_FLAME,
	EMISSIVE_ENTITY_FIXTURE_SMALL_FLAME,
	EMISSIVE_ENTITY_FIXTURE_TALL_BRAZIER,
	EMISSIVE_ENTITY_FIXTURE_SHORT_BRAZIER,
	EMISSIVE_ENTITY_FIXTURE_LONG_TORCH,
	EMISSIVE_ENTITY_FIXTURE_PYRE
} emissive_entity_fixture_family_t;

typedef struct emissive_entity_fixture_def_s
{
	const char						*classname;
	const char						*model;
	int								 frame;
	int								 skin;
	emissive_entity_fixture_family_t family;
	float							 radius;
	float							 intensity;
	vec3_t							 color;
} emissive_entity_fixture_def_t;

typedef struct emissive_entity_candidate_s
{
	const emissive_entity_fixture_def_t *definition;
	uint32_t							 lump_hash;
	int									 lump_ordinal;
	vec3_t								 origin;
	vec3_t								 angles;
	vec3_t								 color;
	float								 authored_light;
	int									 frame;
	int									 skin;
	int									 style;
	int									 spawnflags;
	qboolean							 has_angles;
	qboolean							 has_color;
	qboolean							 has_light;
	qboolean							 has_style;
	qboolean							 has_frame;
	qboolean							 has_skin;
} emissive_entity_candidate_t;

typedef struct emissive_entity_source_s
{
	const emissive_entity_fixture_def_t *definition;
	uint32_t							 lump_hash;
	int									 lump_ordinal;
	int									 static_entity;
	int									 candidate;
	int									 style;
	emissive_light_t					 light;
} emissive_entity_source_t;

#define EMISSIVE_ENTITY_FIXTURE_TABLE_VERSION 2
#define EMISSIVE_ENTITY_ORIGIN_TOLERANCE	  1.0f
#define EMISSIVE_ENTITY_ANGLE_TOLERANCE		  1.0f

static const emissive_texture_def_t emissive_texture_defs[] = {
	/*
	 * Untuned prototype fixtures keep their prototype peak-output ratios, anchored
	 * to validated TLIGHT01 and converted to unit luminance. TLIGHT11 remains independently tuned.
	 */
	{"TLIGHT01", 192.0f, 0.9124f, 16.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
	{"TLIGHT02", 224.0f, 1.3043f, 16.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
	{"TLIGHT03", 128.0f, 0.8694f, 8.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
	{"TLIGHT07", 128.0f, 1.5611f, 8.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
	{"TLIGHT11", 192.0f, 0.7850f, 8.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, true, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}},
	{"CEIL1_1", 96.0f, 0.7538f, 8.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
	{"SFLOOR4_4", 144.0f, 1.3956f, 8.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
	{"TECH03_2", 96.0f, 0.5528f, 6.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
	{"TECH06_2", 96.0f, 0.1913f, 6.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
	{"SLIPLITE",
	 112.0f,
	 0.3189f,
	 8.0f,
	 EMISSIVE_PROXY_POINT,
	 EMISSIVE_FIXTURE_FAMILY_SLIPGATE,
	 true,
	 false,
	 true,
	 false,
	 {0.0f, 0.0f, 0.0f},
	 {0.0f, 0.0f, 0.0f}},
	{"SLIP2", 112.0f, 0.3189f, 8.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_SLIPGATE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
	{"SLIPSIDE",
	 112.0f,
	 0.3189f,
	 8.0f,
	 EMISSIVE_PROXY_POINT,
	 EMISSIVE_FIXTURE_FAMILY_SLIPGATE,
	 true,
	 false,
	 true,
	 false,
	 {0.0f, 0.0f, 0.0f},
	 {0.0f, 0.0f, 0.0f}},
	{"BASEBUTN3", 56.0f, 0.1913f, 4.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
	{"SWITCH_1", 56.0f, 0.1913f, 4.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
	{"COMP1_6", 40.0f, 0.0425f, 2.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
	{"SLIP1", 32.0f, 0.0319f, 2.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
	{"Z_EXIT", 56.0f, 0.1063f, 4.0f, EMISSIVE_PROXY_POINT, EMISSIVE_FIXTURE_FAMILY_NONE, true, false, true, false, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
};

static qboolean R_EmissiveTextureDefsShareFixture (const emissive_texture_def_t *a, const emissive_texture_def_t *b)
{
	return a == b || (a->fixture_family != EMISSIVE_FIXTURE_FAMILY_NONE && a->fixture_family == b->fixture_family);
}

static qboolean R_IsPrimaryEmissiveTextureDef (const emissive_texture_def_t *definition)
{
	for (const emissive_texture_def_t *candidate = emissive_texture_defs; candidate < definition; ++candidate)
		if (R_EmissiveTextureDefsShareFixture (candidate, definition))
			return false;
	return true;
}

static const emissive_entity_fixture_def_t emissive_entity_fixture_defs[] = {
	{"light_torch_small_walltorch", "progs/flame.mdl", -1, 0, EMISSIVE_ENTITY_FIXTURE_WALL_TORCH, 192.0f, 2.0480f, {1.0f, 0.48f, 0.18f}},
	{"light_flame_large_yellow", "progs/flame2.mdl", 1, 0, EMISSIVE_ENTITY_FIXTURE_LARGE_FLAME, 272.0f, 2.6290f, {1.0f, 0.52f, 0.18f}},
	{"light_flame_small_yellow", "progs/flame2.mdl", 0, 0, EMISSIVE_ENTITY_FIXTURE_SMALL_FLAME, 160.0f, 2.6290f, {1.0f, 0.52f, 0.18f}},
	{"light_flame_small_white", "progs/flame2.mdl", 0, 0, EMISSIVE_ENTITY_FIXTURE_SMALL_FLAME, 160.0f, 4.4f, {1.0f, 1.0f, 1.0f}},
	{NULL, "progs/braztall.mdl", -1, -1, EMISSIVE_ENTITY_FIXTURE_TALL_BRAZIER, 272.0f, 2.3328f, {1.0f, 0.5f, 0.18f}},
	{NULL, "progs/brazshrt.mdl", -1, -1, EMISSIVE_ENTITY_FIXTURE_SHORT_BRAZIER, 144.0f, 2.0995f, {1.0f, 0.5f, 0.18f}},
	{NULL, "progs/longtrch.mdl", -1, -1, EMISSIVE_ENTITY_FIXTURE_LONG_TORCH, 152.0f, 2.3328f, {1.0f, 0.5f, 0.18f}},
	{NULL, "progs/flame_pyre.mdl", -1, -1, EMISSIVE_ENTITY_FIXTURE_PYRE, 160.0f, 2.6290f, {1.0f, 0.52f, 0.18f}},
};

static void R_EmissiveWorldSurfaceGeometry (const qmodel_t *model, const msurface_t *surface, vec3_t center, vec3_t normal, float *area);
static const vec3_t *R_EmissiveWorldSurfaceVertex (const qmodel_t *model, const msurface_t *surface, int vertex);
static void			 R_ActivateEmissiveWorldSurfaceCache (void);

static emissive_world_surface_t *emissive_world_surfaces;
static int						 num_emissive_world_surfaces;
static emissive_world_fixture_t *emissive_world_fixtures;
static int						 num_emissive_world_fixtures;
static int						 num_emissive_cacheable_world_fixtures;
static int						 num_emissive_transient_brush_owned_fixtures;
static int						 num_emissive_unpaired_brush_sources;
static int						 num_emissive_ambiguous_brush_owners;
static int						 num_emissive_ambiguous_world_owners;
static int						 num_emissive_world_receivers;
static emissive_surface_light_t *emissive_world_surface_lights;
static int						 num_emissive_world_surface_lights;
static qmodel_t					*emissive_surface_worldmodel;
static uint32_t					 emissive_prepare_time_us;
static qboolean					 emissive_world_lights_uploaded;
static emissive_entity_candidate_t *emissive_entity_candidates;
static int							num_emissive_entity_candidates;
static emissive_entity_source_t	   *emissive_entity_sources;
static int							num_emissive_entity_sources;
static int							num_emissive_entity_candidates_parsed;
static int							num_emissive_entity_candidates_matched;
static int							num_emissive_entity_candidates_ambiguous;
static int							num_emissive_entity_candidates_unmatched;
static int							num_emissive_entity_fallback_sources;
static int							num_emissive_entity_ambiguous_fallbacks;
static int							num_emissive_entity_rejected_sources;
static int							num_emissive_generalized_model_sources;
static int							num_emissive_generalized_model_rejections;
static uint32_t						emissive_entity_lump_hash;
static uint32_t						emissive_entity_discovery_time_us;
static qboolean						emissive_entity_sources_matched;

static void R_ClearEmissiveEntitySources (void)
{
	SAFE_FREE (emissive_entity_candidates);
	SAFE_FREE (emissive_entity_sources);
	num_emissive_entity_candidates = 0;
	num_emissive_entity_sources = 0;
	num_emissive_entity_candidates_parsed = 0;
	num_emissive_entity_candidates_matched = 0;
	num_emissive_entity_candidates_ambiguous = 0;
	num_emissive_entity_candidates_unmatched = 0;
	num_emissive_entity_fallback_sources = 0;
	num_emissive_entity_ambiguous_fallbacks = 0;
	num_emissive_entity_rejected_sources = 0;
	emissive_entity_lump_hash = 0;
	emissive_entity_discovery_time_us = 0;
	emissive_entity_sources_matched = false;
}

static const emissive_entity_fixture_def_t *R_EmissiveEntityFixtureDefForClassname (const char *classname)
{
	for (int i = 0; i < countof (emissive_entity_fixture_defs); ++i)
		if (emissive_entity_fixture_defs[i].classname && !q_strcasecmp (classname, emissive_entity_fixture_defs[i].classname))
			return &emissive_entity_fixture_defs[i];
	return NULL;
}

static qboolean R_EmissiveEntityFixtureDefMatchesVisual (const emissive_entity_fixture_def_t *definition, const entity_t *entity)
{
	if (!entity->model || entity->model->needload || q_strcasecmp (entity->model->name, definition->model))
		return false;
	if (definition->frame >= 0 && entity->frame != definition->frame)
		return false;
	if (definition->skin >= 0 && entity->skinnum != definition->skin)
		return false;
	return true;
}

static const emissive_entity_fixture_def_t *R_EmissiveEntityFallbackDef (const entity_t *entity, qboolean *ambiguous)
{
	if (ambiguous)
		*ambiguous = false;
	const emissive_entity_fixture_def_t *match = NULL;
	for (int i = 0; i < countof (emissive_entity_fixture_defs); ++i)
	{
		const emissive_entity_fixture_def_t *const definition = &emissive_entity_fixture_defs[i];
		if (!R_EmissiveEntityFixtureDefMatchesVisual (definition, entity))
			continue;
		if (match)
		{
			if (ambiguous)
				*ambiguous = true;
			return NULL;
		}
		match = definition;
	}
	return match;
}

static void R_AppendEmissiveEntityCandidate (const emissive_entity_candidate_t *candidate, int *capacity)
{
	if (num_emissive_entity_candidates == *capacity)
	{
		*capacity = *capacity ? *capacity * 2 : 32;
		emissive_entity_candidates = Mem_Realloc (emissive_entity_candidates, *capacity * sizeof (*emissive_entity_candidates));
	}
	emissive_entity_candidates[num_emissive_entity_candidates++] = *candidate;
}

static void R_AppendEmissiveEntitySource (const emissive_entity_source_t *source, int *capacity)
{
	if (num_emissive_entity_sources == *capacity)
	{
		*capacity = *capacity ? *capacity * 2 : 32;
		emissive_entity_sources = Mem_Realloc (emissive_entity_sources, *capacity * sizeof (*emissive_entity_sources));
	}
	emissive_entity_sources[num_emissive_entity_sources++] = *source;
}

static void R_ParseEmissiveEntityCandidates (void)
{
	if (!cl.worldmodel || !cl.worldmodel->entities)
		return;

	const double parse_start = Sys_DoubleTime ();
	const char	*data = cl.worldmodel->entities;
	emissive_entity_lump_hash = COM_HashBlock (data, strlen (data));
	int ordinal = 0;
	int capacity = 0;

	while (1)
	{
		data = COM_Parse (data);
		if (!data || com_token[0] != '{')
			break;

		char						classname[128] = "";
		char						model[MAX_QPATH] = "";
		emissive_entity_candidate_t candidate;
		memset (&candidate, 0, sizeof (candidate));
		candidate.lump_hash = emissive_entity_lump_hash;
		candidate.lump_ordinal = ordinal++;

		qboolean has_origin = false;
		qboolean malformed = false;
		while (1)
		{
			data = COM_Parse (data);
			if (!data)
			{
				malformed = true;
				break;
			}
			if (com_token[0] == '}')
				break;
			char key[128];
			q_strlcpy (key, com_token, sizeof (key));
			while (key[0] && key[strlen (key) - 1] == ' ')
				key[strlen (key) - 1] = 0;
			data = COM_ParseEx (data, CPE_ALLOWTRUNC);
			if (!data)
			{
				malformed = true;
				break;
			}

			if (!strcmp (key, "classname"))
				q_strlcpy (classname, com_token, sizeof (classname));
			else if (!strcmp (key, "model"))
				q_strlcpy (model, com_token, sizeof (model));
			else if (!strcmp (key, "origin"))
				has_origin = sscanf (com_token, "%f %f %f", &candidate.origin[0], &candidate.origin[1], &candidate.origin[2]) == 3;
			else if (!strcmp (key, "angles"))
				candidate.has_angles = sscanf (com_token, "%f %f %f", &candidate.angles[0], &candidate.angles[1], &candidate.angles[2]) == 3;
			else if (!strcmp (key, "angle"))
			{
				candidate.angles[1] = atof (com_token);
				candidate.has_angles = true;
			}
			else if (!strcmp (key, "_color"))
				candidate.has_color = sscanf (com_token, "%f %f %f", &candidate.color[0], &candidate.color[1], &candidate.color[2]) == 3;
			else if (!strcmp (key, "light"))
			{
				candidate.authored_light = atof (com_token);
				candidate.has_light = true;
			}
			else if (!strcmp (key, "style"))
			{
				candidate.style = atoi (com_token);
				candidate.has_style = true;
			}
			else if (!strcmp (key, "spawnflags"))
				candidate.spawnflags = atoi (com_token);
			else if (!strcmp (key, "frame"))
			{
				candidate.frame = atoi (com_token);
				candidate.has_frame = true;
			}
			else if (!strcmp (key, "skin"))
			{
				candidate.skin = atoi (com_token);
				candidate.has_skin = true;
			}
		}

		candidate.definition = R_EmissiveEntityFixtureDefForClassname (classname);
		if (!candidate.definition)
		{
			if (malformed)
				break;
			continue;
		}
		++num_emissive_entity_candidates_parsed;
		if (malformed || !has_origin || candidate.style < 0 || candidate.style >= MAX_LIGHTSTYLES || (candidate.has_light && candidate.authored_light < 0.0f) ||
			(model[0] && q_strcasecmp (model, candidate.definition->model)) ||
			(candidate.has_frame && candidate.definition->frame >= 0 && candidate.frame != candidate.definition->frame) ||
			(candidate.has_skin && candidate.definition->skin >= 0 && candidate.skin != candidate.definition->skin))
		{
			++num_emissive_entity_rejected_sources;
			if (malformed)
				break;
			continue;
		}
		R_AppendEmissiveEntityCandidate (&candidate, &capacity);
	}

	if (num_emissive_entity_candidates)
		emissive_entity_candidates = Mem_Realloc (emissive_entity_candidates, num_emissive_entity_candidates * sizeof (*emissive_entity_candidates));
	emissive_entity_discovery_time_us = (uint32_t)((Sys_DoubleTime () - parse_start) * 1000000.0);
}

static float R_EmissiveEntityAngleDifference (float a, float b)
{
	return fabsf (anglemod (a - b + 180.0f) - 180.0f);
}

static void R_EmissiveEntityProxyOrigin (const entity_t *entity, const emissive_entity_fixture_def_t *definition, vec3_t origin)
{
	vec3_t center, forward, right, up;
	vec3_t entity_origin, entity_angles;
	VectorAdd (entity->model->mins, entity->model->maxs, center);
	VectorScale (center, 0.5f * ENTSCALE_DECODE (entity->netstate.scale), center);
	VectorCopy (entity->origin, entity_origin);
	VectorCopy (entity->angles, entity_angles);
	AngleVectors (entity_angles, forward, right, up);
	VectorCopy (entity_origin, origin);
	VectorMA (origin, center[0], forward, origin);
	VectorMA (origin, -center[1], right, origin);
	VectorMA (origin, center[2], up, origin);

	if (definition->family != EMISSIVE_ENTITY_FIXTURE_WALL_TORCH || !cl.worldmodel)
		return;

	static const vec3_t directions[] = {{1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};
	float				best_distance = FLT_MAX;
	trace_t				best_trace;
	memset (&best_trace, 0, sizeof (best_trace));
	for (int i = 0; i < countof (directions); ++i)
	{
		vec3_t end;
		VectorMA (entity_origin, 32.0f, directions[i], end);
		trace_t trace;
		memset (&trace, 0, sizeof (trace));
		trace.fraction = 1.0f;
		SV_RecursiveHullCheck (cl.worldmodel->hulls, entity_origin, end, &trace, CONTENTMASK_ANYSOLID);
		const float distance = trace.fraction * 32.0f;
		if (!trace.allsolid && trace.fraction < 1.0f && fabsf (trace.plane.normal[2]) < 0.7f && distance < best_distance)
		{
			best_distance = distance;
			best_trace = trace;
		}
	}
	if (best_distance < FLT_MAX)
	{
		vec3_t from_wall;
		VectorSubtract (origin, best_trace.endpos, from_wall);
		const float gap = DotProduct (from_wall, best_trace.plane.normal);
		if (gap < 4.0f)
			VectorMA (origin, 4.0f - gap, best_trace.plane.normal, origin);
	}
}

static qboolean R_NormalizeEmissiveColor (vec3_t color)
{
	for (int channel = 0; channel < 3; ++channel)
		color[channel] = q_max (0.0f, color[channel]);
	const float luminance = 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2];
	if (luminance <= 0.0f)
		return false;
	VectorScale (color, 1.0f / luminance, color);
	return true;
}

static void R_TransformEmissivePoint (const float matrix[16], const vec3_t point, vec3_t transformed);

static void R_BuildEmissiveEntitySourceLight (emissive_entity_source_t *source, const entity_t *entity, const emissive_entity_candidate_t *candidate)
{
	const emissive_entity_fixture_def_t *const definition = source->definition;
	R_EmissiveEntityProxyOrigin (entity, definition, source->light.origin);
	source->light.radius = definition->radius;
	VectorCopy (candidate && candidate->has_color ? candidate->color : definition->color, source->light.color);
	const float authored_scale = candidate && candidate->has_light ? candidate->authored_light / 300.0f : 1.0f;
	source->light.intensity = R_NormalizeEmissiveColor (source->light.color) ? definition->intensity * q_max (0.0f, authored_scale) : 0.0f;
}

#define EMISSIVE_GENERALIZED_MODEL_MAX_SOURCES 64

/*
==================
R_BuildGeneralizedModelEmitter

Reduces the active fullbright skin across all alias/MD5 surfaces to one stable
entity proxy. This is deliberately a source adapter, not animated triangle
transport: pose changes do not rebuild an AS and curated fixture definitions
continue to own any model they recognize.
==================
*/
static qboolean R_BuildGeneralizedModelEmitter (const entity_t *entity, emissive_light_t *light)
{
	const int light_effects = EF_MUZZLEFLASH | EF_BRIGHTLIGHT | EF_DIMLIGHT | EF_QEX_QUADLIGHT | EF_QEX_PENTALIGHT;
	if (!entity || !entity->model || entity->model->needload || entity->model->type != mod_alias || (entity->effects & light_effects) ||
		(entity->alpha != ENTALPHA_DEFAULT && ENTALPHA_DECODE (entity->alpha) < 1.0f))
		return false;

	aliashdr_t *const first_header = (aliashdr_t *)Mod_Extradata_CheckSkin (entity->model, entity->skinnum);
	const int		  anim = (int)(cl.time * 10) & 3;
	vec3_t			  weighted_color = {0.0f, 0.0f, 0.0f};
	float			  luminous_weight = 0.0f;
	float			  total_weight = 0.0f;
	for (aliashdr_t *header = first_header; header; header = header->nextsurface)
	{
		if (header->numskins <= 0)
			continue;
		const int	skin = CLAMP (0, entity->skinnum, header->numskins - 1);
		const float surface_weight = (float)q_max (header->numtris, 1);
		total_weight += surface_weight;
		const gltexture_t *const fullbright = header->fbtextures[skin][anim];
		if (!fullbright || fullbright->fullbright_coverage <= 0.0f)
			continue;
		const float weight = surface_weight * fullbright->fullbright_coverage;
		VectorMA (weighted_color, weight, fullbright->fullbright_color, weighted_color);
		luminous_weight += weight;
	}
	if (luminous_weight <= 0.0f || total_weight <= 0.0f)
		return false;

	VectorScale (weighted_color, 1.0f / luminous_weight, light->color);
	if (!R_NormalizeEmissiveColor (light->color))
		return false;

	vec3_t local_center;
	VectorAdd (entity->model->mins, entity->model->maxs, local_center);
	VectorScale (local_center, 0.5f, local_center);
	float  matrix[16];
	vec3_t origin, angles;
	VectorCopy (entity->origin, origin);
	VectorCopy (entity->angles, angles);
	IdentityMatrix (matrix);
	R_RotateForEntity (matrix, origin, angles, entity->netstate.scale);
	R_TransformEmissivePoint (matrix, local_center, light->origin);

	vec3_t size;
	VectorSubtract (entity->model->maxs, entity->model->mins, size);
	const float half_diagonal = 0.5f * VectorLength (size) * ENTSCALE_DECODE (entity->netstate.scale);
	light->radius = CLAMP (64.0f, half_diagonal * 6.0f, 384.0f);
	light->intensity = 2.0f * sqrtf (CLAMP (0.0f, luminous_weight / total_weight, 1.0f));
	return light->intensity > 0.0f;
}

qboolean R_EmissiveAliasEntityIsSource (const entity_t *entity)
{
	if (!entity || !entity->model || entity->model->needload || entity->model->type != mod_alias)
		return false;
	if (R_EmissiveEntityFallbackDef (entity, NULL))
		return true;
	if (CLAMP (0, (int)r_emissive_rt_model_emitters.value, 1) > 0)
	{
		emissive_light_t light;
		return R_BuildGeneralizedModelEmitter (entity, &light);
	}
	return false;
}

static qboolean R_EmissiveEntityCandidateLocationMatchesVisual (const emissive_entity_candidate_t *candidate, const entity_t *entity)
{
	vec3_t offset;
	VectorSubtract (candidate->origin, entity->origin, offset);
	if (DotProduct (offset, offset) > EMISSIVE_ENTITY_ORIGIN_TOLERANCE * EMISSIVE_ENTITY_ORIGIN_TOLERANCE)
		return false;
	if (candidate->has_angles)
		for (int axis = 0; axis < 3; ++axis)
			if (R_EmissiveEntityAngleDifference (candidate->angles[axis], entity->angles[axis]) > EMISSIVE_ENTITY_ANGLE_TOLERANCE)
				return false;
	return true;
}

static qboolean R_EmissiveEntityCandidateMatchesVisual (const emissive_entity_candidate_t *candidate, const entity_t *entity)
{
	return R_EmissiveEntityFixtureDefMatchesVisual (candidate->definition, entity) &&
		R_EmissiveEntityCandidateLocationMatchesVisual (candidate, entity);
}

static void R_MatchEmissiveEntitySources (void)
{
	if (emissive_entity_sources_matched || cls.signon < 2)
		return;

	const double	match_start = Sys_DoubleTime ();
	qboolean *const claimed = cl.num_statics ? Mem_Alloc (cl.num_statics * sizeof (*claimed)) : NULL;
	qboolean *const authored_association = cl.num_statics ? Mem_Alloc (cl.num_statics * sizeof (*authored_association)) : NULL;
	if (claimed)
	{
		memset (claimed, 0, cl.num_statics * sizeof (*claimed));
		memset (authored_association, 0, cl.num_statics * sizeof (*authored_association));
	}
	int capacity = 0;

	for (int candidate_index = 0; candidate_index < num_emissive_entity_candidates; ++candidate_index)
	{
		const emissive_entity_candidate_t *const candidate = &emissive_entity_candidates[candidate_index];
		int										 match = -1;
		int										 match_count = 0;
		for (int static_index = 0; static_index < cl.num_statics; ++static_index)
		{
			if (R_EmissiveEntityCandidateLocationMatchesVisual (candidate, cl.static_entities[static_index]))
				authored_association[static_index] = true;
			if (R_EmissiveEntityCandidateMatchesVisual (candidate, cl.static_entities[static_index]))
			{
				match = static_index;
				++match_count;
			}
		}

		if (match_count == 1 && !claimed[match])
		{
			emissive_entity_source_t source;
			memset (&source, 0, sizeof (source));
			source.definition = candidate->definition;
			source.lump_hash = candidate->lump_hash;
			source.lump_ordinal = candidate->lump_ordinal;
			source.static_entity = match;
			source.candidate = candidate_index;
			source.style = candidate->has_style ? candidate->style : 255;
			R_BuildEmissiveEntitySourceLight (&source, cl.static_entities[match], candidate);
			R_AppendEmissiveEntitySource (&source, &capacity);
			claimed[match] = true;
			++num_emissive_entity_candidates_matched;
		}
		else if (match_count > 1 || (match_count == 1 && claimed[match]))
			++num_emissive_entity_candidates_ambiguous;
		else
			++num_emissive_entity_candidates_unmatched;
	}

	for (int static_index = 0; static_index < cl.num_statics; ++static_index)
	{
		if (claimed[static_index] || authored_association[static_index])
			continue;
		qboolean								   ambiguous = false;
		const emissive_entity_fixture_def_t *const definition = R_EmissiveEntityFallbackDef (cl.static_entities[static_index], &ambiguous);
		if (!definition)
		{
			if (ambiguous)
				++num_emissive_entity_ambiguous_fallbacks;
			continue;
		}
		emissive_entity_source_t source;
		memset (&source, 0, sizeof (source));
		source.definition = definition;
		source.lump_hash = emissive_entity_lump_hash;
		source.lump_ordinal = -1;
		source.static_entity = static_index;
		source.candidate = -1;
		source.style = 255;
		R_BuildEmissiveEntitySourceLight (&source, cl.static_entities[static_index], NULL);
		R_AppendEmissiveEntitySource (&source, &capacity);
		claimed[static_index] = true;
		++num_emissive_entity_fallback_sources;
	}

	if (num_emissive_entity_sources)
		emissive_entity_sources = Mem_Realloc (emissive_entity_sources, num_emissive_entity_sources * sizeof (*emissive_entity_sources));
	Mem_Free (claimed);
	Mem_Free (authored_association);
	emissive_entity_sources_matched = true;
	emissive_entity_discovery_time_us += (uint32_t)((Sys_DoubleTime () - match_start) * 1000000.0);
	Con_DPrintf (
		"RT emissives: entity fixtures %d parsed, %d matched, %d ambiguous, %d unmatched, %d fallback, %d ambiguous fallback, %d rejected "
		"(%d sources, %.3f ms)\n",
		num_emissive_entity_candidates_parsed, num_emissive_entity_candidates_matched, num_emissive_entity_candidates_ambiguous,
		num_emissive_entity_candidates_unmatched, num_emissive_entity_fallback_sources, num_emissive_entity_ambiguous_fallbacks,
		num_emissive_entity_rejected_sources, num_emissive_entity_sources, (double)emissive_entity_discovery_time_us / 1000.0);
}

static qboolean R_ResolveEmissiveTextureColor (const emissive_texture_def_t *definition, const gltexture_t *fullbright, vec3_t color)
{
	const vec3_t *const source = definition->derive_color ? &fullbright->fullbright_color : &definition->color;
	VectorCopy (*source, color);
	return R_NormalizeEmissiveColor (color);
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
	R_ClearEmissiveEntitySources ();
	SAFE_FREE (emissive_world_surfaces);
	SAFE_FREE (emissive_world_fixtures);
	SAFE_FREE (emissive_world_surface_lights);
	num_emissive_world_surfaces = 0;
	num_emissive_world_fixtures = 0;
	num_emissive_cacheable_world_fixtures = 0;
	num_emissive_transient_brush_owned_fixtures = 0;
	num_emissive_unpaired_brush_sources = 0;
	num_emissive_ambiguous_brush_owners = 0;
	num_emissive_ambiguous_world_owners = 0;
	num_emissive_world_receivers = 0;
	num_emissive_world_surface_lights = 0;
	emissive_surface_worldmodel = NULL;
	emissive_prepare_time_us = 0;
	emissive_world_lights_uploaded = false;
}

static void R_AppendTransientEmissiveSource (
	transient_emissive_source_t **sources, int *count, int *capacity, transient_emissive_source_id_t id, const emissive_light_t *light)
{
	if (*count == *capacity)
	{
		*capacity = *capacity ? *capacity * 2 : 16;
		*sources = Mem_Realloc (*sources, *capacity * sizeof (**sources));
	}
	(*sources)[*count].id = id;
	(*sources)[(*count)++].light = *light;
}

static int R_CompareTransientEmissiveSources (const void *a_, const void *b_)
{
	const transient_emissive_source_t *const a = a_;
	const transient_emissive_source_t *const b = b_;
	return R_CompareTransientEmissiveSourceIds (&a->id, &b->id);
}

static void R_TransformEmissivePoint (const float matrix[16], const vec3_t point, vec3_t transformed)
{
	for (int row = 0; row < 3; ++row)
		transformed[row] = matrix[row] * point[0] + matrix[4 + row] * point[1] + matrix[8 + row] * point[2] + matrix[12 + row];
}

static qboolean R_BuildEmissiveBrushSource (
	const qmodel_t *model, const emissive_texture_def_t *definition, emissive_brush_source_t *source)
{
	memset (source, 0, sizeof (*source));
	source->definition = definition;
	for (int axis = 0; axis < 3; ++axis)
	{
		source->mins[axis] = FLT_MAX;
		source->maxs[axis] = -FLT_MAX;
	}

	vec3_t weighted_origin = {0.0f, 0.0f, 0.0f};
	vec3_t weighted_normal = {0.0f, 0.0f, 0.0f};
	float  total_weight = 0.0f;
	for (int i = 0; i < model->nummodelsurfaces; ++i)
	{
		msurface_t *const					surface = &model->surfaces[model->firstmodelsurface + i];
		texture_t *const					texture = surface->texinfo ? surface->texinfo->texture : NULL;
		const emissive_texture_def_t *const surface_definition = R_CacheableEmissiveTextureDef (texture);
		if (!surface_definition || !R_EmissiveTextureDefsShareFixture (surface_definition, definition) || surface->numedges < 3 ||
			(surface->flags & SURF_DRAWTILED))
			continue;

		vec3_t center, normal, surface_color;
		float  area;
		R_EmissiveWorldSurfaceGeometry (model, surface, center, normal, &area);
		const float weight = area * texture->fullbright->fullbright_coverage;
		R_ResolveEmissiveTextureColor (definition, texture->fullbright, surface_color);
		VectorMA (weighted_origin, weight, center, weighted_origin);
		VectorMA (weighted_normal, weight, normal, weighted_normal);
		VectorMA (source->color, weight, surface_color, source->color);
		total_weight += weight;
		for (int vertex = 0; vertex < surface->numedges; ++vertex)
			for (int axis = 0; axis < 3; ++axis)
			{
				const float coordinate = (*R_EmissiveWorldSurfaceVertex (model, surface, vertex))[axis];
				source->mins[axis] = q_min (source->mins[axis], coordinate);
				source->maxs[axis] = q_max (source->maxs[axis], coordinate);
			}
	}
	if (total_weight <= 0.0f)
		return false;

	VectorScale (weighted_origin, 1.0f / total_weight, source->origin);
	VectorScale (source->color, 1.0f / total_weight, source->color);
	if (VectorLength (definition->emission_direction) > 0.0f)
		VectorCopy (definition->emission_direction, source->normal);
	else
		VectorCopy (weighted_normal, source->normal);
	if (VectorNormalize (source->normal) == 0.0f)
		source->normal[2] = 1.0f;

	/* The immutable-world AS excludes inline brushes. Keep their source at the luminous centroid so fixed world housings can occlude it. */
	return true;
}

static float R_EmissiveBoundsOverlapFraction (float min1, float max1, float min2, float max2)
{
	const float overlap = q_max (0.0f, q_min (max1, max2) - q_max (min1, min2));
	const float minimum_span = q_min (max1 - min1, max2 - min2);
	if (minimum_span > 0.0f)
		return overlap / minimum_span;
	return fabsf (0.5f * (min1 + max1 - min2 - max2)) <= 1.0f ? 1.0f : 0.0f;
}

static qboolean R_EmissiveBrushSourceMatchesWorldFixture (
	const emissive_brush_source_t *source, const emissive_world_fixture_t *fixture)
{
	if (source->definition != fixture->definition || !source->definition->adjacent_inline_brush_owns_source)
		return false;

	int axis = 0;
	for (int candidate_axis = 1; candidate_axis < 3; ++candidate_axis)
		if (fabsf (source->definition->emission_direction[candidate_axis]) > fabsf (source->definition->emission_direction[axis]))
			axis = candidate_axis;
	const float direction = source->definition->emission_direction[axis];
	if (direction == 0.0f)
		return false;
	const float contact_distance = direction > 0.0f ? fabsf (fixture->maxs[axis] - source->mins[axis]) :
													 fabsf (fixture->mins[axis] - source->maxs[axis]);
	if (contact_distance > 1.0f)
		return false;

	for (int lateral_axis = 0; lateral_axis < 3; ++lateral_axis)
		if (lateral_axis != axis &&
			R_EmissiveBoundsOverlapFraction (source->mins[lateral_axis], source->maxs[lateral_axis], fixture->mins[lateral_axis],
				fixture->maxs[lateral_axis]) < 0.5f)
			return false;
	return true;
}

typedef struct emissive_brush_owner_candidate_s
{
	qmodel_t *model;
	int		  fixture;
} emissive_brush_owner_candidate_t;

static void R_ResolveEmissiveWorldFixtureOwnership (void)
{
	num_emissive_cacheable_world_fixtures = num_emissive_world_fixtures;
	num_emissive_transient_brush_owned_fixtures = 0;
	num_emissive_unpaired_brush_sources = 0;
	num_emissive_ambiguous_brush_owners = 0;
	num_emissive_ambiguous_world_owners = 0;
	for (int fixture = 0; fixture < num_emissive_world_fixtures; ++fixture)
		emissive_world_fixtures[fixture].transient_brush_owned = false;
	if (!num_emissive_world_fixtures || cl.num_entities <= 1)
		return;

	emissive_brush_owner_candidate_t *const candidates =
		Mem_Alloc (cl.num_entities * countof (emissive_texture_defs) * sizeof (*candidates));
	int *const fixture_candidate_counts = Mem_Alloc (num_emissive_world_fixtures * sizeof (*fixture_candidate_counts));
	qboolean *const ambiguous_fixtures = Mem_Alloc (num_emissive_world_fixtures * sizeof (*ambiguous_fixtures));
	memset (fixture_candidate_counts, 0, num_emissive_world_fixtures * sizeof (*fixture_candidate_counts));
	memset (ambiguous_fixtures, 0, num_emissive_world_fixtures * sizeof (*ambiguous_fixtures));
	int num_candidates = 0;

	for (int entity_index = 1; entity_index < cl.num_entities; ++entity_index)
	{
		const entity_t *const entity = &cl.entities[entity_index];
		qmodel_t *model = NULL;
		if (entity->baseline.modelindex > 0 && entity->baseline.modelindex < MAX_MODELS)
			model = cl.model_precache[entity->baseline.modelindex];
		if (!model || model->needload || model->type != mod_brush || model->name[0] != '*' || model->surfaces != cl.worldmodel->surfaces)
			continue;

		for (int definition_index = 0; definition_index < countof (emissive_texture_defs); ++definition_index)
		{
			const emissive_texture_def_t *const definition = &emissive_texture_defs[definition_index];
			if (!R_IsPrimaryEmissiveTextureDef (definition) || !definition->adjacent_inline_brush_owns_source)
				continue;
			emissive_brush_source_t source;
			if (!R_BuildEmissiveBrushSource (model, definition, &source))
				continue;
			int matching_fixture = -1, num_matches = 0;
			for (int fixture = 0; fixture < num_emissive_world_fixtures; ++fixture)
				if (R_EmissiveBrushSourceMatchesWorldFixture (&source, &emissive_world_fixtures[fixture]))
				{
					matching_fixture = fixture;
					++num_matches;
				}
			if (num_matches == 1)
			{
				candidates[num_candidates].model = model;
				candidates[num_candidates].fixture = matching_fixture;
				++fixture_candidate_counts[matching_fixture];
				++num_candidates;
			}
			else if (num_matches == 0)
				++num_emissive_unpaired_brush_sources;
			else if (num_matches > 1)
			{
				++num_emissive_ambiguous_brush_owners;
				for (int fixture = 0; fixture < num_emissive_world_fixtures; ++fixture)
					if (R_EmissiveBrushSourceMatchesWorldFixture (&source, &emissive_world_fixtures[fixture]))
						ambiguous_fixtures[fixture] = true;
			}
		}
	}

	for (int fixture = 0; fixture < num_emissive_world_fixtures; ++fixture)
		if (ambiguous_fixtures[fixture] || fixture_candidate_counts[fixture] > 1)
			++num_emissive_ambiguous_world_owners;
	for (int candidate = 0; candidate < num_candidates; ++candidate)
	{
		const int fixture = candidates[candidate].fixture;
		if (ambiguous_fixtures[fixture] || fixture_candidate_counts[fixture] != 1)
			continue;
		emissive_world_fixtures[fixture].transient_brush_owned = true;
		++num_emissive_transient_brush_owned_fixtures;
		Con_DPrintf ("RT emissives: world fixture %d owned by inline brush %s\n", fixture, candidates[candidate].model->name);
	}
	num_emissive_cacheable_world_fixtures -= num_emissive_transient_brush_owned_fixtures;
	Mem_Free (ambiguous_fixtures);
	Mem_Free (fixture_candidate_counts);
	Mem_Free (candidates);
}

/*
==================
R_UpdateTransientEmissiveSources

Collects spatially changing fullbright-derived sources in stable entity/surface
order. Moving inline-brush emitters with fixed source textures are reduced to
one proxy per curated texture definition. Fixed animated textures remain
deferred until radiance modulation can reuse cached transport without rays.
==================
*/
void R_UpdateTransientEmissiveSources (void)
{
	if (!cl.worldmodel || r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f)
	{
		R_InvalidateTransientEmissiveLights ();
		return;
	}
	const qboolean sources_were_matched = emissive_entity_sources_matched;
	R_MatchEmissiveEntitySources ();
	if (!sources_were_matched && emissive_entity_sources_matched)
		R_ActivateEmissiveWorldSurfaceCache ();
	transient_emissive_source_t *sources = NULL;
	int					 count = 0;
	int					 capacity = 0;
	num_emissive_generalized_model_sources = 0;
	num_emissive_generalized_model_rejections = 0;

	if (cl.worldmodel)
	{
		for (int entity_index = 1; entity_index < cl.num_entities; ++entity_index)
		{
			entity_t *const entity = &cl.entities[entity_index];
			qmodel_t *const model = entity->model;
			if (!model || model->needload || model->type != mod_brush || model->name[0] != '*' || model->surfaces != cl.worldmodel->surfaces ||
				(entity->alpha != ENTALPHA_DEFAULT && ENTALPHA_DECODE (entity->alpha) < 1.0f))
				continue;

			vec3_t angles;
			VectorCopy (entity->angles, angles);
			angles[0] = -angles[0];
			float matrix[16];
			IdentityMatrix (matrix);
			R_RotateForEntity (matrix, entity->origin, angles, entity->netstate.scale);

			for (int def_index = 0; def_index < countof (emissive_texture_defs); ++def_index)
			{
				if (!R_IsPrimaryEmissiveTextureDef (&emissive_texture_defs[def_index]))
					continue;
				emissive_brush_source_t source;
				if (!R_BuildEmissiveBrushSource (model, &emissive_texture_defs[def_index], &source))
					continue;
				emissive_light_t light;
				R_TransformEmissivePoint (matrix, source.origin, light.origin);
				light.radius = source.definition->radius;
				VectorCopy (source.color, light.color);
				light.intensity = source.definition->intensity;
				const transient_emissive_source_id_t id = {
					(uint32_t)entity_index, (uint16_t)def_index, TRANSIENT_EMISSIVE_SOURCE_INLINE_BRUSH, 0};
				R_AppendTransientEmissiveSource (&sources, &count, &capacity, id, &light);
			}
		}

		const int light_effects = EF_MUZZLEFLASH | EF_BRIGHTLIGHT | EF_DIMLIGHT | EF_QEX_QUADLIGHT | EF_QEX_PENTALIGHT;
		for (int entity_index = 1; entity_index < cl.num_entities; ++entity_index)
		{
			entity_t *const entity = &cl.entities[entity_index];
			if (!entity->model || entity->model->needload || entity->model->type != mod_alias || (entity->effects & light_effects) ||
				(entity->alpha != ENTALPHA_DEFAULT && ENTALPHA_DECODE (entity->alpha) < 1.0f))
				continue;
			const emissive_entity_fixture_def_t *const definition = R_EmissiveEntityFallbackDef (entity, NULL);
			if (!definition)
				continue;
			emissive_entity_source_t source;
			memset (&source, 0, sizeof (source));
			source.definition = definition;
			R_BuildEmissiveEntitySourceLight (&source, entity, NULL);
			if (source.light.intensity > 0.0f)
			{
				const transient_emissive_source_id_t id = {
					(uint32_t)entity_index, 0, TRANSIENT_EMISSIVE_SOURCE_CURATED_ENTITY, 0};
				R_AppendTransientEmissiveSource (&sources, &count, &capacity, id, &source.light);
			}
		}

		if (r_emissive_rt_model_emitters.value >= 1.0f)
		{
			for (int source_index = 0; source_index < cl.num_statics + cl.num_entities - 1; ++source_index)
			{
				entity_t *const entity = source_index < cl.num_statics ? cl.static_entities[source_index] : &cl.entities[source_index - cl.num_statics + 1];
				if (R_EmissiveEntityFallbackDef (entity, NULL))
					continue;
				emissive_light_t light;
				if (!R_BuildGeneralizedModelEmitter (entity, &light))
					continue;
				if (num_emissive_generalized_model_sources >= EMISSIVE_GENERALIZED_MODEL_MAX_SOURCES)
				{
					++num_emissive_generalized_model_rejections;
					continue;
				}
				const qboolean is_static = source_index < cl.num_statics;
				/* Dynamic owners are 1-based, keeping owner 0 exclusive to the first static source. */
				const transient_emissive_source_id_t id = {
					(uint32_t)(is_static ? source_index : source_index - cl.num_statics + 1), 0,
					is_static ? TRANSIENT_EMISSIVE_SOURCE_GENERALIZED_STATIC : TRANSIENT_EMISSIVE_SOURCE_GENERALIZED_DYNAMIC, 0};
				R_AppendTransientEmissiveSource (&sources, &count, &capacity, id, &light);
				++num_emissive_generalized_model_sources;
			}
		}
	}
	if (count > 1)
	{
		qboolean sorted = true;
		for (int i = 1; i < count; ++i)
			if (R_CompareTransientEmissiveSourceIds (&sources[i - 1].id, &sources[i].id) > 0)
			{
				sorted = false;
				break;
			}
		if (!sorted)
			qsort (sources, count, sizeof (*sources), R_CompareTransientEmissiveSources);
		for (int i = 1; i < count; ++i)
			assert (R_CompareTransientEmissiveSourceIds (&sources[i - 1].id, &sources[i].id) < 0);
	}
	R_SetTransientEmissiveLights (sources, count);
	Mem_Free (sources);
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
	return R_EmissiveTextureDefsShareFixture (a->definition, b->definition) && R_EmissiveWorldSurfacesShareEdge (model, a->surface, b->surface);
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
		for (int axis = 0; axis < 3; ++axis)
		{
			fixture->mins[axis] = FLT_MAX;
			fixture->maxs[axis] = -FLT_MAX;
		}
		const gltexture_t *const fixture_fullbright = emissive_world_surfaces[group].surface->texinfo->texture->fullbright;
		R_ResolveEmissiveTextureColor (fixture->definition, fixture_fullbright, fixture->color);

		for (int i = group; i < num_emissive_world_surfaces; ++i)
		{
			if (surface_groups[i] != group)
				continue;
			emissive_world_surfaces[i].fixture_index = current_fixture;
			vec3_t center, normal;
			float  area;
			R_EmissiveWorldSurfaceGeometry (worldmodel, emissive_world_surfaces[i].surface, center, normal, &area);
			const gltexture_t *const surface_fullbright = emissive_world_surfaces[i].surface->texinfo->texture->fullbright;
			const float			 luminous_area = area * surface_fullbright->fullbright_coverage;
			if (!fixture->num_surfaces)
				VectorCopy (center, fallback_origin);
			VectorMA (weighted_origin, luminous_area, center, weighted_origin);
			VectorMA (weighted_normal, luminous_area, normal, weighted_normal);
			fixture->geometric_area += area;
			fixture->luminous_area += luminous_area;
			++fixture->num_surfaces;
			for (int vertex = 0; vertex < emissive_world_surfaces[i].surface->numedges; ++vertex)
				for (int axis = 0; axis < 3; ++axis)
				{
					const float coordinate =
						(*R_EmissiveWorldSurfaceVertex (worldmodel, emissive_world_surfaces[i].surface, vertex))[axis];
					fixture->mins[axis] = q_min (fixture->mins[axis], coordinate);
					fixture->maxs[axis] = q_max (fixture->maxs[axis], coordinate);
				}
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
		if (VectorLength (fixture->definition->emission_direction) > 0.0f)
			VectorCopy (fixture->definition->emission_direction, fixture->normal);
		else if (VectorNormalize (weighted_normal) > 0.0f)
			VectorCopy (weighted_normal, fixture->normal);
		else
			VectorCopy (fallback_normal, fixture->normal);

		/* A centroid shared by outward-facing fixture sides can lie inside the fixture. Move it beyond the fixture hull before applying the gap. */
		float support_distance = DotProduct (fixture->origin, fixture->normal);
		for (int i = group; i < num_emissive_world_surfaces; ++i)
		{
			if (surface_groups[i] != group)
				continue;
			const msurface_t *const surface = emissive_world_surfaces[i].surface;
			for (int vertex = 0; vertex < surface->numedges; ++vertex)
				support_distance = q_max (support_distance, DotProduct (*R_EmissiveWorldSurfaceVertex (worldmodel, surface, vertex), fixture->normal));
		}
		VectorMA (fixture->origin,
			support_distance - DotProduct (fixture->origin, fixture->normal) + fixture->definition->normal_offset, fixture->normal, fixture->origin);
	}
	assert (fixture_index == num_emissive_world_fixtures);
	for (int i = 0; i < num_emissive_world_surfaces; ++i)
		assert (emissive_world_surfaces[i].fixture_index >= 0 && emissive_world_surfaces[i].fixture_index < num_emissive_world_fixtures);
	Mem_Free (surface_groups);
}

static float R_PointTriangleDistanceSquared (const vec3_t point, const vec3_t a, const vec3_t b, const vec3_t c)
{
	vec3_t ab, ac, ap;
	VectorSubtract (b, a, ab);
	VectorSubtract (c, a, ac);
	VectorSubtract (point, a, ap);
	const float d1 = DotProduct (ab, ap);
	const float d2 = DotProduct (ac, ap);
	if (d1 <= 0.0f && d2 <= 0.0f)
		return DotProduct (ap, ap);

	vec3_t bp;
	VectorSubtract (point, b, bp);
	const float d3 = DotProduct (ab, bp);
	const float d4 = DotProduct (ac, bp);
	if (d3 >= 0.0f && d4 <= d3)
		return DotProduct (bp, bp);

	const float vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
	{
		const float v = d1 / (d1 - d3);
		vec3_t		closest, delta;
		VectorMA (a, v, ab, closest);
		VectorSubtract (point, closest, delta);
		return DotProduct (delta, delta);
	}

	vec3_t cp;
	VectorSubtract (point, c, cp);
	const float d5 = DotProduct (ab, cp);
	const float d6 = DotProduct (ac, cp);
	if (d6 >= 0.0f && d5 <= d6)
		return DotProduct (cp, cp);

	const float vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
	{
		const float w = d2 / (d2 - d6);
		vec3_t		closest, delta;
		VectorMA (a, w, ac, closest);
		VectorSubtract (point, closest, delta);
		return DotProduct (delta, delta);
	}

	const float va = d3 * d6 - d5 * d4;
	if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f)
	{
		const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		vec3_t		bc, closest, delta;
		VectorSubtract (c, b, bc);
		VectorMA (b, w, bc, closest);
		VectorSubtract (point, closest, delta);
		return DotProduct (delta, delta);
	}

	const float denominator = 1.0f / (va + vb + vc);
	const float v = vb * denominator;
	const float w = vc * denominator;
	vec3_t		closest, delta;
	VectorCopy (a, closest);
	VectorMA (closest, v, ab, closest);
	VectorMA (closest, w, ac, closest);
	VectorSubtract (point, closest, delta);
	return DotProduct (delta, delta);
}

static qboolean R_EmissiveLightInfluencesSurface (const qmodel_t *worldmodel, const emissive_light_t *light, const msurface_t *surface)
{
	vec3_t normal;
	VectorCopy (surface->plane->normal, normal);
	float plane_dist = surface->plane->dist;
	if (surface->flags & SURF_PLANEBACK)
	{
		VectorScale (normal, -1.0f, normal);
		plane_dist = -plane_dist;
	}
	if (DotProduct (light->origin, normal) <= plane_dist)
		return false;

	const float	  radius_squared = light->radius * light->radius;
	const vec3_t *first = R_EmissiveWorldSurfaceVertex (worldmodel, surface, 0);
	for (int vertex = 1; vertex < surface->numedges - 1; ++vertex)
	{
		const vec3_t *second = R_EmissiveWorldSurfaceVertex (worldmodel, surface, vertex);
		const vec3_t *third = R_EmissiveWorldSurfaceVertex (worldmodel, surface, vertex + 1);
		if (R_PointTriangleDistanceSquared (light->origin, *first, *second, *third) < radius_squared)
			return true;
	}
	return false;
}

static void R_ClassifyEmissiveWorldReceivers (qmodel_t *worldmodel, const emissive_light_t *lights, int num_lights)
{
	SAFE_FREE (emissive_world_surface_lights);
	num_emissive_world_surface_lights = 0;
	num_emissive_world_receivers = 0;
	int surface_light_capacity = 0;
	msurface_t *const first_surface = &worldmodel->surfaces[worldmodel->firstmodelsurface];
	for (int i = 0; i < worldmodel->nummodelsurfaces; ++i)
	{
		msurface_t *const surface = &first_surface[i];
		surface->cacheable_emissive_influence = false;
		surface->emissive_influence = false;
		surface->emissive_bounce_influence = false;
		if (surface->numedges < 3 || (surface->flags & SURF_DRAWTILED))
			continue;
		for (int light = 0; light < num_lights; ++light)
			if (lights[light].intensity > 0.0f && R_EmissiveLightInfluencesSurface (worldmodel, &lights[light], surface))
			{
				if (!surface->cacheable_emissive_influence)
				{
					surface->cacheable_emissive_influence = true;
					surface->emissive_influence = true;
					++num_emissive_world_receivers;
				}
				if (num_emissive_world_surface_lights == surface_light_capacity)
				{
					surface_light_capacity = surface_light_capacity ? surface_light_capacity * 2 : 64;
					emissive_world_surface_lights =
						Mem_Realloc (emissive_world_surface_lights, surface_light_capacity * sizeof (*emissive_world_surface_lights));
				}
				emissive_world_surface_lights[num_emissive_world_surface_lights].surface = i;
				emissive_world_surface_lights[num_emissive_world_surface_lights].light = light;
				++num_emissive_world_surface_lights;
			}
	}
	if (num_emissive_world_surface_lights)
		emissive_world_surface_lights =
			Mem_Realloc (emissive_world_surface_lights, num_emissive_world_surface_lights * sizeof (*emissive_world_surface_lights));
}

static void R_UploadEmissiveLights (void)
{
	R_ResolveEmissiveWorldFixtureOwnership ();
	const int num_lights = num_emissive_cacheable_world_fixtures + num_emissive_entity_sources;
	if (!num_lights)
	{
		emissive_world_lights_uploaded = true;
		return;
	}

	emissive_light_t *const lights = Mem_Alloc (num_lights * sizeof (*lights));
	byte *const				styles = Mem_Alloc (num_lights * sizeof (*styles));
	int						light = 0;
	for (int i = 0; i < num_emissive_world_fixtures; ++i)
	{
		const emissive_world_fixture_t *const fixture = &emissive_world_fixtures[i];
		if (fixture->transient_brush_owned)
			continue;
		VectorCopy (fixture->origin, lights[light].origin);
		lights[light].radius = fixture->definition->radius;
		VectorCopy (fixture->color, lights[light].color);
		lights[light].intensity = fixture->definition->intensity;
		styles[light++] = 255;
	}
	for (int i = 0; i < num_emissive_entity_sources; ++i)
	{
		lights[light] = emissive_entity_sources[i].light;
		styles[light++] = emissive_entity_sources[i].style;
	}
	assert (light == num_lights);
	R_ClassifyEmissiveWorldReceivers (cl.worldmodel, lights, num_lights);
	if (num_emissive_world_receivers)
	{
		R_AllocateEmissiveLightmaps ();
		R_SetEmissiveLights (lights, styles, num_lights, emissive_world_surface_lights, num_emissive_world_surface_lights);
	}
	Mem_Free (lights);
	Mem_Free (styles);
	emissive_world_lights_uploaded = true;
}

static void R_BuildEmissiveWorldSurfaceCache (void)
{
	if (!cl.worldmodel)
		return;
	if (emissive_surface_worldmodel == cl.worldmodel)
		return;
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
	}
	R_ParseEmissiveEntityCandidates ();

	emissive_surface_worldmodel = worldmodel;
	emissive_prepare_time_us = (uint32_t)((Sys_DoubleTime () - prepare_start) * 1000000.0);
	Con_DPrintf (
		"RT emissives: %d cacheable world surface%s, %d fixture prox%s, %d receiver surface%s (%u bytes, %.3f ms)\n", num_emissive_world_surfaces,
		num_emissive_world_surfaces == 1 ? "" : "s", num_emissive_world_fixtures, num_emissive_world_fixtures == 1 ? "y" : "ies", num_emissive_world_receivers,
		num_emissive_world_receivers == 1 ? "" : "s",
		(unsigned)(num_emissive_world_surfaces * sizeof (*emissive_world_surfaces) + num_emissive_world_fixtures * sizeof (*emissive_world_fixtures) +
			num_emissive_world_surface_lights * sizeof (*emissive_world_surface_lights)),
		(double)emissive_prepare_time_us / 1000.0);
}

static void R_ActivateEmissiveWorldSurfaceCache (void)
{
	if (r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f || !emissive_entity_sources_matched ||
		(!num_emissive_world_fixtures && !num_emissive_entity_sources))
		return;
	if (!emissive_world_lights_uploaded)
		R_UploadEmissiveLights ();
	if (!num_emissive_world_receivers)
		return;
	if (!R_EmissiveDetailReady () && R_EmissiveDetailAvailable ())
		GL_RequestAccelerationStructure (RT_AS_CONSUMER_CACHEABLE_EMISSIVES);
	GL_RebuildIndirectDraws (num_emissive_world_receivers > 0, false);
}

void R_EmissiveRTPrepareNewMap (void)
{
	R_ClearEmissiveWorldSurfaces ();
	if (r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f)
		return;
	R_BuildEmissiveWorldSurfaceCache ();
}

void R_EmissiveRTNewMap (void)
{
	R_EmissiveVolumeNewMap ();
	if (r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f)
		return;
	R_BuildEmissiveWorldSurfaceCache ();
	R_ActivateEmissiveWorldSurfaceCache ();
}

void R_EmissiveRTChanged_f (cvar_t *var)
{
	if (var->value > 0.0f && gl_fullbrights.value > 0.0f)
	{
		R_BuildEmissiveWorldSurfaceCache ();
		R_EmissiveBounceChanged_f (&r_emissive_rt_bounce);
		R_ActivateEmissiveWorldSurfaceCache ();
		R_EmissiveBounceDebugChanged_f (&r_emissive_rt_debug);
	}
	else if (cl.worldmodel)
		GL_RebuildIndirectDraws (false, false);
}

void R_EmissiveBandlimitChanged_f (cvar_t *var)
{
	(void)var;
	if (!cl.worldmodel || r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f)
		return;
	emissive_world_lights_uploaded = false;
	R_InvalidateTransientEmissiveLights ();
	R_EmissiveBounceChanged_f (&r_emissive_rt_bounce);
	R_ActivateEmissiveWorldSurfaceCache ();
}

void R_EmissiveResolutionChanged_f (cvar_t *var)
{
	const int requested_scale = var->value <= 1.0f ? 1 : var->value <= 2.0f ? 2 : 4;
	if (cl.worldmodel && requested_scale != R_EmissiveDetailScale ())
		Con_Printf ("RT emissive direct resolution will change from %dx to %dx on the next map load\n", R_EmissiveDetailScale (), requested_scale);
}

void R_EmissiveLiquidReceiversChanged_f (cvar_t *var)
{
	(void)var;
	if (cl.worldmodel)
		GL_RebuildIndirectDraws (num_emissive_world_receivers > 0 && r_emissive_rt.value > 0.0f, true);
}

void R_RTMaxQuality_f (void)
{
	if (!vulkan_globals.ray_query)
	{
		Con_Printf ("Maximum RT quality is unavailable: this Vulkan device does not support the required ray-query features\n");
		return;
	}

	/* Prerequisites and ordinary dynamic-light shadows. */
	Cvar_SetValueQuick (&gl_fullbrights, 1.0f);
	Cvar_SetValueQuick (&r_gpulightmapupdate, 1.0f);
	Cvar_SetValueQuick (&r_rtshadows, 3.0f);

	/* Direct transport quality and complete optional receiver coverage. */
	Cvar_SetValueQuick (&r_emissive_rt_resolution, 4.0f);
	Cvar_SetValueQuick (&r_emissive_rt_occluders, 2.0f);
	Cvar_SetValueQuick (&r_emissive_rt_external_bsp, 1.0f);
	Cvar_SetValueQuick (&r_emissive_rt_liquid_receivers, 1.0f);
	Cvar_SetValueQuick (&r_emissive_rt_translucent_receivers, 1.0f);
	Cvar_SetValueQuick (&r_emissive_rt_sprite_receivers, 1.0f);
	Cvar_SetValueQuick (&r_emissive_rt_particle_receivers, 1.0f);
	Cvar_SetValueQuick (&r_emissive_rt_model_emitters, 1.0f);
	Cvar_SetValueQuick (&r_emissive_rt_bandlimit, 1.0f);
	Cvar_SetValueQuick (&r_emissive_rt_model_lights, EMISSIVE_CLUSTERED_LIGHTS);

	/* Highest supported bounce sampling without changing artistic energy controls. */
	Cvar_SetValueQuick (&r_emissive_rt_bounce_rays, 128.0f);
	Cvar_SetValueQuick (&r_emissive_rt_bounce_resolution, 1.0f);
	if (r_emissive_rt_bounce_strength.value <= 0.0f)
		Cvar_SetValueQuick (&r_emissive_rt_bounce_strength, 0.65f);
	Cvar_SetValueQuick (&r_emissive_rt_bounce, 1.0f);

	Cvar_SetValueQuick (&r_emissive_rt_debug, 0.0f);
	Cvar_SetValueQuick (&r_emissive_rt, 1.0f);

	Con_Printf (
		"Maximum RT quality requested: dynamic shadows high, direct 4x band-limited, occluders tier 2, all implemented receivers and emitters, "
		"4 model lights, bounce full-coarse at 128 rays/sample\n");
	Con_Printf ("Bounce strength and reflectance remain artistic controls; 4x direct resolution becomes active on the next map load\n");
}

void R_EmissiveRTStats_f (void)
{
	int		 coarse_lightmaps;
	uint64_t coarse_logical_bytes;
	uint64_t coarse_allocated_bytes;
	int		 detail_lightmaps;
	uint64_t detail_logical_bytes;
	uint64_t detail_allocated_bytes;
	uint64_t detail_budget_bytes;
	qboolean detail_budget_limited;
	qboolean detail_pending;
	qboolean detail_as_active;
	qboolean detail_ready;
	int		 affected_tiles;
	int		 total_tiles;
	int		 tile_source_links;
	int		 tile_dispatches;
	uint64_t tile_cpu_bytes;
	uint64_t tile_gpu_bytes;
	int		 transient_lights;
	int		 transient_tiles;
	int		 transient_source_links;
	uint64_t transient_cpu_bytes;
	uint64_t transient_gpu_bytes;
	uint32_t transient_cpu_time_us;
	uint32_t transient_rejected_publications;
	qboolean transient_pending;
	qboolean transient_detail_ready;
	int		 radiance_groups;
	int		 radiance_tiles;
	int		 radiance_source_links;
	int		 radiance_tile_groups;
	int		 radiance_max_groups_per_tile;
	uint64_t radiance_cpu_bytes;
	uint64_t radiance_gpu_bytes;
	uint32_t radiance_cpu_time_us;
	qboolean radiance_visibility_available;
	qboolean radiance_pending;
	int		 emissive_lights;
	uint64_t emissive_light_bytes;
	qboolean coarse_pending;
	uint64_t emissive_world_as_bytes;
	uint32_t emissive_world_as_triangles;
	uint32_t emissive_world_as_build_time_us;
	qboolean emissive_world_as_build_time_valid;
	qboolean emissive_world_as_ready;
	qboolean live_as_ready;
	uint32_t live_as_instances;
	uint32_t bounce_direct_texels, bounce_samples, bounce_rays, bounce_valid_taps, bounce_invalid_taps;
	uint64_t bounce_logical_bytes, bounce_allocated_bytes, bounce_budget_bytes;
	uint32_t bounce_prepare_time_us, bounce_build_time_us, bounce_resolve_time_us, bounce_filter_time_us, bounce_combine_time_us;
	uint32_t bounce_refresh_cpu_time_us, bounce_no_ray_refreshes, bounce_dirty_receiver_surfaces, bounce_cacheable_direct_epoch, bounce_cacheable_epoch;
	uint32_t bounce_transient_direct_epoch, bounce_transient_epoch;
	qboolean bounce_gpu_time_valid, bounce_budget_limited, bounce_pending, bounce_ready;
	qboolean bandlimit_active, bandlimit_budget_limited;
	uint64_t bandlimit_logical_bytes, bandlimit_allocated_bytes, bandlimit_peak_bytes;
	int		 brush_receiver_records, brush_receiver_active, brush_receiver_ready, brush_receiver_dirty, brush_receiver_layers;
	uint64_t brush_receiver_allocated_bytes, brush_receiver_budget_bytes;
	qboolean brush_receiver_budget_limited;
	uint32_t brush_receiver_updates, brush_receiver_dispatches, brush_receiver_no_ray_dispatches, brush_receiver_transform_invalidations;
	int		 clustered_alias_records, clustered_alias_active, clustered_alias_ready;
	uint32_t clustered_alias_receivers, clustered_alias_builds, clustered_alias_source_evaluations, clustered_alias_shadow_tests,
		clustered_alias_shadow_rejections, clustered_alias_contributors;
	R_EmissiveLightmapStats (&coarse_lightmaps, &coarse_logical_bytes, &coarse_allocated_bytes);
	R_EmissiveDetailLightmapStats (
		&detail_lightmaps, &detail_logical_bytes, &detail_allocated_bytes, &detail_budget_bytes, &detail_budget_limited, &detail_pending, &detail_as_active,
		&detail_ready);
	R_EmissiveLightStats (&emissive_lights, &emissive_light_bytes, &coarse_pending);
	R_EmissiveTileStats (&affected_tiles, &total_tiles, &tile_source_links, &tile_dispatches, &tile_cpu_bytes, &tile_gpu_bytes);
	R_TransientEmissiveStats (
		&transient_lights, &transient_tiles, &transient_source_links, &transient_cpu_bytes, &transient_gpu_bytes, &transient_cpu_time_us,
		&transient_rejected_publications, &transient_pending, &transient_detail_ready);
	R_EmissiveRadianceStats (
		&radiance_groups, &radiance_tiles, &radiance_source_links, &radiance_tile_groups, &radiance_max_groups_per_tile, &radiance_cpu_bytes,
		&radiance_gpu_bytes, &radiance_cpu_time_us, &radiance_visibility_available, &radiance_pending);
	R_EmissiveBounceStats (
		&bounce_direct_texels, &bounce_samples, &bounce_rays, &bounce_valid_taps, &bounce_invalid_taps, &bounce_logical_bytes, &bounce_allocated_bytes,
		&bounce_budget_bytes, &bounce_prepare_time_us, &bounce_build_time_us, &bounce_resolve_time_us, &bounce_filter_time_us, &bounce_combine_time_us,
		&bounce_refresh_cpu_time_us, &bounce_no_ray_refreshes, &bounce_dirty_receiver_surfaces, &bounce_cacheable_direct_epoch, &bounce_cacheable_epoch,
		&bounce_transient_direct_epoch, &bounce_transient_epoch, &bounce_gpu_time_valid, &bounce_budget_limited, &bounce_pending, &bounce_ready);
	R_EmissiveBandlimitStats (&bandlimit_active, &bandlimit_budget_limited, &bandlimit_logical_bytes, &bandlimit_allocated_bytes, &bandlimit_peak_bytes);
	R_EmissiveBrushReceiverStats (
		&brush_receiver_records, &brush_receiver_active, &brush_receiver_ready, &brush_receiver_dirty, &brush_receiver_layers, &brush_receiver_allocated_bytes,
		&brush_receiver_budget_bytes, &brush_receiver_budget_limited, &brush_receiver_updates, &brush_receiver_dispatches, &brush_receiver_no_ray_dispatches,
		&brush_receiver_transform_invalidations);
	R_EmissiveClusteredAliasStats (
		&clustered_alias_records, &clustered_alias_active, &clustered_alias_ready, &clustered_alias_receivers, &clustered_alias_builds,
		&clustered_alias_source_evaluations, &clustered_alias_shadow_tests, &clustered_alias_shadow_rejections, &clustered_alias_contributors);
	GL_EmissiveWorldAccelerationStructureStats (
		&emissive_world_as_bytes, &emissive_world_as_triangles, &emissive_world_as_build_time_us, &emissive_world_as_build_time_valid,
		&emissive_world_as_ready);
	GL_LiveAccelerationStructureStats (&live_as_ready, &live_as_instances);
	const char *coarse_gpu_time = rs_emissive_coarse_gputime_valid ? va ("%.3f ms", (double)rs_emissive_coarse_gputime_us / 1000.0) : "unavailable";
	const char *detail_gpu_time = rs_emissive_detail_gputime_valid ? va ("%.3f ms", (double)rs_emissive_detail_gputime_us / 1000.0) : "unavailable";
	const char *brush_receiver_gpu_time =
		rs_emissive_brush_receiver_gputime_valid ? va ("%.3f ms", (double)rs_emissive_brush_receiver_gputime_us / 1000.0) : "unavailable";
	const char				*coarse_state = !coarse_lightmaps ? "unavailable" : coarse_pending ? "pending" : "ready";
	const char				*detail_state = !detail_lightmaps ? "unavailable" : detail_pending ? "pending" : detail_ready ? "ready" : "unbuilt";
	const char				*live_as_gpu_time = rs_live_as_gputime_valid ? va ("%.3f ms", (double)rs_live_as_gputime_us / 1000.0) : "unavailable";
	static const char *const debug_names[] = {
		"off",
		"coarse",
		"detail",
		"validity",
		"selected",
		"bounce x32",
		"band-limit output",
		"band-limit validity",
		"stored band-limit detail",
		"minimum accepted band-limit tap"};
	const int	   debug_mode = CLAMP (0, (int)r_emissive_rt_debug.value, (int)countof (debug_names) - 1);
	const int	   detail_scale = R_EmissiveDetailScale ();
	const int	   detail_workgroups = affected_tiles * detail_scale * detail_scale;
	const uint64_t max_source_evaluations = (uint64_t)tile_source_links * 8 * detail_scale * 8 * detail_scale;
	const uint64_t bandlimit_rays = bandlimit_active ? max_source_evaluations * EMISSIVE_BANDLIMIT_SAMPLES : 0;
	Con_Printf (
		"RT emissives: %s, %d cacheable world surface%s, %d fixture prox%s, %d receiver surface%s, %u CPU bytes, %d coarse lightmap%s, %" PRIu64
		" logical GPU bytes, %" PRIu64 " allocated GPU bytes, %d uploaded source light%s, %" PRIu64
		" light-buffer bytes, %.3f ms CPU prepare, last GPU coarse %s, coarse %s, debug view %s\n",
		r_emissive_rt.value > 0.0f ? "enabled" : "disabled", num_emissive_world_surfaces, num_emissive_world_surfaces == 1 ? "" : "s",
		num_emissive_world_fixtures, num_emissive_world_fixtures == 1 ? "y" : "ies", num_emissive_world_receivers, num_emissive_world_receivers == 1 ? "" : "s",
		(unsigned)(num_emissive_world_surfaces * sizeof (*emissive_world_surfaces) + num_emissive_world_fixtures * sizeof (*emissive_world_fixtures) +
				   num_emissive_world_surface_lights * sizeof (*emissive_world_surface_lights)),
		coarse_lightmaps, coarse_lightmaps == 1 ? "" : "s", coarse_logical_bytes, coarse_allocated_bytes, emissive_lights, emissive_lights == 1 ? "" : "s",
		emissive_light_bytes, (double)emissive_prepare_time_us / 1000.0, coarse_gpu_time, coarse_state, debug_names[debug_mode]);
	Con_Printf (
		"RT emissive brush ownership: %d cacheable world fixture%s, %d transient-brush-owned, %d unpaired brush source%s, %d ambiguous brush/%d ambiguous "
		"world match%s\n",
		num_emissive_cacheable_world_fixtures, num_emissive_cacheable_world_fixtures == 1 ? "" : "s", num_emissive_transient_brush_owned_fixtures,
		num_emissive_unpaired_brush_sources, num_emissive_unpaired_brush_sources == 1 ? "" : "s", num_emissive_ambiguous_brush_owners,
		num_emissive_ambiguous_world_owners, num_emissive_ambiguous_brush_owners + num_emissive_ambiguous_world_owners == 1 ? "" : "es");
	Con_Printf (
		"RT emissive brush receivers: %d record%s, %d active, %d ready, %d dirty, %d cropped lightmap layer%s, %" PRIu64 "/%" PRIu64
		" allocated GPU bytes, %u receiver update%s, %u image dispatch%s (%u transport/%u no-ray radiance), last GPU update %s, %u transform "
		"invalidation%s%s\n",
		brush_receiver_records, brush_receiver_records == 1 ? "" : "s", brush_receiver_active, brush_receiver_ready, brush_receiver_dirty,
		brush_receiver_layers, brush_receiver_layers == 1 ? "" : "s", brush_receiver_allocated_bytes, brush_receiver_budget_bytes, brush_receiver_updates,
		brush_receiver_updates == 1 ? "" : "s", brush_receiver_dispatches, brush_receiver_dispatches == 1 ? "" : "es",
		brush_receiver_dispatches - brush_receiver_no_ray_dispatches, brush_receiver_no_ray_dispatches, brush_receiver_gpu_time,
		brush_receiver_transform_invalidations, brush_receiver_transform_invalidations == 1 ? "" : "s",
		brush_receiver_budget_limited ? ", budget limited; coarse or inactive fallback" : "");
	Con_Printf (
		"RT emissive brush receiver policy: inline BSP on, external BSP %s, translucent BSP %s, liquid %s\n",
		r_emissive_rt_external_bsp.value > 0.0f ? "on" : "off", r_emissive_rt_translucent_receivers.value > 0.0f ? "on" : "off",
		r_emissive_rt_liquid_receivers.value > 0.0f ? "on" : "off");
	Con_Printf (
		"RT emissive generalized receivers: alias light limit %d/%d, model emitters tier %d (%d active/%d budget rejected), %d record%s/%d active/%d ready, %u "
		"cache build%s, %u no-ray selection%s, "
		"%u source evaluation%s, %u selected contributor%s, %u bounded world/brush-shadow test%s, %u rejection%s; surface occluder tier %d; "
		"sprite receivers %s, classic particle receivers %s; scripted particles remain a separate self-emission-only class\n",
		CLAMP (0, (int)r_emissive_rt_model_lights.value, EMISSIVE_CLUSTERED_LIGHTS), EMISSIVE_CLUSTERED_LIGHTS,
		CLAMP (0, (int)r_emissive_rt_model_emitters.value, 1), num_emissive_generalized_model_sources, num_emissive_generalized_model_rejections,
		clustered_alias_records, clustered_alias_records == 1 ? "" : "s", clustered_alias_active, clustered_alias_ready, clustered_alias_builds,
		clustered_alias_builds == 1 ? "" : "s", clustered_alias_receivers, clustered_alias_receivers == 1 ? "" : "s", clustered_alias_source_evaluations,
		clustered_alias_source_evaluations == 1 ? "" : "s", clustered_alias_contributors, clustered_alias_contributors == 1 ? "" : "s",
		clustered_alias_shadow_tests, clustered_alias_shadow_tests == 1 ? "" : "s", clustered_alias_shadow_rejections,
		clustered_alias_shadow_rejections == 1 ? "" : "s", CLAMP (0, (int)r_emissive_rt_occluders.value, 2),
		r_emissive_rt_sprite_receivers.value > 0.0f ? "on" : "off", r_emissive_rt_particle_receivers.value > 0.0f ? "on" : "off");
	Con_Printf (
		"RT emissive detail: requested %dx/active %dx, %d dense lightmap%s, %" PRIu64 " logical GPU bytes, %" PRIu64 " allocated GPU bytes, %" PRIu64
		" byte budget, %d/%d affected 8x8 tile%s (%.1f%%), %d tile-source link%s (%.2f/tile), %" PRIu64 " tile CPU bytes, %" PRIu64
		" tile-list GPU bytes, %d dispatch%s/%d workgroups, %" PRIu64 " maximum source evaluation%s, last GPU detail %s, %s%s\n",
		r_emissive_rt_resolution.value <= 1.0f ? 1 : r_emissive_rt_resolution.value <= 2.0f ? 2 : 4, detail_scale, detail_lightmaps,
		detail_lightmaps == 1 ? "" : "s", detail_logical_bytes, detail_allocated_bytes, detail_budget_bytes, affected_tiles, total_tiles,
		affected_tiles == 1 ? "" : "s", total_tiles ? 100.0 * affected_tiles / total_tiles : 0.0, tile_source_links, tile_source_links == 1 ? "" : "s",
		affected_tiles ? (double)tile_source_links / affected_tiles : 0.0, tile_cpu_bytes, tile_gpu_bytes, tile_dispatches, tile_dispatches == 1 ? "" : "es",
		detail_workgroups, max_source_evaluations, max_source_evaluations == 1 ? "" : "s", detail_gpu_time, detail_state,
		detail_budget_limited ? ", budget exceeded; coarse fallback" : "");
	Con_Printf (
		"RT emissive band-limit v%d: requested %s, %s, %d deterministic receiver samples, %" PRIu64
		" maximum rays, per-source coverage (no neighbor filter), %" PRIu64 " logical/%" PRIu64
		" allocated retained-visibility bytes, %" PRIu64 " aggregate required bytes, last GPU coverage %s%s\n",
		EMISSIVE_BANDLIMIT_VERSION,
		r_emissive_rt_bandlimit.value > 0.0f ? "on" : "off", bandlimit_active ? "active" : "classic fallback",
		EMISSIVE_BANDLIMIT_SAMPLES, bandlimit_rays, bandlimit_logical_bytes, bandlimit_allocated_bytes,
		bandlimit_peak_bytes, bandlimit_active ? detail_gpu_time : "unavailable", bandlimit_budget_limited ? ", budget exceeded" : "");
	if (emissive_world_as_build_time_valid)
		Con_Printf (
			"RT emissive world AS: %s, %u triangle%s, %" PRIu64 " allocated GPU bytes, %.3f ms GPU build\n",
			emissive_world_as_ready ? "ready" : "unavailable", emissive_world_as_triangles, emissive_world_as_triangles == 1 ? "" : "s",
			emissive_world_as_bytes, (double)emissive_world_as_build_time_us / 1000.0);
	else
		Con_Printf (
			"RT emissive world AS: %s, %u triangle%s, %" PRIu64 " allocated GPU bytes, GPU build timing unavailable\n",
			emissive_world_as_ready ? "ready" : "unavailable", emissive_world_as_triangles, emissive_world_as_triangles == 1 ? "" : "s",
			emissive_world_as_bytes);
	Con_Printf (
		"RT AS consumers: cacheable emissives %s, transient emissives %s (occluder tier %d, immutable world %s), RT shadows %s (live scene %s, %u instance%s, last %.3f ms CPU / %s GPU)\n",
		detail_as_active ? "active" : "inactive", transient_pending && transient_tiles && vulkan_globals.ray_query ? "active" : "inactive",
		CLAMP (0, (int)r_emissive_rt_occluders.value, 2), emissive_world_as_ready ? "resident" : "unavailable",
		(vulkan_globals.ray_query && r_rtshadows.value > 0.0f && r_gpulightmapupdate.value > 0.0f) ? "active" : "inactive",
		live_as_ready ? "ready" : "unavailable", live_as_instances,
		live_as_instances == 1 ? "" : "s", (double)rs_live_as_cputime_us / 1000.0, live_as_gpu_time);
	Con_Printf (
		"RT emissive transient: %d active source%s, %d changed 8x8 tile%s, %d tile-source link%s, %" PRIu64 " CPU bytes, %" PRIu64
		" GPU bytes, last %.3f ms CPU / %s GPU, detail %s%s, %u rejected stale publication%s\n",
		transient_lights, transient_lights == 1 ? "" : "s", transient_tiles, transient_tiles == 1 ? "" : "s", transient_source_links,
		transient_source_links == 1 ? "" : "s", transient_cpu_bytes, transient_gpu_bytes, (double)transient_cpu_time_us / 1000.0,
		rs_emissive_transient_gputime_valid ? va ("%.3f ms", (double)rs_emissive_transient_gputime_us / 1000.0) : "unavailable",
		transient_detail_ready ? "ready" : "coarse-only", transient_pending ? ", pending" : "", transient_rejected_publications,
		transient_rejected_publications == 1 ? "" : "s");
	Con_Printf (
		"RT emissive radiance: %d modulation group%s, %d dirty 8x8 tile%s, %d tile-source link%s, %.2f average/%d maximum group%s per "
		"dirty tile, %" PRIu64 " CPU bytes, %" PRIu64 " GPU bytes, retained visibility %s, last %.3f ms CPU / %s GPU%s\n",
		radiance_groups, radiance_groups == 1 ? "" : "s", radiance_tiles, radiance_tiles == 1 ? "" : "s", radiance_source_links,
		radiance_source_links == 1 ? "" : "s", radiance_tiles ? (double)radiance_tile_groups / radiance_tiles : 0.0, radiance_max_groups_per_tile,
		radiance_max_groups_per_tile == 1 ? "" : "s", radiance_cpu_bytes, radiance_gpu_bytes, radiance_visibility_available ? "ready" : "unavailable",
		(double)radiance_cpu_time_us / 1000.0,
		rs_emissive_radiance_gputime_valid ? va ("%.3f ms", (double)rs_emissive_radiance_gputime_us / 1000.0) : "unavailable",
		radiance_pending ? ", pending" : "");
	Con_Printf (
		"RT emissive bounce: %s, %s resolution (%d world units), strength %.3f, reflectance lift %.3f, %d ray%s/sample, %u direct texel%s, %u receiver sample%s, %u transfer ray%s, %u valid/%u invalid taps, %" PRIu64
		" logical/%" PRIu64 " allocated GPU bytes, %" PRIu64 " byte budget, %.3f ms CPU prepare, GPU build/resolve/filter/combine %s, "
		"%u no-ray refresh%s, last refresh %.3f ms CPU / %s GPU over %u receiver surface%s, epochs cacheable %u/%u transient %u/%u%s\n",
		r_emissive_rt_bounce.value <= 0.0f || r_emissive_rt_bounce_strength.value <= 0.0f ? "disabled" :
			bounce_pending ? "pending" : bounce_ready ? "ready" : bounce_budget_limited ? "direct-only" : "unavailable",
		CLAMP (0, (int)r_emissive_rt_bounce_resolution.value, 1) ? "full-coarse" : "half-coarse",
		CLAMP (0, (int)r_emissive_rt_bounce_resolution.value, 1) ? 16 : 32,
		CLAMP (0.0f, r_emissive_rt_bounce_strength.value, 4.0f), CLAMP (0.0f, r_emissive_rt_bounce_reflectance.value, 1.0f),
		CLAMP (1, (int)r_emissive_rt_bounce_rays.value, 128), CLAMP (1, (int)r_emissive_rt_bounce_rays.value, 128) == 1 ? "" : "s", bounce_direct_texels,
		bounce_direct_texels == 1 ? "" : "s", bounce_samples, bounce_samples == 1 ? "" : "s", bounce_rays, bounce_rays == 1 ? "" : "s",
		bounce_valid_taps, bounce_invalid_taps, bounce_logical_bytes, bounce_allocated_bytes, bounce_budget_bytes,
		(double)bounce_prepare_time_us / 1000.0,
		bounce_gpu_time_valid ? va ("%.3f/%.3f/%.3f/%.3f ms", (double)bounce_build_time_us / 1000.0,
			(double)bounce_resolve_time_us / 1000.0, (double)bounce_filter_time_us / 1000.0, (double)bounce_combine_time_us / 1000.0) :
			"unavailable",
		bounce_no_ray_refreshes, bounce_no_ray_refreshes == 1 ? "" : "es", (double)bounce_refresh_cpu_time_us / 1000.0,
		rs_emissive_bounce_refresh_gputime_valid ? va ("%.3f ms", (double)rs_emissive_bounce_refresh_gputime_us / 1000.0) : "unavailable",
		bounce_dirty_receiver_surfaces,
		bounce_dirty_receiver_surfaces == 1 ? "" : "s", bounce_cacheable_direct_epoch, bounce_cacheable_epoch,
		bounce_transient_direct_epoch, bounce_transient_epoch,
		bounce_budget_limited ? ", budget exceeded; direct-only fallback" : "");
	Con_Printf (
		"RT emissive entity fixtures: table v%d, lump %08x, %d parsed candidate%s, %d matched, %d ambiguous, %d unmatched, %d fallback, "
		"%d ambiguous fallback, %d rejected, %d retained source%s, %u CPU bytes, %.3f ms CPU, %s\n",
		EMISSIVE_ENTITY_FIXTURE_TABLE_VERSION, emissive_entity_lump_hash, num_emissive_entity_candidates_parsed,
		num_emissive_entity_candidates_parsed == 1 ? "" : "s", num_emissive_entity_candidates_matched, num_emissive_entity_candidates_ambiguous,
		num_emissive_entity_candidates_unmatched, num_emissive_entity_fallback_sources, num_emissive_entity_ambiguous_fallbacks,
		num_emissive_entity_rejected_sources, num_emissive_entity_sources, num_emissive_entity_sources == 1 ? "" : "s",
		(unsigned)(num_emissive_entity_candidates * sizeof (*emissive_entity_candidates) + num_emissive_entity_sources * sizeof (*emissive_entity_sources)),
		(double)emissive_entity_discovery_time_us / 1000.0, emissive_entity_sources_matched ? "ready" : "awaiting static entities");
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
