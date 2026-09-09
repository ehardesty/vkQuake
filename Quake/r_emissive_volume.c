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
extern cvar_t r_emissive_rt_debug;
extern cvar_t gl_farclip;
extern float r_fovx, r_fovy;

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
static uint32_t				   volume_prepare_cpu_us;
// Immutable Tier-0 world AS borrowed from the parent (never owned here).
// World-only visibility for both collections: moving doors, brush housings,
// and monsters never shadow the air; fixed world housings do.
static VkAccelerationStructureKHR volume_world_tlas = VK_NULL_HANDLE;
static uint64_t				   volume_evaluated_pairs_estimate;

// RV2 resource state (defined below; forward-declared for NewMap ordering).
static void R_EmissiveVolumeTeardownResources (void);
static void R_EmissiveVolumeEnsureResources (void);
static void R_EmissiveVolumeResourceStats (void);
static void R_EmissiveVolumeRefreshComputeSet (int slot_index);
static void R_EmissiveVolumePushWorldAS (cb_context_t *cbx);
static void R_EmissiveVolumeRecordBarriers (cb_context_t *cbx, int slot_index);
static void R_EmissiveVolumePublishSlot (cb_context_t *cbx, int slot_index);

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
	R_EmissiveVolumeTeardownResources ();
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
	volume_world_tlas = VK_NULL_HANDLE;
	volume_prepare_cpu_us = 0;
	volume_evaluated_pairs_estimate = 0;
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
	const double prepare_start = Sys_DoubleTime ();

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
	else if (R_EmissiveBandlimitActive ())
		volume_inactive_reason = "bandlimit combination unsupported (zero addition)";
	else if (volume_cacheable_positive + volume_transient_positive <= 0)
		volume_inactive_reason = "no positive-radiance source (zero addition, no work)";
	else
		volume_inactive_reason = "ready";
	R_EmissiveVolumeWorldAS (&volume_world_tlas);
	if (R_EmissiveVolumeActive () && R_EmissiveVolumeShadowed () && volume_world_tlas == VK_NULL_HANDLE)
		volume_inactive_reason = "shadowed unavailable (world AS not resident)";

	// Resource creation lives here (not in Update) because before_mark is
	// CPU-ordered before every draw and update task, so freshly allocated
	// descriptor sets are safe to bind the same frame. First-frame draws may
	// sample the just-cleared zero volume; that is correct zero addition.
	R_EmissiveVolumeEnsureResources ();

	volume_prepare_cpu_us = (uint32_t)((Sys_DoubleTime () - prepare_start) * 1000000.0);
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
	R_EmissiveVolumeResourceStats ();
}

/*
=============================================================================
RV2: fused view-volume generation + opaque-world composition. One small
camera-aligned 3D grid per in-flight frame slot (DOUBLE_BUFFERED == 2).
Each compute invocation integrates one XY column through depth over every
eligible canonical emitter (unshadowed diagnostic in RV2) and stores
cumulative scattering at depth boundaries. Fragments sample the current
slot at their own forward depth after ordinary fog. Disabled or zero-source
frames select the original pipelines and bind nothing.
=============================================================================
*/

#define EMISSIVE_VOLUME_SLOTS 2
#define EMISSIVE_VOLUME_BASE_NX 128
#define EMISSIVE_VOLUME_SEGMENTS 64
#define EMISSIVE_VOLUME_BOUNDARIES (EMISSIVE_VOLUME_SEGMENTS + 1)
#define EMISSIVE_VOLUME_REFERENCE_LENGTH 256.0f
#define EMISSIVE_VOLUME_MEMORY_BUDGET_MB 64
// Must match the local_size_x/y in Shaders/emissive_volume.comp.
#define EMISSIVE_VOLUME_LOCAL_SIZE 8

typedef struct emissive_volume_slot_s
{
	VkImage image;
	VkImageView view;
	vulkan_memory_t memory;
	VkDescriptorSet compute_set;
	VkDescriptorSet fragment_set;
} emissive_volume_slot_t;

static emissive_volume_slot_t volume_slots[EMISSIVE_VOLUME_SLOTS];
static VkBuffer volume_dummy_buffer = VK_NULL_HANDLE;
static vulkan_memory_t volume_dummy_memory;
static int volume_slot;
static int volume_nx;
static int volume_ny;
static float volume_zmax;
static float volume_viewport[4];
static qboolean volume_resources_valid;
static const char *volume_resource_reason = "never prepared";
static qboolean volume_slot_initialized[EMISSIVE_VOLUME_SLOTS];
#define EMISSIVE_VOLUME_OCCLUDER_MASK 0x01u

static void R_EmissiveVolumeDestroySlot (emissive_volume_slot_t *slot)
{
	if (slot->compute_set != VK_NULL_HANDLE)
	{
		R_FreeDescriptorSet (slot->compute_set, &vulkan_globals.emissive_volume_set_layout);
		slot->compute_set = VK_NULL_HANDLE;
	}
	if (slot->fragment_set != VK_NULL_HANDLE)
	{
		R_FreeDescriptorSet (slot->fragment_set, &vulkan_globals.single_texture_set_layout);
		slot->fragment_set = VK_NULL_HANDLE;
	}
	if (slot->view != VK_NULL_HANDLE)
	{
		vkDestroyImageView (vulkan_globals.device, slot->view, NULL);
		slot->view = VK_NULL_HANDLE;
	}
	if (slot->image != VK_NULL_HANDLE)
	{
		vkDestroyImage (vulkan_globals.device, slot->image, NULL);
		slot->image = VK_NULL_HANDLE;
	}
	R_FreeVulkanMemory (&slot->memory, &num_vulkan_bmodel_allocations);
}

static void R_EmissiveVolumeTeardownResources (void)
{
	int i;
	if (!volume_resources_valid && volume_dummy_buffer == VK_NULL_HANDLE)
		return;
	GL_WaitForDeviceIdle ();
	for (i = 0; i < EMISSIVE_VOLUME_SLOTS; ++i)
		R_EmissiveVolumeDestroySlot (&volume_slots[i]);
	R_FreeBuffer (volume_dummy_buffer, &volume_dummy_memory, &num_vulkan_bmodel_allocations);
	volume_dummy_buffer = VK_NULL_HANDLE;
	volume_resources_valid = false;
	volume_resource_reason = "released";
}

static qboolean R_EmissiveVolumeCreateSlot (emissive_volume_slot_t *slot, int nx, int ny)
{
	VkResult err;
	memset (slot, 0, sizeof (*slot));
	ZEROED_STRUCT (VkImageCreateInfo, image_info);
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_3D;
	image_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	image_info.extent.width = (uint32_t)nx;
	image_info.extent.height = (uint32_t)ny;
	image_info.extent.depth = EMISSIVE_VOLUME_BOUNDARIES;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	err = vkCreateImage (vulkan_globals.device, &image_info, NULL, &slot->image);
	if (err != VK_SUCCESS)
		return false;
	ZEROED_STRUCT (VkMemoryRequirements, requirements);
	vkGetImageMemoryRequirements (vulkan_globals.device, slot->image, &requirements);
	if (requirements.size > (VkDeviceSize)EMISSIVE_VOLUME_MEMORY_BUDGET_MB * 1024 * 1024)
		return false;
	ZEROED_STRUCT (VkMemoryAllocateInfo, allocate_info);
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize = requirements.size;
	allocate_info.memoryTypeIndex =
		GL_MemoryTypeFromProperties (requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
	memset (&slot->memory, 0, sizeof (slot->memory));
	R_AllocateVulkanMemory (&slot->memory, &allocate_info, VULKAN_MEMORY_TYPE_DEVICE, &num_vulkan_bmodel_allocations);
	err = vkBindImageMemory (vulkan_globals.device, slot->image, slot->memory.handle, 0);
	if (err != VK_SUCCESS)
		return false;
	ZEROED_STRUCT (VkImageViewCreateInfo, view_info);
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = slot->image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
	view_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	err = vkCreateImageView (vulkan_globals.device, &view_info, NULL, &slot->view);
	if (err != VK_SUCCESS)
		return false;
	slot->compute_set = R_AllocateDescriptorSet (&vulkan_globals.emissive_volume_set_layout);
	slot->fragment_set = R_AllocateDescriptorSet (&vulkan_globals.single_texture_set_layout);
	ZEROED_STRUCT (VkDescriptorImageInfo, fragment_image);
	fragment_image.sampler = vulkan_globals.point_sampler;
	fragment_image.imageView = slot->view;
	fragment_image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	ZEROED_STRUCT (VkWriteDescriptorSet, fragment_write);
	fragment_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	fragment_write.dstSet = slot->fragment_set;
	fragment_write.descriptorCount = 1;
	fragment_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	fragment_write.pImageInfo = &fragment_image;
	vkUpdateDescriptorSets (vulkan_globals.device, 1, &fragment_write, 0, NULL);
	return true;
}

static void R_EmissiveVolumeEnsureResources (void)
{
	int wanted_nx;
	int wanted_ny;
	int i;
	if (!R_EmissiveVolumeActive () || volume_cacheable_positive + volume_transient_positive <= 0 || R_EmissiveBandlimitActive ())
	{
		if (volume_resources_valid)
			R_EmissiveVolumeTeardownResources ();
		else
			volume_resource_reason = "inactive (released)";
		return;
	}
	if (r_refdef.vrect.height <= 0 || r_refdef.vrect.width <= 0)
	{
		volume_resource_reason = "degenerate viewport";
		return;
	}
	wanted_nx = EMISSIVE_VOLUME_BASE_NX;
	wanted_ny = (int)(EMISSIVE_VOLUME_BASE_NX * ((float)r_refdef.vrect.height / (float)r_refdef.vrect.width) + 0.5f);
	wanted_ny = CLAMP (32, wanted_ny, 128);
	volume_slot = R_EmissiveVolumeFrameSlot () % EMISSIVE_VOLUME_SLOTS;
	if (volume_slot < 0 || volume_slot >= EMISSIVE_VOLUME_SLOTS)
		volume_slot = 0;
	if (volume_resources_valid && (wanted_nx != volume_nx || wanted_ny != volume_ny))
		R_EmissiveVolumeTeardownResources ();
	if (!volume_resources_valid)
	{
		for (i = 0; i < EMISSIVE_VOLUME_SLOTS; ++i)
			if (!R_EmissiveVolumeCreateSlot (&volume_slots[i], wanted_nx, wanted_ny))
			{
				R_EmissiveVolumeTeardownResources ();
				volume_resource_reason = "allocation rejected (zero addition)";
				return;
			}
		R_CreateBuffer (
			&volume_dummy_buffer, &volume_dummy_memory, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
			&num_vulkan_bmodel_allocations, NULL, "emissive volume dummy");
		volume_nx = wanted_nx;
		volume_ny = wanted_ny;
		volume_resources_valid = true;
		for (i = 0; i < EMISSIVE_VOLUME_SLOTS; ++i)
			volume_slot_initialized[i] = false;
	}
	R_CreateEmissiveVolumePipelines ();
	volume_viewport[0] = (float)r_refdef.vrect.x;
	volume_viewport[1] = (float)(vid.height - ((glheight - r_refdef.vrect.y - r_refdef.vrect.height) + r_refdef.vrect.height));
	volume_viewport[2] = (float)r_refdef.vrect.width;
	volume_viewport[3] = (float)r_refdef.vrect.height;
	volume_zmax = q_max (gl_farclip.value, 1.0f);
	volume_evaluated_pairs_estimate =
		(uint64_t)volume_nx * (uint64_t)volume_ny * EMISSIVE_VOLUME_SEGMENTS * (uint64_t)(volume_num_cacheable + volume_num_transient);
	volume_resource_reason = "ready";
}

qboolean R_EmissiveVolumeReady (void)
{
	if (!volume_snapshot_valid || !R_EmissiveVolumeActive ())
		return false;
	if (volume_cacheable_positive + volume_transient_positive <= 0)
		return false;
	if (!volume_resources_valid)
		return false;
	if (vulkan_globals.emissive_volume_pipeline.handle == VK_NULL_HANDLE)
		return false;
	if (R_EmissiveBandlimitActive ())
		return false;
	// Shadowed modes never fall back silently: without a resident world AS
	// (or its pipeline) they expose zero addition and report the reason.
	if (R_EmissiveVolumeShadowed () &&
		(volume_world_tlas == VK_NULL_HANDLE || vulkan_globals.emissive_volume_shadow_pipeline.handle == VK_NULL_HANDLE))
		return false;
	return true;
}

qboolean R_EmissiveVolumeScatterOnly (void)
{
	const int debug_mode = (int)r_emissive_rt_volumetrics_debug.value;
	return debug_mode == 1 || debug_mode == 3;
}

qboolean R_EmissiveVolumeShadowed (void)
{
	const int debug_mode = (int)r_emissive_rt_volumetrics_debug.value;
	return debug_mode == 0 || debug_mode == 1;
}

vulkan_pipeline_t R_EmissiveVolumeWorldPipeline (int variant, int pipeline_index, qboolean scatter_only)
{
	if (variant < 0 || variant >= MAIN_RENDER_PASS_VARIANT_COUNT || pipeline_index < 0 || pipeline_index >= WORLD_PIPELINE_COUNT)
	{
		vulkan_pipeline_t null_pipeline;
		memset (&null_pipeline, 0, sizeof (null_pipeline));
		return null_pipeline;
	}
	return vulkan_globals.world_volume_pipelines[variant][pipeline_index][scatter_only ? 1 : 0];
}

void R_EmissiveVolumeUpdate (struct cb_context_s *cbx)
{
	emissive_volume_push_t constants;
	if (!R_EmissiveVolumeReady ())
		return;
	R_BeginDebugUtilsLabel (cbx, "Emissive Volume");
	R_EmissiveVolumeRefreshComputeSet (volume_slot);
	R_EmissiveVolumeRecordBarriers (cbx, volume_slot);
	memset (&constants, 0, sizeof (constants));
	constants.camera_origin_zmax[0] = r_refdef.vieworg[0];
	constants.camera_origin_zmax[1] = r_refdef.vieworg[1];
	constants.camera_origin_zmax[2] = r_refdef.vieworg[2];
	constants.camera_origin_zmax[3] = volume_zmax;
	constants.forward_tanx[0] = vpn[0];
	constants.forward_tanx[1] = vpn[1];
	constants.forward_tanx[2] = vpn[2];
	constants.forward_tanx[3] = tanf (DEG2RAD (r_fovx) * 0.5f);
	constants.right_tany[0] = vright[0];
	constants.right_tany[1] = vright[1];
	constants.right_tany[2] = vright[2];
	constants.right_tany[3] = tanf (DEG2RAD (r_fovy) * 0.5f);
	constants.up_fogdensity[0] = vup[0];
	constants.up_fogdensity[1] = vup[1];
	constants.up_fogdensity[2] = vup[2];
	constants.up_fogdensity[3] = Fog_GetDensity () / 64.0f;
	constants.counts[0] = (uint32_t)volume_num_cacheable;
	constants.counts[1] = (uint32_t)volume_num_transient;
	constants.counts[2] = EMISSIVE_VOLUME_SEGMENTS;
	constants.counts[3] = 0;
	constants.extra[0] = 0;
	constants.extra[1] = EMISSIVE_VOLUME_OCCLUDER_MASK;
	constants.extra[2] = 0;
	constants.extra[3] = 0;
	constants.params[0] = R_EmissiveVolumeClampedStrength ();
	constants.params[1] = (float)volume_nx;
	constants.params[2] = (float)volume_ny;
	constants.params[3] = EMISSIVE_VOLUME_REFERENCE_LENGTH;
	if (R_EmissiveVolumeShadowed ())
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, vulkan_globals.emissive_volume_shadow_pipeline);
	else
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, vulkan_globals.emissive_volume_pipeline);
	vulkan_globals.vk_cmd_bind_descriptor_sets (
		cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, cbx->current_pipeline.layout.handle, 0, 1, &volume_slots[volume_slot].compute_set, 0, NULL);
	if (R_EmissiveVolumeShadowed ())
		R_EmissiveVolumePushWorldAS (cbx);
	R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
	vkCmdDispatch (
		cbx->cb, (uint32_t)((volume_nx + EMISSIVE_VOLUME_LOCAL_SIZE - 1) / EMISSIVE_VOLUME_LOCAL_SIZE),
		(uint32_t)((volume_ny + EMISSIVE_VOLUME_LOCAL_SIZE - 1) / EMISSIVE_VOLUME_LOCAL_SIZE), 1);
	R_EmissiveVolumePublishSlot (cbx, volume_slot);
	R_EndDebugUtilsLabel (cbx);
}

static void R_EmissiveVolumePushWorldAS (cb_context_t *cbx)
{
	ZEROED_STRUCT (VkWriteDescriptorSetAccelerationStructureKHR, tlas_info);
	tlas_info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	tlas_info.accelerationStructureCount = 1;
	tlas_info.pAccelerationStructures = &volume_world_tlas;
	ZEROED_STRUCT (VkWriteDescriptorSet, tlas_write);
	tlas_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	tlas_write.pNext = &tlas_info;
	tlas_write.dstBinding = 0;
	tlas_write.descriptorCount = 1;
	tlas_write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	vulkan_globals.vk_cmd_push_descriptor_set (
		cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, cbx->current_pipeline.layout.handle, 1, 1, &tlas_write);
}

static void R_EmissiveVolumeRefreshComputeSet (int slot_index)
{
	emissive_volume_slot_t *const slot = &volume_slots[slot_index];
	VkDescriptorBufferInfo buffers[3];
	VkDescriptorImageInfo storage_image;
	VkWriteDescriptorSet writes[4];
	VkBuffer parent_buffers[3];
	int i;
	R_EmissiveVolumeParentBuffers (&parent_buffers[0], &parent_buffers[1], &parent_buffers[2]);
	for (i = 0; i < 3; ++i)
		buffers[i].buffer = parent_buffers[i] != VK_NULL_HANDLE ? parent_buffers[i] : volume_dummy_buffer;
	for (i = 0; i < 3; ++i)
	{
		buffers[i].offset = 0;
		buffers[i].range = VK_WHOLE_SIZE;
	}
	memset (&storage_image, 0, sizeof (storage_image));
	storage_image.imageView = slot->view;
	storage_image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset (writes, 0, sizeof (writes));
	for (i = 0; i < 4; ++i)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = slot->compute_set;
		writes[i].dstBinding = (uint32_t)i;
		writes[i].descriptorCount = 1;
	}
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[0].pImageInfo = &storage_image;
	for (i = 1; i < 4; ++i)
	{
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[i].pBufferInfo = &buffers[i - 1];
	}
	vkUpdateDescriptorSets (vulkan_globals.device, 4, writes, 0, NULL);
}

static void R_EmissiveVolumeImageBarrier (
	cb_context_t *cbx, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
	VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
	ZEROED_STRUCT (VkImageMemoryBarrier, barrier);
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = src_access;
	barrier.dstAccessMask = dst_access;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier (cbx->cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void R_EmissiveVolumeRecordBarriers (cb_context_t *cbx, int slot_index)
{
	emissive_volume_slot_t *const slot = &volume_slots[slot_index];
	VkBufferMemoryBarrier buffer_barriers[3];
	VkBuffer parent_buffers[3];
	VkAccessFlags image_src_access;
	int i;
	if (volume_slot_initialized[slot_index])
		image_src_access = VK_ACCESS_SHADER_READ_BIT;
	else
	{
		ZEROED_STRUCT (VkImageSubresourceRange, range);
		ZEROED_STRUCT (VkClearColorValue, clear);
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.levelCount = 1;
		range.layerCount = 1;
		R_EmissiveVolumeImageBarrier (
			cbx, slot->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkCmdClearColorImage (cbx->cb, slot->image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
		volume_slot_initialized[slot_index] = true;
		image_src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
	}
	R_EmissiveVolumeParentBuffers (&parent_buffers[0], &parent_buffers[1], &parent_buffers[2]);
	memset (buffer_barriers, 0, sizeof (buffer_barriers));
	for (i = 0; i < 3; ++i)
	{
		buffer_barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		buffer_barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		buffer_barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		buffer_barriers[i].srcQueueFamilyIndex = buffer_barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		buffer_barriers[i].buffer = parent_buffers[i] != VK_NULL_HANDLE ? parent_buffers[i] : volume_dummy_buffer;
		buffer_barriers[i].offset = 0;
		buffer_barriers[i].size = VK_WHOLE_SIZE;
	}
	R_EmissiveVolumeImageBarrier (
		cbx, slot->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, image_src_access, VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	vkCmdPipelineBarrier (
		cbx->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 3, buffer_barriers, 0, NULL);
}

VkDescriptorSet R_EmissiveVolumeFragmentSet (void)
{
	if (!volume_resources_valid || volume_slot < 0 || volume_slot >= EMISSIVE_VOLUME_SLOTS)
		return VK_NULL_HANDLE;
	return volume_slots[volume_slot].fragment_set;
}

static void R_EmissiveVolumePublishSlot (cb_context_t *cbx, int slot_index)
{
	R_EmissiveVolumeImageBarrier (
		cbx, volume_slots[slot_index].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

static void R_EmissiveVolumeResourceStats (void)
{
	uint64_t logical_bytes;
	uint64_t allocated_bytes;
	if (!volume_resources_valid)
	{
		Con_Printf ("   volume resources: %s\n", volume_resource_reason);
		return;
	}
	logical_bytes = (uint64_t)volume_nx * (uint64_t)volume_ny * EMISSIVE_VOLUME_BOUNDARIES * 8 * EMISSIVE_VOLUME_SLOTS;
	allocated_bytes = volume_slots[0].memory.size + volume_slots[1].memory.size + volume_dummy_memory.size;
	Con_Printf ("   volume grid: %dx%dx%d, extent %.0f, %s\n", volume_nx, volume_ny, EMISSIVE_VOLUME_SEGMENTS, volume_zmax,
		R_EmissiveVolumeShadowed () ? "world-shadowed" : "unshadowed diagnostic");
	Con_Printf (
		"   volume shadows: world AS %s, mask 0x%02x (world-only; moving occluders excluded)\n",
		volume_world_tlas != VK_NULL_HANDLE ? "resident" : "unavailable", EMISSIVE_VOLUME_OCCLUDER_MASK);
	Con_Printf ("   volume resources: %s, prepare %u us\n", volume_resource_reason, volume_prepare_cpu_us);
	Con_Printf ("   volume memory: %llu logical, %llu allocated bytes\n", (unsigned long long)logical_bytes, (unsigned long long)allocated_bytes);
	Con_Printf (
		"   volume work: %llu scheduled source evaluations (estimate; bounds shadowed visibility queries)\n",
		(unsigned long long)volume_evaluated_pairs_estimate);
}

void R_EmissiveVolumeFragmentPush (float out_viewport_zmax[5])
{
	out_viewport_zmax[0] = volume_viewport[0];
	out_viewport_zmax[1] = volume_viewport[1];
	out_viewport_zmax[2] = volume_viewport[2];
	out_viewport_zmax[3] = volume_viewport[3];
	out_viewport_zmax[4] = volume_zmax;
}
