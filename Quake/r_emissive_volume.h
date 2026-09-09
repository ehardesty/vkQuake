/*
 * r_emissive_volume.h — optional RT-emissive air-scattering receiver.
 *
 * RV0 integration note (verified against rt_emissives HEAD, reference
 * snapshot 50800831; no prior-attempt volume code was present, so there was
 * nothing to preserve):
 *
 * - Canonical cacheable sources live in r_brush.c as
 *   emissive_cacheable_lights[num_emissive_lights] with per-source modulation
 *   in emissive_light_modulations[] (1.0 when the source style is 255,
 *   otherwise d_lightstylevalue[style]/256). They are published by
 *   R_SetEmissiveLights at map load; R_UpdateEmissiveLightstyles refreshes
 *   modulation every frame. The stored intensity is PRE-modulation: the
 *   volume adapter must multiply by the matching modulation exactly once.
 * - Canonical transient sources live in r_brush.c as
 *   transient_emissive_lights[num_transient_emissive_lights], republished by
 *   R_SetTransientEmissiveLights from R_UpdateTransientEmissiveSources every
 *   rendered frame. Transient evaluation in r_brush.c uses modulation 1.0
 *   (three sites) and neither inline-brush nor curated-entity construction
 *   applies a lightstyle, so transient intensity is FINAL: the adapter must
 *   not modulate it again.
 * - Both collections already contain current moving proxies (inline-brush
 *   luminous centroids, curated-entity alias origins, generalized model
 *   emitters). Fixed-world fixtures owned by an inline brush are suppressed
 *   at the parent, so reading both collections introduces no duplicate owner.
 * - Frame order in R_RenderView is: R_UpdateTransientEmissiveSources, then
 *   R_SetupViewBeforeMark (R_UpdateEmissiveLightstyles,
 *   R_LatchEmissiveResolvedTextures, view latch), then the build-TLAS task,
 *   then R_UpdateLightmapsAndIndirect (emissive coarse/detail/radiance/
 *   transient/brush/bounce). The volume Prepare hook sits at the end of
 *   R_SetupViewBeforeMark so counts, modulation, and view are all current.
 * - Shadowed operation borrows the immutable world AS through
 *   R_EmissiveDirectAccelerationStructure with R_EmissiveOccluderMask
 *   (both file-local in r_brush.c today; a narrow accessor arrives with the
 *   RV3 slice that first needs them).
 * - World graphics use one shared 8-set pipeline layout (single/single/
 *   single/MBOIT/bmodel-instances/single/single/single). Sets 5, 6, 7 back
 *   the emissive coarse/detail/surface-index bindings; set 7 is unbound in
 *   every non-bandlimit variant, so the volume sampler reuses set 7 with the
 *   existing layout unchanged. Bandlimit+volume therefore stays an explicit
 *   unsupported combination (volume reports unavailable) rather than a
 *   silent layout collision.
 * - Shader fog convention (world_common.inc + Fog_SetupFrame): the pushed
 *   fog_density is Fog_GetDensity()/64 and transmittance is
 *   exp(-(density*z)^2) with z = in_fog_frag_coord = gl_Position.w, i.e. the
 *   forward view depth the volume integrates along. At fog 0 the weight is
 *   exactly 1 and can never gate scattering.
 * - Camera rays use the tested helpers only: origin r_refdef.vieworg, basis
 *   vpn/vright/vup from AngleVectors, half-angles from r_fovx/r_fovy, far
 *   extent gl_farclip.value, viewport r_refdef.vrect. Vulkan framebuffer
 *   y grows downward and matches gl_FragCoord, so column (ix,iy) maps to
 *   u = (ix+0.5)/nx rightward and v = (iy+0.5)/ny downward with
 *   q = vpn + 2*(u-0.5)*tan(fovx/2)*vright - 2*(v-0.5)*tan(fovy/2)*vup.
 *
 * Module shape (codebase-design): r_emissive_volume is a deep module. Its
 * interface is the handful of R_EmissiveVolume* functions below; everything
 * else (resources, descriptors, lists, compute recording, composition
 * policy) stays inside the implementation plus its two shaders. Callers and
 * the stats command cross the same seam, so diagnostics observe exactly what
 * the renderer consumes.
 */

#ifndef R_EMISSIVE_VOLUME_H
#define R_EMISSIVE_VOLUME_H

#include "q_stdinc.h"
#include "cvar.h"

extern cvar_t r_emissive_rt_volumetrics;
extern cvar_t r_emissive_rt_volumetrics_strength;
extern cvar_t r_emissive_rt_volumetrics_debug;

// Requested (cvar) state. No resources are owned in this state.
void R_EmissiveVolumeInit (void);
// Reset latched map-lifetime state. Never allocates; safe with no map.
void R_EmissiveVolumeNewMap (void);
// Latch the current canonical source view, modulation, and parameters.
// Must run after R_UpdateEmissiveLightstyles each frame; performs no GPU work.
void R_EmissiveVolumePrepare (void);
// Effective predicate: parent emissives on, volume requested, strength > 0,
// and a valid latched snapshot. Map fog is never part of this predicate.
qboolean R_EmissiveVolumeActive (void);
// Console report for the volume adapter state (RV1: counts and state;
// resource/GPU/ray counters join in their slices).
void R_EmissiveVolumeStats_f (void);

// --- RV2: fused view-volume generation + opaque-world composition ---
struct cb_context_s;
// Record volume generation into the update command buffer. Must run inside
// R_UpdateLightmapsAndIndirect after the transient updates so the reused
// parent buffers and modulation uploads are current for this frame.
void R_EmissiveVolumeUpdate (struct cb_context_s *cbx);
// Draw-time predicate for scene fragments: active snapshot, positive
// radiance available, and this frame's volume resources valid. Draw-local
// policy (alpha blend, debug pipelines, render pass) stays with the caller.
qboolean R_EmissiveVolumeReady (void);
// Scatter-only fragment selection from the volume debug mode.
qboolean R_EmissiveVolumeScatterOnly (void);
// Fragment push values: viewport rect (framebuffer px, y down) + z extent.
void R_EmissiveVolumeFragmentPush (float out_viewport_zmax[5]);

#endif // R_EMISSIVE_VOLUME_H
