/*
 * r_emissive_volume.c — optional RT-emissive air-scattering receiver.
 *
 * RV1 slice: parent-exclusive module skeleton and complete source access.
 * The module latches a read-only view of both canonical source collections
 * every frame and reports counts/diagnostics. It owns no GPU resources yet,
 * rediscovers no sources, and changes no surface output.
 */

#include "quakedef.h"
#include "r_emissive_volume.h"

cvar_t r_emissive_rt_volumetrics = {"r_emissive_rt_volumetrics", "0", CVAR_NONE};
cvar_t r_emissive_rt_volumetrics_strength = {"r_emissive_rt_volumetrics_strength", "1", CVAR_NONE};
cvar_t r_emissive_rt_volumetrics_debug = {"r_emissive_rt_volumetrics_debug", "0", CVAR_NONE};

extern cvar_t r_emissive_rt;
extern cvar_t gl_fullbrights;

// Latched per-frame adapter view. These are borrowed canonical pointers:
// the module never frees, reallocates, or writes through them, and never
// outlives the frame's source publication.
static const emissive_light_t *volume_cacheable_lights;
static const float			   *volume_cacheable_modulations;
static int					   volume_num_cacheable;
static const emissive_light_t *volume_transient_lights;
static int					   volume_num_transient;
static int					   volume_cacheable_positive;
static int					   volume_transient_positive;
static double				   volume_position_checksum;
static qboolean				   volume_snapshot_valid;
static const char			  *volume_inactive_reason = "unprepared";

static float R_EmissiveVolumeClampedStrength (void)
{
	const float strength = r_emissive_rt_volumetrics_strength.value;
	if (!isfinite (strength) || strength < 0.0f)
		return 0.0f;
	return strength;
}

void R_EmissiveVolumeInit (void)
{
	Cvar_RegisterVariable (&r_emissive_rt_volumetrics);
	Cvar_RegisterVariable (&r_emissive_rt_volumetrics_strength);
	Cvar_RegisterVariable (&r_emissive_rt_volumetrics_debug);
	Cmd_AddCommand ("r_emissive_rt_volumetrics_stats", R_EmissiveVolumeStats_f);
}

void R_EmissiveVolumeNewMap (void)
{
	volume_cacheable_lights = NULL;
	volume_cacheable_modulations = NULL;
	volume_num_cacheable = 0;
	volume_transient_lights = NULL;
	volume_num_transient = 0;
	volume_cacheable_positive = 0;
	volume_transient_positive = 0;
	volume_position_checksum = 0.0;
	volume_snapshot_valid = false;
	volume_inactive_reason = "no map snapshot";
}

static void R_EmissiveVolumeCountPositive (void)
{
	int i;

	volume_cacheable_positive = 0;
	for (i = 0; i < volume_num_cacheable; ++i)
	{
		const emissive_light_t *const light = &volume_cacheable_lights[i];
		const float modulation = volume_cacheable_modulations ? volume_cacheable_modulations[i] : 1.0f;
		if (light->radius > 0.0f && light->intensity > 0.0f && modulation > 0.0f &&
			(light->color[0] > 0.0f || light->color[1] > 0.0f || light->color[2] > 0.0f))
			++volume_cacheable_positive;
	}

	volume_transient_positive = 0;
	for (i = 0; i < volume_num_transient; ++i)
	{
		const emissive_light_t *const light = &volume_transient_lights[i];
		if (light->radius > 0.0f && light->intensity > 0.0f &&
			(light->color[0] > 0.0f || light->color[1] > 0.0f || light->color[2] > 0.0f))
			++volume_transient_positive;
	}
}

// Position checksum over current proxies (both collections, canonical order).
// Moving emitters shift this value frame to frame; it is diagnostics only and
// never feeds rendering, so its exact hash function is not load-bearing.
static void R_EmissiveVolumeChecksumPositions (void)
{
	int	   i;
	int	   axis;
	double checksum = 0.0;

	for (i = 0; i < volume_num_cacheable; ++i)
		for (axis = 0; axis < 3; ++axis)
			checksum += (double)volume_cacheable_lights[i].origin[axis] * (double)(i + 1) * (double)(axis + 1);
	for (i = 0; i < volume_num_transient; ++i)
		for (axis = 0; axis < 3; ++axis)
			checksum += (double)volume_transient_lights[i].origin[axis] * (double)(volume_num_cacheable + i + 1) * (double)(axis + 1);
	volume_position_checksum = checksum;
}

void R_EmissiveVolumePrepare (void)
{
	R_EmissiveVolumeSourceView (&volume_cacheable_lights, &volume_cacheable_modulations, &volume_num_cacheable, &volume_transient_lights,
		&volume_num_transient);
	volume_snapshot_valid = true;
	R_EmissiveVolumeCountPositive ();
	R_EmissiveVolumeChecksumPositions ();

	if (r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f)
		volume_inactive_reason = "parent RT emissives disabled";
	else if (r_emissive_rt_volumetrics.value <= 0.0f)
		volume_inactive_reason = "volumetrics not requested";
	else if (R_EmissiveVolumeClampedStrength () <= 0.0f)
		volume_inactive_reason = "strength is zero";
	else
		volume_inactive_reason = "ready";
}

qboolean R_EmissiveVolumeActive (void)
{
	if (!volume_snapshot_valid)
		return false;
	if (r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f)
		return false;
	if (r_emissive_rt_volumetrics.value <= 0.0f)
		return false;
	if (R_EmissiveVolumeClampedStrength () <= 0.0f)
		return false;
	return true;
}

void R_EmissiveVolumeStats_f (void)
{
	const int debug_mode = (int)r_emissive_rt_volumetrics_debug.value;

	Con_Printf ("RT emissive volumetrics: %s (%s)\n", R_EmissiveVolumeActive () ? "active" : "inactive", volume_inactive_reason);
	Con_Printf ("   requested %g, strength %g (effective %g), debug %d\n", r_emissive_rt_volumetrics.value,
		r_emissive_rt_volumetrics_strength.value, R_EmissiveVolumeClampedStrength (), debug_mode);
	Con_Printf ("   cacheable sources: %d total, %d positive-radiance\n", volume_num_cacheable, volume_cacheable_positive);
	Con_Printf ("   transient sources: %d total, %d positive-radiance\n", volume_num_transient, volume_transient_positive);
	Con_Printf ("   eligible sources: %d, proxy checksum %.3f\n", volume_num_cacheable + volume_num_transient, volume_position_checksum);
	if (!volume_snapshot_valid)
		Con_Printf ("   no latched snapshot yet (prepare has not run this map)\n");
}
