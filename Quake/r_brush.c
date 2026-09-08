/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2016 Axel Gneiting

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
// r_brush.c: brush model rendering. renamed from r_surf.c

#include "quakedef.h"
#include "gl_heap.h"

extern cvar_t gl_fullbrights, r_drawflat, r_gpulightmapupdate, r_rtshadows;
extern cvar_t r_emissive_rt, r_emissive_rt_resolution, r_emissive_rt_occluders, r_emissive_rt_external_bsp, r_emissive_rt_liquid_receivers,
	r_emissive_rt_translucent_receivers, r_emissive_rt_debug, r_emissive_rt_bandlimit, r_emissive_rt_bounce, r_emissive_rt_bounce_strength,
	r_emissive_rt_bounce_reflectance, r_emissive_rt_bounce_rays, r_emissive_rt_bounce_resolution, r_emissive_rt_model_lights;

int gl_lightmap_format;

#define SHELF_HEIGHT 256
#define SHELVES		 (LMBLOCK_HEIGHT / SHELF_HEIGHT)

#define LM_BIN_E 8
#define LM_BINS	 49
#define EMISSIVE_DETAIL_MEMORY_BUDGET_MB 512
#define EMISSIVE_VISIBILITY_MEMORY_BUDGET_MB 128
#define EMISSIVE_BRUSH_RECEIVER_MEMORY_BUDGET_MB 256

enum
{
	EMISSIVE_PUBLICATION_UPDATE,
	EMISSIVE_PUBLICATION_INVALIDATE,
	EMISSIVE_PUBLICATION_NO_VISIBILITY
};

struct lightmap_s *lightmaps;
int				   lightmap_count;
int				   last_lightmap_allocated;
int				   used_columns[MAX_SANITY_LIGHTMAPS][SHELVES];
int				   lightmap_idx[LM_BINS];
int				   shelf_idx[LM_BINS];
int				   columns[LM_BINS];
int				   rows[LM_BINS];

/* Lightmap extents are usually <= 18 with the default qbsp -subdivide of 240. The check in CalcSurfaceExtents ()
   limits them to 126 x 126 on load. The lightmap packer and the blocklights array can handle up to 256 x 256. */

unsigned blocklights[256 * 256 * 3 + 1]; // johnfitz -- was 18*18, added lit support (*3) and loosened surface extents maximum

qboolean indirect = true;
qboolean indirect_ready = false;

typedef struct
{
	texture_t *texture;
	msurface_t *surface; // non-NULL for independently lit liquid draws
	short	   lightmap_idx;
	byte	   is_bmodel; // for gl_zfix
	byte	   world_flags;
	int		   max_indices;
} indirectdraw_t;

#define INDIRECT_WORLD_MODEL		1
#define INDIRECT_EMISSIVE_INFLUENCE 2
COMPILE_TIME_ASSERT (indirectdraw_t, sizeof (indirectdraw_t) == 24);

#define MAX_INDIRECT_DRAWS 32768
static indirectdraw_t indirect_draws[MAX_INDIRECT_DRAWS];
static int			  used_indirect_draws = 0;
static uint32_t		  indirect_bmodel_start;
static qboolean		  indirect_emissive_grouping;

#define INDIRECT_ZBIAS 1 // suport gl_zfix for nontransformed models. Costs extra indirect drawcalls
extern cvar_t gl_zfix;

extern cvar_t vid_filter;
extern cvar_t vid_palettize;

static VkDrawIndexedIndirectCommand initial_indirect_buffer[MAX_INDIRECT_DRAWS];

#define TLAS_SIZE_MULTIPLE 1024

typedef union
{
	struct // 1st element contains this
	{
		int water_count;
		int lm_count;
	};
	atomic_uint32_t *update_warp; // next water_count elements contain this
	struct						  // last lm_count elements contain this
	{
		int		 lightmap_num;
		uint32_t lightmap_styles;
	};
} combined_brush_deps;

static combined_brush_deps *brush_deps_data;
static int					used_deps_data = 0;
#define INITIAL_BRUSH_DEPS_SIZE 16384

static vulkan_memory_t	   bmodel_memory;
VkBuffer				   bmodel_vertex_buffer;
uint32_t				   bmodel_numverts;
VkDeviceAddress			   bmodel_vertex_buffer_device_address;
VkAccelerationStructureKHR bmodel_tlas = VK_NULL_HANDLE;
static VkBuffer			   bmodel_tlas_buffer;
static size_t			   bmodel_tlas_size;
static vulkan_memory_t	   bmodel_tlas_device_memory;
static uint32_t			   bmodel_tlas_max_instances = TLAS_SIZE_MULTIPLE;
static VkBuffer			   bmodel_indices_buffer;
static VkDeviceAddress	   bmodel_indices_device_address;
static vulkan_memory_t	   bmodel_as_device_memory;
uint32_t				   rs_live_as_cputime_us;
static uint32_t			   live_as_instance_count;

static VkAccelerationStructureKHR emissive_world_blas;
static VkAccelerationStructureKHR emissive_world_tlas;
static VkBuffer					  emissive_world_indices_buffer;
static VkBuffer					  emissive_world_primitive_surfaces_buffer;
static VkBuffer					  emissive_world_instances_buffer;
static VkBuffer					  emissive_world_blas_buffer;
static VkBuffer					  emissive_world_tlas_buffer;
static VkDeviceAddress			  emissive_world_indices_address;
static VkDeviceAddress			  emissive_world_instances_address;
static VkDeviceAddress			  emissive_world_blas_buffer_address;
static VkDeviceAddress			  emissive_world_blas_address;
static vulkan_memory_t			  emissive_world_as_memory;
static uint64_t					  emissive_world_as_bytes;
static uint32_t					  emissive_world_as_triangles;
static uint32_t					  emissive_world_as_build_time_us;
static qboolean					  emissive_world_as_build_time_valid;

#define TLAS_GARBAGE_FRAME_COUNT 2
static VkAccelerationStructureKHR tlas_garbage[TLAS_GARBAGE_FRAME_COUNT];
static int						  tlas_garbage_index;

#define MIN_SCRATCH_BUFFER_SIZE_MB 8

// Shared scratch buffer for all AS operations (bmodel BLAS build, TLAS build, animated BLAS updates)
dynbuffer_t			   as_scratch_buffer;
static vulkan_memory_t as_scratch_memory;
uint32_t			   as_scratch_buffer_size;

/*
===============
R_EnsureASScratchBufferSize
===============
*/
void R_EnsureASScratchBufferSize (uint32_t required_size)
{
	if (required_size <= as_scratch_buffer_size)
		return;

	if (as_scratch_buffer.buffer != VK_NULL_HANDLE)
		R_AddDynamicBufferGarbage (as_scratch_memory, &as_scratch_buffer, 1, NULL);
	as_scratch_buffer_size = Q_nextPow2 (q_max (required_size, MIN_SCRATCH_BUFFER_SIZE_MB * 1024 * 1024));

	Sys_Printf ("Reallocating dynamic AS scratch buffer (%u KB)\n", as_scratch_buffer_size / 1024);

	VkResult err;

	ZEROED_STRUCT (VkBufferCreateInfo, buffer_create_info);
	buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_create_info.size = as_scratch_buffer_size;
	buffer_create_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
							   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;

	err = vkCreateBuffer (vulkan_globals.device, &buffer_create_info, NULL, &as_scratch_buffer.buffer);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateBuffer failed with code %i", (int)err);
	GL_SetObjectName ((uint64_t)as_scratch_buffer.buffer, VK_OBJECT_TYPE_BUFFER, "AS scratch buffer");

	VkMemoryRequirements memory_requirements;
	vkGetBufferMemoryRequirements (vulkan_globals.device, as_scratch_buffer.buffer, &memory_requirements);

	ZEROED_STRUCT (VkMemoryAllocateFlagsInfo, memory_allocate_flags_info);
	memory_allocate_flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO_KHR;
	memory_allocate_flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

	ZEROED_STRUCT (VkMemoryAllocateInfo, memory_allocate_info);
	memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	memory_allocate_info.pNext = &memory_allocate_flags_info;
	memory_allocate_info.allocationSize = memory_requirements.size;
	memory_allocate_info.memoryTypeIndex = GL_MemoryTypeFromProperties (memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);

	R_AllocateVulkanMemory (&as_scratch_memory, &memory_allocate_info, VULKAN_MEMORY_TYPE_DEVICE, &num_vulkan_dynbuf_allocations);
	GL_SetObjectName ((uint64_t)as_scratch_memory.handle, VK_OBJECT_TYPE_DEVICE_MEMORY, "AS scratch buffer");

	err = vkBindBufferMemory (vulkan_globals.device, as_scratch_buffer.buffer, as_scratch_memory.handle, 0);
	if (err != VK_SUCCESS)
		Sys_Error ("vkBindBufferMemory failed with code %i", (int)err);

	ZEROED_STRUCT (VkBufferDeviceAddressInfoKHR, buffer_device_address_info);
	buffer_device_address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR;
	buffer_device_address_info.buffer = as_scratch_buffer.buffer;
	as_scratch_buffer.device_address = vulkan_globals.vk_get_buffer_device_address (vulkan_globals.device, &buffer_device_address_info);
	as_scratch_buffer.current_offset = 0;
}

/*
===============
R_FreeASScratchBuffer
===============
*/
void R_FreeASScratchBuffer (void)
{
	if (as_scratch_buffer.buffer != VK_NULL_HANDLE)
	{
		vkDestroyBuffer (vulkan_globals.device, as_scratch_buffer.buffer, NULL);
		memset (&as_scratch_buffer, 0, sizeof (as_scratch_buffer));
		R_FreeVulkanMemory (&as_scratch_memory, &num_vulkan_dynbuf_allocations);
	}
	as_scratch_buffer_size = 0;
}

/*
===============
R_CollectTLASGarbage
===============
*/
void R_CollectTLASGarbage (void)
{
	tlas_garbage_index = (tlas_garbage_index + 1) % TLAS_GARBAGE_FRAME_COUNT;
	if (tlas_garbage[tlas_garbage_index] != VK_NULL_HANDLE)
	{
		vulkan_globals.vk_destroy_acceleration_structure (vulkan_globals.device, tlas_garbage[tlas_garbage_index], NULL);
		tlas_garbage[tlas_garbage_index] = VK_NULL_HANDLE;
	}
}

/*
===============
R_AllocateTLAS
===============
*/
static void R_AllocateTLAS (void)
{
	VkResult err;

	ZEROED_STRUCT (VkBufferCreateInfo, buffer_create_info);
	buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_create_info.size = bmodel_tlas_size;
	buffer_create_info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

	err = vkCreateBuffer (vulkan_globals.device, &buffer_create_info, NULL, &bmodel_tlas_buffer);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateBuffer failed with code %i", (int)err);
	GL_SetObjectName ((uint64_t)bmodel_tlas_buffer, VK_OBJECT_TYPE_BUFFER, "BModel TLAS");

	VkMemoryRequirements memory_requirements;
	vkGetBufferMemoryRequirements (vulkan_globals.device, bmodel_tlas_buffer, &memory_requirements);

	ZEROED_STRUCT (VkMemoryAllocateFlagsInfo, memory_allocate_flags_info);
	memory_allocate_flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO_KHR;
	memory_allocate_flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

	ZEROED_STRUCT (VkMemoryAllocateInfo, memory_allocate_info);
	memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	memory_allocate_info.pNext = &memory_allocate_flags_info;
	memory_allocate_info.allocationSize = memory_requirements.size;
	memory_allocate_info.memoryTypeIndex = GL_MemoryTypeFromProperties (memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);

	R_AllocateVulkanMemory (&bmodel_tlas_device_memory, &memory_allocate_info, VULKAN_MEMORY_TYPE_DEVICE, &num_vulkan_bmodel_allocations);
	GL_SetObjectName ((uint64_t)bmodel_tlas_device_memory.handle, VK_OBJECT_TYPE_DEVICE_MEMORY, "BModel TLAS");

	err = vkBindBufferMemory (vulkan_globals.device, bmodel_tlas_buffer, bmodel_tlas_device_memory.handle, 0);
	if (err != VK_SUCCESS)
		Sys_Error ("vkBindBufferMemory failed with code %i", (int)err);

	ZEROED_STRUCT (VkAccelerationStructureCreateInfoKHR, acceleration_structure_create_info);
	acceleration_structure_create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	acceleration_structure_create_info.buffer = bmodel_tlas_buffer;
	acceleration_structure_create_info.size = bmodel_tlas_size;
	acceleration_structure_create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	err = vulkan_globals.vk_create_acceleration_structure (vulkan_globals.device, &acceleration_structure_create_info, NULL, &bmodel_tlas);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateAccelerationStructure failed with code %i", (int)err);
}

extern cvar_t r_showtris;
extern cvar_t r_simd;
typedef struct lm_compute_surface_data_s
{
	uint32_t packed_lightstyles;
	vec3_t	 normal;
	float	 dist;
	uint32_t packed_light_st;
	uint32_t packed_tex_edgecount;
	uint32_t vbo_offset;
	vec4_t	 vecs[2];
} lm_compute_surface_data_t;
COMPILE_TIME_ASSERT (lm_compute_surface_data_t, sizeof (lm_compute_surface_data_t) == 64);

// keep in sync with bmodel_instance_t in globals.inc
typedef struct bmodel_instance_s
{
	vec4_t transform[3];  // model to world, transposed rows
	vec4_t local_vieworg; // view origin in model space, for backface culling
} bmodel_instance_t;
COMPILE_TIME_ASSERT (bmodel_instance_t, sizeof (bmodel_instance_t) == 64);

typedef struct lm_compute_light_s
{
	vec3_t origin;
	float  radius;
	vec3_t color;
	float  minlight; // < 0: rerelease dynamiclight with intensity -minlight, using the KEX falloff
	vec3_t cone_dir;
	float  cone_cos; // cos of the spotlight apex angle, <= -1: not a spotlight
} lm_compute_light_t;
COMPILE_TIME_ASSERT (lm_compute_light_t, sizeof (lm_compute_light_t) == 48);

#define WORKGROUP_BOUNDS_BUFFER_SIZE ((LMBLOCK_WIDTH / 8) * (LMBLOCK_HEIGHT / 8) * sizeof (lm_compute_workgroup_bounds_t))

vulkan_memory_t			   frame_upload_buffers_memory;
static vulkan_memory_t	   surface_data_buffer_memory;
static vulkan_memory_t	   surface_submodels_buffer_memory;
static vulkan_memory_t	   workgroup_bounds_buffer_memory;
static vulkan_memory_t	   indirect_buffer_memory;
static vulkan_memory_t	   indirect_index_buffer_memory;
static vulkan_memory_t	   dyn_visibility_buffer_memory;
static VkBuffer			   surface_data_buffer;
static VkBuffer			   surface_submodels_buffer;
static int				   num_surfaces;
static VkBuffer			   indirect_buffer;
static VkBuffer			   indirect_index_buffer;
static VkBuffer			   dyn_visibility_buffer;
static uint32_t			   dyn_visibility_offset; // for double-buffering
static unsigned char	  *dyn_visibility_view;
static VkBuffer			   lightstyles_scales_buffer;
static VkBuffer			   lights_buffer;
static vulkan_memory_t	   emissive_lights_buffer_memory;
static VkBuffer			   emissive_lights_buffer;
/* Stable per-source band-limit sampling seeds, parallel to the lights array. */
static vulkan_memory_t	   emissive_light_seeds_buffer_memory;
static VkBuffer			   emissive_light_seeds_buffer;
static vulkan_memory_t	   emissive_tiles_buffer_memory;
static VkBuffer			   emissive_tiles_buffer;
static vulkan_memory_t	   emissive_tile_sources_buffer_memory;
static VkBuffer			   emissive_tile_sources_buffer;
static int				   num_emissive_lights;
static emissive_light_t  *emissive_cacheable_lights;
static emissive_light_t   emissive_light_diagnostics[MAX_DLIGHTS];
static int				   num_emissive_light_diagnostics;
static byte				  *emissive_light_styles;
static float			  *emissive_light_modulations;
static VkBuffer			   emissive_modulations_buffer;
static vulkan_memory_t	   emissive_modulations_buffer_memory;
static VkBuffer			   emissive_visibility_buffer;
static vulkan_memory_t	   emissive_visibility_buffer_memory;
static VkBuffer			   emissive_radiance_tiles_buffer;
static vulkan_memory_t	   emissive_radiance_tiles_buffer_memory;
static qboolean			   emissive_visibility_available;
static int				   num_emissive_radiance_tiles;
static int				   num_emissive_radiance_source_links;
static int				   num_emissive_radiance_tile_groups;
static int				   max_emissive_radiance_groups_per_tile;
static int				   num_emissive_modulation_groups;
static qboolean			   emissive_radiance_coarse_pending;
static qboolean			   emissive_radiance_detail_pending;
static qboolean			   emissive_radiance_force_all_styled_tiles;
static qboolean			   emissive_modulations_pending;
static qboolean			   emissive_radiance_logged;
static uint32_t			   emissive_radiance_cpu_time_us;
typedef struct emissive_brush_receiver_layer_s
{
	int				lightmap;
	uint32_t		atlas_offset[2];
	uint32_t		width, height;
	gltexture_t	   *surface_indices_texture;
	gltexture_t	   *coarse_texture;
	gltexture_t	   *detail_texture;
	VkBuffer		visibility_buffers[2];
	vulkan_memory_t visibility_memories[2];
	VkDescriptorSet coarse_descriptor_set;
	VkDescriptorSet detail_descriptor_set;
} emissive_brush_receiver_layer_t;
#define EMISSIVE_BRUSH_RECEIVER_MAX_SOURCES 32
typedef struct emissive_brush_receiver_s
{
	uint32_t						 receiver_instance_id;
	entity_t						*entity;
	qmodel_t						*model;
	vec4_t							 transform[3];
	uint32_t						 transport_signature;
	uint32_t						 radiance_signature;
	uint32_t						 source_indices[EMISSIVE_BRUSH_RECEIVER_MAX_SOURCES];
	int								 source_count;
	qboolean						 active, dirty, radiance_only, ready;
	int								 num_layers;
	emissive_brush_receiver_layer_t *layers;
	VkBuffer						 source_indices_buffer;
	vulkan_memory_t					 source_indices_memory;
	uint32_t						*source_indices_mapped;
	uint64_t						 allocated_bytes;
} emissive_brush_receiver_t;
typedef struct emissive_brush_receiver_push_constants_s
{
	uint32_t cacheable_count, transient_count, coordinate_scale;
	uint32_t atlas_offset_x, atlas_offset_y, receiver_instance_id;
	uint32_t source_count, radiance_only;
	vec4_t	 transform[3];
	uint32_t occluder_mask;
} emissive_brush_receiver_push_constants_t;
COMPILE_TIME_ASSERT (emissive_brush_receiver_push_constants_t, sizeof (emissive_brush_receiver_push_constants_t) == 84);
static emissive_brush_receiver_t *emissive_brush_receivers;
static int						  emissive_brush_receiver_count, emissive_brush_receiver_capacity;
static uint32_t					  emissive_brush_receiver_source_generation = 1;
static uint32_t					  emissive_brush_receiver_uploaded_source_generation;
static uint64_t					  emissive_brush_receiver_allocated_bytes;
static qboolean					  emissive_brush_receiver_budget_limited;
static atomic_uint32_t			  emissive_brush_receiver_updates;
static atomic_uint32_t			  emissive_brush_receiver_dispatches;
static atomic_uint32_t			  emissive_brush_receiver_no_ray_dispatches;
static atomic_uint32_t			  emissive_brush_receiver_transform_invalidations;
enum
{
	EMISSIVE_CLUSTERED_CANDIDATES = 8
};
typedef struct emissive_clustered_candidate_s
{
	uint32_t source_index;
	float	 base_score;
	vec4_t	 direction;
	vec4_t	 color;
	qboolean visible;
} emissive_clustered_candidate_t;
typedef struct emissive_alias_receiver_s
{
	entity_t					  *entity;
	qmodel_t					  *model;
	vec3_t						   origin, angles;
	byte						   scale;
	uint32_t					   source_generation, occluder_generation;
	qboolean					   active, ready;
	emissive_clustered_candidate_t candidates[EMISSIVE_CLUSTERED_CANDIDATES];
} emissive_alias_receiver_t;
static emissive_alias_receiver_t *emissive_alias_receivers;
static int						  emissive_alias_receiver_count, emissive_alias_receiver_capacity;
static atomic_uint32_t emissive_clustered_alias_receivers;
static atomic_uint32_t emissive_clustered_alias_builds;
static atomic_uint32_t emissive_clustered_alias_source_evaluations;
static atomic_uint32_t emissive_clustered_alias_shadow_tests;
static atomic_uint32_t emissive_clustered_alias_shadow_rejections;
static atomic_uint32_t emissive_clustered_alias_contributors;
typedef struct emissive_occluder_state_s
{
	entity_t *entity;
	qmodel_t *model;
	vec3_t	 origin, angles;
	int		 pose1, pose2;
	byte	 scale;
} emissive_occluder_state_t;
#define EMISSIVE_OCCLUDER_UPDATE_INTERVAL (1.0 / 30.0)
#define EMISSIVE_OCCLUDER_POSITION_QUANTUM 1.0f
#define EMISSIVE_OCCLUDER_ANGLE_STEPS 64.0f
#define EMISSIVE_OCCLUDER_BOUNDS_PADDING 8.0f
static emissive_occluder_state_t *emissive_occluder_states, *emissive_occluder_state_scratch;
static int					 emissive_occluder_state_count, emissive_occluder_state_capacity;
static qmodel_t				*emissive_occluder_worldmodel;
static uint32_t				 emissive_brush_occluder_generation;
static qboolean				 emissive_occluder_state_valid, emissive_occluder_dirty_tiles_valid, emissive_live_as_dirty;
static qboolean				 emissive_occluder_receiver_refresh_pending;
static double				 emissive_occluder_next_update_time;
typedef struct emissive_bounce_surface_s
{
	uint32_t direct_base, bounce_base, packed_direct_size, packed_bounce_size;
} emissive_bounce_surface_t;
typedef struct emissive_bounce_sample_s
{
	uint32_t surface, packed_st;
} emissive_bounce_sample_t;
/*
 * coordinate_scale carries a mode-specific stride, not one global scale:
 * mode 0 captures direct texels (1), mode 3 filters bounce samples
 * (emissive_bounce_sample_spacing), mode 4 combines detail output
 * (R_EmissiveDetailScale or 1). offset/extent bound the exact image-space
 * rectangle a partial dispatch may write; modes 1/2 walk samples instead
 * and leave extent unused. Set both explicitly at every dispatch.
 */
typedef struct emissive_bounce_push_constants_s
{
	uint32_t mode, count, coordinate_scale, rays_per_sample;
	float strength, max_distance, reflectance_lift;
	uint32_t first;
	int32_t offset_x, offset_y;
	int32_t extent_x, extent_y;
} emissive_bounce_push_constants_t;
COMPILE_TIME_ASSERT (emissive_bounce_surface_t, sizeof (emissive_bounce_surface_t) == 16);
COMPILE_TIME_ASSERT (emissive_bounce_sample_t, sizeof (emissive_bounce_sample_t) == 8);
COMPILE_TIME_ASSERT (emissive_bounce_push_constants_t, sizeof (emissive_bounce_push_constants_t) == 48);
#define EMISSIVE_BOUNCE_MAX_RAYS 128
#define EMISSIVE_BOUNCE_MEMORY_BUDGET_MB 256
#define EMISSIVE_BOUNCE_VERSION 2
enum
{
	EMISSIVE_BOUNCE_TIMESTAMP_TRANSFER,
	EMISSIVE_BOUNCE_TIMESTAMP_RESOLVE,
	EMISSIVE_BOUNCE_TIMESTAMP_FILTER,
	EMISSIVE_BOUNCE_TIMESTAMP_COARSE_COMBINE,
	EMISSIVE_BOUNCE_TIMESTAMP_DETAIL_START,
	EMISSIVE_BOUNCE_TIMESTAMP_DETAIL_END
};
static VkBuffer emissive_bounce_surfaces_buffer, emissive_bounce_samples_buffer, emissive_bounce_taps_buffer;
static VkBuffer emissive_bounce_direct_buffer, emissive_bounce_reflectance_buffer, emissive_bounce_values_buffer, emissive_bounce_filtered_buffer;
static VkBuffer emissive_bounce_counters_buffer;
static vulkan_memory_t emissive_bounce_memory, emissive_bounce_counters_memory;
static uint32_t *emissive_bounce_counters;
static uint32_t num_emissive_bounce_direct_texels, num_emissive_bounce_samples;
static emissive_bounce_surface_t *emissive_bounce_surface_metadata;
static vec3_t *emissive_bounce_surface_mins, *emissive_bounce_surface_maxs;
static uint32_t emissive_bounce_rays_per_sample;
static uint32_t emissive_bounce_sample_spacing;
static uint64_t emissive_bounce_logical_bytes, emissive_bounce_required_bytes;
static uint32_t emissive_bounce_prepare_time_us;
static uint32_t emissive_bounce_build_time_us, emissive_bounce_resolve_time_us, emissive_bounce_filter_time_us, emissive_bounce_combine_time_us;
static qboolean emissive_bounce_pending, emissive_bounce_building, emissive_bounce_recorded, emissive_bounce_ready;
static qboolean emissive_bounce_recombine_pending;
static qboolean emissive_bounce_cacheable_refresh_pending, emissive_bounce_transient_refresh_pending;
static qboolean emissive_bounce_cacheable_force_full_refresh;
static qboolean emissive_bounce_transient_outputs_initialized, emissive_bounce_transient_force_full_refresh;
static qboolean emissive_bounce_cacheable_latched;
static uint32_t emissive_cacheable_direct_epoch, emissive_cacheable_bounce_epoch;
static uint32_t emissive_transient_direct_epoch, emissive_transient_bounce_epoch;
static uint32_t emissive_bounce_no_ray_refreshes;
/* Which direct input direct_values currently holds: 0 = none, 1 = cacheable, 2 = transient. */
static int emissive_bounce_capture_owner;
static uint32_t emissive_bounce_dirty_receiver_surfaces;
static uint32_t emissive_bounce_refresh_cpu_time_us;
static qboolean emissive_bounce_budget_limited, emissive_bounce_gpu_time_valid, emissive_bounce_admission_attempted;
static qboolean emissive_bounce_transient_budget_limited;
static qboolean emissive_bounce_debug_pending, emissive_bounce_debug_ready, emissive_bounce_debug_budget_limited;
static qboolean			   emissive_coarse_pending;
static qboolean			   emissive_detail_pending;
static qboolean			   emissive_detail_building;
static qboolean			   emissive_detail_ready;
static int				   emissive_detail_recorded_tiles, emissive_detail_recorded_dispatches;
static qboolean			   emissive_detail_budget_limited;
static int				   emissive_detail_scale = 2;
static qboolean			   emissive_bandlimit_active;
static qboolean			   emissive_bandlimit_budget_limited;
static uint64_t			   emissive_bandlimit_logical_bytes;
static uint64_t			   emissive_bandlimit_allocated_bytes;
static uint64_t			   emissive_bandlimit_peak_bytes;
typedef struct emissive_logical_tile_s
{
	uint32_t first_source;
	uint32_t num_sources;
	uint16_t lightmap;
	byte	 x;
	byte	 y;
} emissive_logical_tile_t;
COMPILE_TIME_ASSERT (emissive_logical_tile_t, sizeof (emissive_logical_tile_t) == 12);
static emissive_logical_tile_t *emissive_logical_tiles;
static emissive_logical_tile_t *emissive_radiance_tiles;
static uint32_t				*emissive_logical_tile_sources;
static vec3_t				*emissive_logical_tile_mins, *emissive_logical_tile_maxs;
static byte					*emissive_occluder_dirty_tile_bits;
static emissive_logical_tile_t *emissive_occluder_tiles;
static int					 num_emissive_occluder_dirty_tiles;
static qboolean				 emissive_occluder_full_detail_refresh, emissive_occluder_partial_detail_refresh;
static VkBuffer				 emissive_occluder_tiles_buffer;
static vulkan_memory_t		 emissive_occluder_tiles_buffer_memory;
static VkDescriptorSet		*emissive_occluder_detail_descriptor_sets;
static int					 num_emissive_logical_tiles;
static int					 num_emissive_logical_tiles_total;
static int					 num_emissive_logical_tile_sources;
static qboolean				 emissive_logical_tiles_built;
static emissive_light_t		*transient_emissive_lights;
static emissive_light_t		*previous_transient_emissive_lights;
static transient_emissive_source_id_t *transient_emissive_light_ids;
static transient_emissive_source_id_t *previous_transient_emissive_light_ids;
static uint32_t *transient_emissive_light_seeds;
static int					 num_transient_emissive_lights;
static int					 num_previous_transient_emissive_lights;
static emissive_logical_tile_t *transient_emissive_tiles;
static uint32_t				*transient_emissive_tile_sources;
static int					 num_transient_emissive_tiles;
static int					 num_transient_emissive_tile_sources;
static qboolean				 transient_emissive_pending;
static qboolean				 transient_emissive_detail_pending;
/* Set by occluder movement: the last source-change footprint no longer covers
 * every receiver whose visibility changed, so the update task rebuilds the
 * worklist as the union of active influence before dispatching. */
static qboolean				 transient_emissive_occluder_tiles_pending;
static qboolean				 transient_emissive_detail_ready;
/* Generation whose detail was completely recorded into this frame's update
 * command buffer. Lets draw recording select current-generation detail without
 * waiting for cross-frame completion; the generation check, not a reset,
 * invalidates it when sources change. */
static uint32_t				 transient_emissive_detail_published_generation;
static uint32_t				 transient_emissive_generation;
static uint32_t				 transient_emissive_rejected_publications;
static qboolean				 transient_emissive_initialized;
static qboolean				 transient_emissive_detail_cache_copied;
static qboolean				 transient_emissive_force_refresh;
static VkBuffer				 transient_emissive_lights_buffer;
static vulkan_memory_t		 transient_emissive_lights_buffer_memory;
static VkBuffer				 transient_emissive_light_seeds_buffer;
static vulkan_memory_t		 transient_emissive_light_seeds_buffer_memory;
static VkBuffer				 transient_emissive_tiles_buffer;
static vulkan_memory_t		 transient_emissive_tiles_buffer_memory;
static VkBuffer				 transient_emissive_tile_sources_buffer;
static vulkan_memory_t		 transient_emissive_tile_sources_buffer_memory;
static size_t				 transient_emissive_lights_capacity;
static size_t				 transient_emissive_light_seeds_capacity;
static size_t				 transient_emissive_tiles_capacity;
static size_t				 transient_emissive_tile_sources_capacity;
static uint32_t				 transient_emissive_cpu_time_us;
static uint32_t				*transient_emissive_tile_surface_offsets;
static uint32_t				*transient_emissive_tile_surfaces;
static uint32_t				*transient_emissive_tile_generations;
static int					*transient_emissive_tile_indices;
static uint32_t				 transient_emissive_tile_generation;
static int					*transient_emissive_surface_influence_counts;
static int					*transient_emissive_surface_deltas;
static uint32_t				*transient_emissive_surface_delta_generations;
static int					   *transient_emissive_touched_surfaces;
static int						num_transient_emissive_touched_surfaces;
static uint32_t					transient_emissive_surface_delta_generation;
static int						num_transient_emissive_tile_surfaces;
static int						num_transient_emissive_total_tiles;
static qmodel_t				   *transient_emissive_tile_surface_worldmodel;
static void						R_EnsureTransientEmissiveResources (void);
static qboolean					R_TransientEmissiveDetailAvailable (void);
static VkAccelerationStructureKHR R_EmissiveDirectAccelerationStructure (void);
static uint32_t					R_EmissiveOccluderMask (void);
static void						R_EmissiveBounceSurfaceBounds (const msurface_t *surface, vec3_t mins, vec3_t maxs);
static qboolean					R_SurfaceInEmissiveWorldAccelerationStructure (const msurface_t *surface);
static void						R_DeleteEmissiveBounceResources (void);
static void						R_FreeEmissiveBandlimitDescriptorSets (void);
static void						R_FreeEmissiveOccluderDescriptorSets (void);
static qboolean					R_EnableEmissiveBandlimit (void);
static void						R_ResetEmissiveBandlimitStats (void);
static void						R_CreateEmissiveBandlimitDescriptorSets (const VkDescriptorBufferInfo source_buffers[6]);
static void						R_InitializeTransientEmissiveImages (cb_context_t *cbx, qboolean detail_only);
static void						R_AllocateEmissiveBounceDebugLightmaps (void);
static void						R_InvalidateEmissiveBrushReceiverSources (void);
static void						R_FreeEmissiveBrushReceiverDescriptorSets (void);
static void						R_FreeEmissiveBrushReceivers (void);
static void						R_UpdateEmissiveBrushReceiverLightmaps (cb_context_t *cbx);
static uint32_t R_EmissiveBrushReceiverRadianceSignature (const uint32_t *source_indices, int source_count);
static void R_EmissiveComputeImageBarrier (cb_context_t *cbx, gltexture_t *texture, VkImageLayout old_layout, VkImageLayout new_layout);
static void R_PublishEmissiveBounceImage (cb_context_t *cbx, gltexture_t *texture);
static VkDeviceSize R_EmissiveBufferMemorySize (VkDeviceSize size, VkBufferUsageFlags usage);
static void						R_UpdateTransientEmissiveBuffer (VkCommandBuffer cb, VkBuffer buffer, const void *data, size_t size);
static VkBuffer			   submodel_transforms_buffer;
static float			  *lightstyles_scales_buffer_mapped;
static lm_compute_light_t *lights_buffer_mapped;
static float			  *submodel_transforms_buffer_mapped;
static vulkan_memory_t	   vertex_submodels_buffer_memory;
static VkBuffer			   vertex_submodels_buffer;
static VkBuffer			   bmodel_instances_buffer;
static bmodel_instance_t  *bmodel_instances_buffer_mapped;

// The first entity drawing a submodel through the indirect path each frame claims its instance slot,
// additional entities sharing the submodel fall back to per-entity drawing
static atomic_uint64_t bmodel_instance_claims[MAX_MODELS];

// current_compute_buffer_index flips mid frame, latch the instance buffer half while the frame is set up
static int bmodel_instances_index;

// Movable brush submodels ('*' models) have their lightmap texels lit in entity space: per surface the submodel
// index selects a per-frame model to world transform that is also used to cull dlights per workgroup on the GPU
static int num_worldmodel_submodels;

static int current_compute_buffer_index;

/*
================
SizeToBin
================
*/
static int SizeToBin (int size)
{
	size -= 1;
	if (size < LM_BIN_E * 2 + 1)
		return size;
	int bc = Q_log2 (size / LM_BIN_E);
	return (size >> bc) + LM_BIN_E * bc + 1;
}

/*
================
BinToSize
================
*/
static int BinToSize (int bin)
{
	if (bin < LM_BIN_E * 2 + 1)
		return bin + 1;
	bin -= 1;
	int bc = bin / LM_BIN_E - 1;
	return (bin % LM_BIN_E + LM_BIN_E + 1) << bc;
}

/*
================
R_AllocDepsData
================
*/
static int R_AllocDepsData (combined_brush_deps *items)
{
	static int last = 0;
	int		   item_count = items[0].water_count + items[0].lm_count;

	if (last < used_deps_data && !memcmp (items, &brush_deps_data[last], sizeof (combined_brush_deps)) &&
		!memcmp (items + 1, &brush_deps_data[last + 1], item_count * sizeof (combined_brush_deps)))
		return last;

	if (used_deps_data == 0)
		brush_deps_data = Mem_Alloc (INITIAL_BRUSH_DEPS_SIZE * sizeof (combined_brush_deps));

	for (int i = 0; i <= item_count; i++)
	{
		if (used_deps_data >= INITIAL_BRUSH_DEPS_SIZE && !(used_deps_data & (used_deps_data - 1)))
			brush_deps_data = Mem_Realloc (brush_deps_data, used_deps_data * 2 * sizeof (combined_brush_deps));
		brush_deps_data[used_deps_data] = items[i];
		++used_deps_data;
	}
	return (last = used_deps_data - item_count - 1);
}

/*
================
R_CalcDeps
================
*/
static void R_CalcDeps (qmodel_t *model, mleaf_t *leaf)
{
	combined_brush_deps deps[1 + 256 + MAX_SANITY_LIGHTMAPS];
	deps[0].water_count = 0;
	deps[0].lm_count = 0;
	const int num_surfs = model ? model->nummodelsurfaces : leaf->nummarksurfaces;

	for (int i = 0; i < num_surfs; i++)
	{
		msurface_t *psurf = model ? &model->surfaces[model->firstmodelsurface] + i : &cl.worldmodel->surfaces[leaf->firstmarksurface[i]];
		texture_t  *t = psurf->texinfo->texture;
		if (t->name[0] == '*' || t->name[0] == '!')
		{
			qboolean found = false;
			for (int j = 1; j < 1 + deps[0].water_count; j++)
				if (deps[j].update_warp == &t->update_warp)
				{
					found = true;
					break;
				}
			if (!found)
			{
				++deps[0].water_count;
				if (deps[0].water_count > 256)
					Sys_Error ("A single bmodel / world leaf is using more than 256 different water textures");
				if (sizeof (atomic_uint32_t *) < sizeof (combined_brush_deps)) // make sure the padding is 0 in 32-bit builds
					memset (&deps[deps[0].water_count], 0, sizeof (combined_brush_deps));
				deps[deps[0].water_count].update_warp = &t->update_warp;
			}
		}
	}

	for (int i = 0; i < num_surfs; i++)
	{
		msurface_t *psurf = model ? &model->surfaces[model->firstmodelsurface] + i : &cl.worldmodel->surfaces[leaf->firstmarksurface[i]];
		if (psurf->lightmaptexturenum >= 0)
		{
			qboolean found = false;
			for (int j = 1 + deps[0].water_count; j < 1 + deps[0].water_count + deps[0].lm_count; j++)
				if (deps[j].lightmap_num == psurf->lightmaptexturenum)
				{
					deps[j].lightmap_styles |= psurf->styles_bitmap;
					found = true;
					break;
				}
			if (!found)
			{
				++deps[0].lm_count;
				deps[deps[0].water_count + deps[0].lm_count].lightmap_num = psurf->lightmaptexturenum;
				deps[deps[0].water_count + deps[0].lm_count].lightmap_styles = psurf->styles_bitmap;
			}
		}
	}

	if (model)
		model->combined_deps = R_AllocDepsData (deps);
	else
		leaf->combined_deps = R_AllocDepsData (deps);
}

/*
================
R_MarkDeps
================
*/
void R_MarkDeps (int combined_deps, int worker_index)
{
	combined_brush_deps *deps = &brush_deps_data[combined_deps];
	int					 water_count = deps->water_count;
	int					 lm_count = deps->lm_count;
	int					 i;
	for (i = 0; i < water_count; ++i)
		Atomic_StoreUInt32_Relaxed ((++deps)->update_warp, true);
	for (i = 0, ++deps; i < lm_count; ++i, ++deps)
		lightmaps[deps->lightmap_num].modified[worker_index] |= deps->lightmap_styles;
}

/*
===============
R_TextureAnimation -- johnfitz -- added "frame" param to eliminate use of "currententity" global

Returns the proper texture for a given time and base texture
===============
*/
texture_t *R_TextureAnimation (texture_t *base, int frame)
{
	int relative;
	int count;

	if (frame)
		if (base->alternate_anims)
			base = base->alternate_anims;

	if (!base->anim_total)
		return base;

	relative = (int)(cl.time * 10) % base->anim_total;

	count = 0;
	while (base->anim_min > relative || base->anim_max <= relative)
	{
		base = base->anim_next;
		if (!base)
			Sys_Error ("R_TextureAnimation: broken cycle");
		if (++count > 100)
			Sys_Error ("R_TextureAnimation: infinite cycle");
	}

	return base;
}

/*
================
DrawGLPoly
================
*/
void DrawGLPoly (cb_context_t *cbx, glpoly_t *p, float color[3], float alpha)
{
	const int numverts = p->numverts;
	const int numtriangles = (numverts - 2);
	const int numindices = numtriangles * 3;

	VkBuffer	 vertex_buffer;
	VkDeviceSize vertex_buffer_offset;

	basicvertex_t *vertices = (basicvertex_t *)R_VertexAllocate (numverts * sizeof (basicvertex_t), &vertex_buffer, &vertex_buffer_offset);

	float *v;
	int	   i;
	int	   current_index = 0;

	v = p->verts[0];
	for (i = 0; i < numverts; ++i, v += VERTEXSIZE)
	{
		vertices[i].position[0] = v[0];
		vertices[i].position[1] = v[1];
		vertices[i].position[2] = v[2];
		vertices[i].texcoord[0] = v[3];
		vertices[i].texcoord[1] = v[4];
		vertices[i].color[0] = color[0] * 255.0f;
		vertices[i].color[1] = color[1] * 255.0f;
		vertices[i].color[2] = color[2] * 255.0f;
		vertices[i].color[3] = alpha * 255.0f;
	}

	// I don't know the maximum poly size quake maps can have, so just in case fall back to dynamic allocations
	// TODO: Find out if it's necessary
	if (numindices > FAN_INDEX_BUFFER_SIZE)
	{
		VkBuffer	 index_buffer;
		VkDeviceSize index_buffer_offset;

		uint16_t *indices = (uint16_t *)R_IndexAllocate (numindices * sizeof (uint16_t), &index_buffer, &index_buffer_offset);
		for (i = 0; i < numtriangles; ++i)
		{
			indices[current_index++] = 0;
			indices[current_index++] = 1 + i;
			indices[current_index++] = 2 + i;
		}
		vulkan_globals.vk_cmd_bind_index_buffer (cbx->cb, index_buffer, index_buffer_offset, VK_INDEX_TYPE_UINT16);
	}
	else
		vulkan_globals.vk_cmd_bind_index_buffer (cbx->cb, vulkan_globals.fan_index_buffer, 0, VK_INDEX_TYPE_UINT16);

	vulkan_globals.vk_cmd_bind_vertex_buffers (cbx->cb, 0, 1, &vertex_buffer, &vertex_buffer_offset);
	vulkan_globals.vk_cmd_draw_indexed (cbx->cb, numindices, 1, 0, 0, 0);
}

/*
=============================================================

	BRUSH MODELS

=============================================================
*/

/*
================
R_RecursiveNode
================
*/
static void R_RecursiveNode (
	mnode_t *node, qmodel_t *model, vec3_t modelorg, int chain, int *brushpolys, int *surfs_visited, int worker_index, qboolean water_transparent_only)
{
	if (node->contents >= 0)
	{
		mplane_t *plane = node->plane;
		float	  dot = (plane->type < 3 ? modelorg[plane->type] : DotProduct (modelorg, plane->normal)) - plane->dist;

		// recurse down the children, front side first (chained surfaces are drawn in reverse order)
		R_RecursiveNode (node->children[dot < 0], model, modelorg, chain, brushpolys, surfs_visited, worker_index, water_transparent_only);

		msurface_t *surf = model->surfaces + node->firstsurface;
		for (int i = node->numsurfaces; i > 0; --i, surf++)
			if (((surf->flags & SURF_PLANEBACK && dot < -BACKFACE_EPSILON) || (!(surf->flags & SURF_PLANEBACK) && dot > BACKFACE_EPSILON)) &&
				(!water_transparent_only || (surf->flags & SURF_DRAWTURB && GL_WaterAlphaForSurface (surf) != 1)))
			{
				R_ChainSurface (surf, chain);
				++(*brushpolys);
				if (!r_gpulightmapupdate.value)
					R_RenderDynamicLightmaps (surf);
				else if (surf->lightmaptexturenum >= 0)
					lightmaps[surf->lightmaptexturenum].modified[worker_index] |= surf->styles_bitmap;
			}
		*surfs_visited += node->numsurfaces;

		R_RecursiveNode (node->children[dot >= 0], model, modelorg, chain, brushpolys, surfs_visited, worker_index, water_transparent_only);
	}
}

/*
=================
R_ClearBModelInstanceClaims
=================
*/
void R_ClearBModelInstanceClaims (void)
{
	bmodel_instances_index = current_compute_buffer_index;
	memset ((void *)bmodel_instance_claims, 0, num_worldmodel_submodels * sizeof (bmodel_instance_claims[0]));
}

/*
=================
R_ClaimBModelInstance
=================
*/
static qboolean R_ClaimBModelInstance (entity_t *e, int submodel)
{
	uint64_t expected = 0;
	if (Atomic_CompareExchangeUInt64 (&bmodel_instance_claims[submodel], &expected, (uint64_t)(uintptr_t)e))
		return true;
	return expected == (uint64_t)(uintptr_t)e;
}

/*
=================
R_IndirectBrush
=================
*/
qboolean R_IndirectBrush (entity_t *e)
{
	assert (e->model->type == mod_brush);
	if (R_EmissiveBrushReceiverActive (e))
		return false;
	const qboolean transparent_entity = ENTALPHA_DECODE (e->alpha) != 1.0f;
	const qboolean has_water = brush_deps_data[e->model->combined_deps].water_count != 0;
	if (has_water && r_emissive_rt_liquid_receivers.value > 0.0f)
		return false;
	// the indirect path only knows global water alpha, so entities with fixed alpha need per-entity drawing
	const qboolean fixed_alpha_water = e->alpha != ENTALPHA_DEFAULT && has_water;
	// without OIT, water needs the stable draw order of the texture chains even in indirect mode
	const qboolean alpha_sorted = !R_UseOIT () && (transparent_entity || has_water);
	if (!indirect || transparent_entity || fixed_alpha_water || alpha_sorted || (e->frame != 0) || (e->model->name[0] != '*'))
		return false;
	const qboolean transformed =
		e->origin[0] || e->origin[1] || e->origin[2] || e->angles[0] || e->angles[1] || e->angles[2] || (ENTSCALE_DECODE (e->netstate.scale) != 1.0f);
	// sky surfaces are drawn with pipelines that don't apply instance transforms
	if (transformed && (e->model->used_specials & SURF_DRAWSKY))
		return false;
	const int submodel = atoi (e->model->name + 1);
	if ((submodel <= 0) || (submodel >= num_worldmodel_submodels))
		return !transformed;
	return R_ClaimBModelInstance (e, submodel);
}

/*
=================
R_DrawBrushModel
=================
*/
void R_DrawBrushModel (cb_context_t *cbx, entity_t *e, int chain, int *brushpolys, qboolean sort, qboolean water_opaque_only, qboolean water_transparent_only)
{
	int			i, k;
	msurface_t *psurf;
	float		dot;
	mplane_t   *pplane;
	qmodel_t   *clmodel;
	vec3_t		modelorg;

	if (R_CullModelForEntity (e))
		return;

	clmodel = e->model;

	if (!water_opaque_only && !water_transparent_only && R_IndirectBrush (e))
	{
		const int submodel = atoi (clmodel->name + 1);
		if ((submodel > 0) && (submodel < num_worldmodel_submodels))
		{
			bmodel_instance_t *instance = bmodel_instances_buffer_mapped + ((size_t)bmodel_instances_index * MAX_MODELS) + submodel;

			vec3_t e_angles;
			VectorCopy (e->angles, e_angles);
			e_angles[0] = -e_angles[0]; // stupid quake bug
			float model_matrix[16];
			IdentityMatrix (model_matrix);
			R_RotateForEntity (model_matrix, e->origin, e_angles, e->netstate.scale);
			for (int row = 0; row < 3; ++row)
				for (int col = 0; col < 4; ++col)
					instance->transform[row][col] = model_matrix[(col * 4) + row];

			// same backface culling origin as the per-entity path below: rotated into model space, unscaled
			VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
			if (e->angles[0] || e->angles[1] || e->angles[2])
			{
				vec3_t temp;
				vec3_t forward, right, up;

				VectorCopy (modelorg, temp);
				AngleVectors (e->angles, forward, right, up);
				modelorg[0] = DotProduct (temp, forward);
				modelorg[1] = -DotProduct (temp, right);
				modelorg[2] = DotProduct (temp, up);
			}
			VectorCopy (modelorg, instance->local_vieworg);
			instance->local_vieworg[3] = 0.0f;
		}

		// indirect mark
		int				 start = clmodel->firstmodelsurface;
		int				 end = start + clmodel->nummodelsurfaces;
		int				 startword = start / 32;
		int				 endword = end / 32;
		atomic_uint32_t *surfvis = (atomic_uint32_t *)cl.worldmodel->surfvis;
		if (startword == endword)
			Atomic_OrUInt32 (&surfvis[startword], (1u << end % 32) - (1u << start % 32));
		else
		{
			uint32_t supress_warning = (1u << start % 32);
			Atomic_OrUInt32 (&surfvis[startword], (1ull << 32) - supress_warning);
			for (i = startword + 1; i < endword; i++)
				Atomic_StoreUInt32 (&surfvis[i], 0xFFFFFFFF);
			Atomic_OrUInt32 (&surfvis[endword], (1u << end % 32) - 1);
		}
		R_MarkDeps (clmodel->combined_deps, Tasks_GetWorkerIndex ());
		return;
	}

	VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		vec3_t temp;
		vec3_t forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (e->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];

	// calculate dynamic lighting for bmodel if it's not an
	// instanced model
	if (!r_gpulightmapupdate.value && clmodel->firstmodelsurface != 0)
	{
		for (k = 0; k < MAX_DLIGHTS; k++)
		{
			if ((cl_dlights[k].die < cl.time) || (!cl_dlights[k].radius))
				continue;

			// transform the light into entity space, the surfaces and nodes of moved brush models are in model space
			dlight_t local_light = cl_dlights[k];
			VectorSubtract (local_light.origin, e->origin, local_light.origin);
			if (e->angles[0] || e->angles[1] || e->angles[2])
			{
				vec3_t temp;
				vec3_t forward, right, up;

				VectorCopy (local_light.origin, temp);
				AngleVectors (e->angles, forward, right, up);
				local_light.origin[0] = DotProduct (temp, forward);
				local_light.origin[1] = -DotProduct (temp, right);
				local_light.origin[2] = DotProduct (temp, up);
			}
			VectorCopy (local_light.origin, lightmap_dlight_origins[k]); // for R_AddDynamicLights
			R_MarkLights (&local_light, k, clmodel->nodes + clmodel->hulls[0].firstclipnode);
		}
	}

	vec3_t e_angles;
	VectorCopy (e->angles, e_angles);
	e_angles[0] = -e_angles[0]; // stupid quake bug
	float model_matrix[16];
	IdentityMatrix (model_matrix);
	R_RotateForEntity (model_matrix, e->origin, e_angles, e->netstate.scale);

	float mvp[16];
	memcpy (mvp, vulkan_globals.view_projection_matrix, 16 * sizeof (float));
	MatrixMultiply (mvp, model_matrix);

	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 16 * sizeof (float), mvp);
	R_ClearTextureChains (clmodel, chain);
	const int worker_index = Tasks_GetWorkerIndex ();
	if (sort && !clmodel->bogus_tree)
	{
		mnode_t *head = &clmodel->nodes[clmodel->hulls[0].firstclipnode];
		int		 surfs_visited = 0;
		R_RecursiveNode (head, clmodel, modelorg, chain, brushpolys, &surfs_visited, worker_index, water_transparent_only);
		if (surfs_visited != clmodel->nummodelsurfaces)
		{
			Con_DPrintf ("model %s nummodelsurfaces %d != node tree numsurfaces sum %d\n", clmodel->name, clmodel->nummodelsurfaces, surfs_visited);
			clmodel->bogus_tree = true;
			R_ClearTextureChains (clmodel, chain);
		}
	}
	if (!sort || clmodel->bogus_tree)
		for (i = 0; i < clmodel->nummodelsurfaces; i++, psurf++)
		{
			if (water_opaque_only && psurf->flags & SURF_DRAWTURB && GL_WaterAlphaForSurface (psurf) != 1)
				continue;
			if (water_transparent_only && (!(psurf->flags & SURF_DRAWTURB) || GL_WaterAlphaForSurface (psurf) == 1))
				continue;
			pplane = psurf->plane;
			dot = DotProduct (modelorg, pplane->normal) - pplane->dist;
			if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) || (!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
			{
				R_ChainSurface (psurf, chain);
				++(*brushpolys);
				if (!r_gpulightmapupdate.value)
					R_RenderDynamicLightmaps (psurf);
				else if (psurf->lightmaptexturenum >= 0)
					lightmaps[psurf->lightmaptexturenum].modified[worker_index] |= psurf->styles_bitmap;
			}
		}

	if (!water_transparent_only)
		R_DrawTextureChains (cbx, clmodel, e, chain);
	if (clmodel->used_specials & SURF_DRAWTURB)
		R_DrawTextureChains_Water (cbx, clmodel, e, chain, water_opaque_only, water_transparent_only);
	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 16 * sizeof (float), vulkan_globals.view_projection_matrix);
}

/*
=================
R_DrawBrushModel_ShowTris -- johnfitz
=================
*/
void R_DrawBrushModel_ShowTris (cb_context_t *cbx, entity_t *e)
{
	int			i;
	msurface_t *psurf;
	float		dot;
	mplane_t   *pplane;
	qmodel_t   *clmodel;
	float		color[] = {1.0f, 1.0f, 1.0f};
	const float alpha = 1.0f;
	vec3_t		modelorg;

	if (R_CullModelForEntity (e) || R_IndirectBrush (e))
		return;

	clmodel = e->model;

	VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		vec3_t temp;
		vec3_t forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (e->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];

	e->angles[0] = -e->angles[0]; // stupid quake bug
	float model_matrix[16];
	IdentityMatrix (model_matrix);
	R_RotateForEntity (model_matrix, e->origin, e->angles, e->netstate.scale);
	e->angles[0] = -e->angles[0]; // stupid quake bug

	float mvp[16];
	memcpy (mvp, vulkan_globals.view_projection_matrix, 16 * sizeof (float));
	MatrixMultiply (mvp, model_matrix);

	if (r_showtris.value == 1)
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.showtris_pipeline[R_MainPassPipelineVariant (cbx->render_pass_index)]);
	else
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.showtris_depth_test_pipeline[R_MainPassPipelineVariant (cbx->render_pass_index)]);
	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 16 * sizeof (float), mvp);

	//
	// draw it
	//
	for (i = 0; i < clmodel->nummodelsurfaces; i++, psurf++)
	{
		pplane = psurf->plane;
		dot = DotProduct (modelorg, pplane->normal) - pplane->dist;
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) || (!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			DrawGLPoly (cbx, psurf->polys, color, alpha);
		}
	}

	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 16 * sizeof (float), vulkan_globals.view_projection_matrix);
}

/*
=============
R_DrawIndirectBrushes
=============
*/
void R_DrawIndirectBrushes (cb_context_t *cbx, qboolean draw_water, qboolean transparent_water, qboolean draw_sky, int index)
{
	assert (!draw_water || !draw_sky);

	R_BeginDebugUtilsLabel (cbx, "Indirect Brushes");

	VkDeviceSize offset = 0;
	vulkan_globals.vk_cmd_bind_vertex_buffers (cbx->cb, 0, 1, &bmodel_vertex_buffer, &offset);
	vulkan_globals.vk_cmd_bind_index_buffer (cbx->cb, indirect_index_buffer, 0, VK_INDEX_TYPE_UINT32);

	if (!draw_sky)
	{
		vulkan_globals.vk_cmd_bind_descriptor_sets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 2, 1, &nulltexture->descriptor_set, 0, NULL);
		if (r_lightmap_cheatsafe)
			vulkan_globals.vk_cmd_bind_descriptor_sets (
				cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 0, 1, &greytexture->descriptor_set, 0, NULL);
		vulkan_globals.vk_cmd_bind_descriptor_sets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 4, 1, &vulkan_globals.bmodel_instances_desc_set, 0, NULL);
		const uint32_t instance_base = ((uint32_t)bmodel_instances_index * MAX_MODELS) + 1;
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 21 * sizeof (float), sizeof (uint32_t), &instance_base);
		const uint32_t emissive_atlas_offset[2] = {0, 0};
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 22 * sizeof (float), sizeof (emissive_atlas_offset), emissive_atlas_offset);
	}

	gltexture_t *lastfullbright = NULL;
	gltexture_t *lastemissive = NULL;
	gltexture_t *lastemissivedetail = NULL;
	gltexture_t *lastemissivesurfaceindices = NULL;
	gltexture_t *lastlightmap = NULL;
	gltexture_t *lasttexture = NULL;
	const int	 debug_mode = CLAMP (0, (int)r_emissive_rt_debug.value, 9);
	const qboolean detail_ready = R_EmissiveDetailReady ();
	const qboolean transient_emissive_active = R_TransientEmissiveActive ();
	float		 last_alpha = FLT_MAX;
	float		 last_constant_factor = FLT_MAX;

	int part_size = (used_indirect_draws + NUM_WORLD_CBX - 1) / NUM_WORLD_CBX;
	int start = index < 0 ? 0 : part_size * index;
	int end = index < 0 ? used_indirect_draws : q_min (part_size * (index + 1), used_indirect_draws);

	for (int i = start; i < end; i++)
	{
		texture_t	*t = indirect_draws[i].texture;
		texture_t	*texture = R_TextureAnimation (t, 0);
		gltexture_t *gl_texture = draw_water ? texture->warpimage : texture->gltexture;

		if (!draw_sky && !gl_texture)
			continue;
		if (draw_water != TEXTYPE_ISLIQUID (texture->type))
			continue;
		if (draw_sky != (texture->type == TEXTYPE_SKY))
			continue;

		if (!draw_sky && !r_lightmap_cheatsafe && lasttexture != gl_texture)
		{
			vulkan_globals.vk_cmd_bind_descriptor_sets (
				cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 0, 1, &gl_texture->descriptor_set, 0, NULL);
			lasttexture = gl_texture;
		}

		float alpha = 1.0f;
		if (draw_water)
		{
			alpha = GL_WaterAlphaForTextureType (texture->type);

			if ((alpha < 1.0f) != transparent_water)
				continue;

			if (alpha != last_alpha)
			{
				R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 20 * sizeof (float), 1 * sizeof (float), &alpha);
				last_alpha = alpha;
			}
		}

		qboolean	 fullbright_enabled = false;
		gltexture_t *fullbright;
		if (!draw_sky && gl_fullbrights.value && (fullbright = R_TextureAnimation (t, 0)->fullbright) && !r_lightmap_cheatsafe)
		{
			fullbright_enabled = true;
			if (lastfullbright != fullbright)
			{
				vulkan_globals.vk_cmd_bind_descriptor_sets (
					cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 2, 1, &fullbright->descriptor_set, 0, NULL);
				lastfullbright = fullbright;
			}
		}

		if (!draw_sky)
		{
			const qboolean	  alpha_test = texture->type == TEXTYPE_CUTOUT;
			const qboolean	  alpha_blend = alpha < 1.0f;
			const int		  lm_idx = indirect_draws[i].lightmap_idx;
			const qboolean bounce_debug = debug_mode == 5 && !draw_water && (indirect_draws[i].world_flags & INDIRECT_WORLD_MODEL) && lm_idx >= 0;
			gltexture_t *emissive_texture = NULL;
			gltexture_t *emissive_detail_texture = NULL;
			if (bounce_debug)
				emissive_texture = R_EmissiveBounceDebugReady () ? lightmaps[lm_idx].emissive_bounce_debug_texture : NULL;
			else if (!draw_water && (indirect_draws[i].world_flags & INDIRECT_EMISSIVE_INFLUENCE) && lm_idx >= 0)
				R_EmissiveResolvedTextures (lm_idx, &emissive_texture, &emissive_detail_texture);
			if (bounce_debug || draw_water || !(indirect_draws[i].world_flags & INDIRECT_EMISSIVE_INFLUENCE) || lm_idx < 0)
				emissive_detail_texture = NULL;
			const qboolean	  emissive_enabled = !alpha_blend && emissive_texture && r_emissive_rt.value > 0.0f && gl_fullbrights.value > 0.0f &&
											 !r_fullbright_cheatsafe && !r_lightmap_cheatsafe;
			const qboolean detail_enabled = emissive_enabled && emissive_detail_texture &&
				(transient_emissive_active
					 ? R_TransientEmissiveDetailReady () || R_TransientEmissiveDetailPublished ()
					 : detail_ready);
			const qboolean	  bandlimit_enabled = detail_enabled && R_EmissiveBandlimitActive ();
			const qboolean	  emissive_debug = emissive_enabled && debug_mode > 0 && (debug_mode == 1 || debug_mode == 5 || detail_enabled);
			vec3_t			  liquid_emissive_add;
			const qboolean	  liquid_emissive_receiver =
				draw_water && indirect_draws[i].surface && R_EmissiveApproximateSurfaceLight (indirect_draws[i].surface, NULL, liquid_emissive_add);
			int pipeline_index = (fullbright_enabled ? 1 : 0) + (alpha_test ? 2 : 0) + (alpha_blend ? 4 : 0) +
								 (vid_filter.value != 0 && vid_palettize.value != 0 ? 8 : 0) + (emissive_enabled ? 16 : 0) + (detail_enabled ? 32 : 0) +
								 (bandlimit_enabled ? 64 : 0);
			vulkan_pipeline_t pipeline;
			if (liquid_emissive_receiver)
			{
				const int liquid_pipeline_index = alpha_blend + ((vid_filter.value != 0 && vid_palettize.value != 0) ? 2 : 0);
				pipeline = R_PipelineForRenderPass (
					cbx->render_pass_index, vulkan_globals.liquid_emissive_pipelines[R_MainPassPipelineVariant (cbx->render_pass_index)][liquid_pipeline_index],
					vulkan_globals.liquid_emissive_wboit_pipelines[liquid_pipeline_index],
					vulkan_globals.liquid_emissive_mboit_moment_pipelines[liquid_pipeline_index],
					vulkan_globals.liquid_emissive_mboit_composite_pipelines[liquid_pipeline_index]);
				R_PushConstants (cbx, VK_SHADER_STAGE_FRAGMENT_BIT, 24 * sizeof (float), sizeof (vec3_t), liquid_emissive_add);
			}
			else if (emissive_debug)
			{
				const int debug_pipeline_index =
					alpha_test + ((vid_filter.value != 0 && vid_palettize.value != 0) ? 2 : 0) + ((debug_mode - 1) * 4) + (bandlimit_enabled ? 36 : 0);
				pipeline = vulkan_globals.world_emissive_debug_pipelines[R_MainPassPipelineVariant (cbx->render_pass_index)][debug_pipeline_index];
			}
			else
				pipeline = R_PipelineForRenderPass (
					cbx->render_pass_index, vulkan_globals.world_pipelines[R_MainPassPipelineVariant (cbx->render_pass_index)][pipeline_index],
					vulkan_globals.world_wboit_pipelines[pipeline_index], vulkan_globals.world_mboit_moment_pipelines[pipeline_index],
					vulkan_globals.world_mboit_composite_pipelines[pipeline_index]);
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

			qboolean use_zbias = INDIRECT_ZBIAS && gl_zfix.value && indirect_draws[i].is_bmodel;
			float	 constant_factor = 0.0f, slope_factor = 0.0f;
			if (use_zbias)
			{
				if (vulkan_globals.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT || vulkan_globals.depth_format == VK_FORMAT_D32_SFLOAT)
				{
					constant_factor = -4.f;
					slope_factor = -0.125f;
				}
				else
				{
					constant_factor = -1.f;
					slope_factor = -0.25f;
				}
			}
			if (last_constant_factor != constant_factor)
			{
				vkCmdSetDepthBias (cbx->cb, constant_factor, 0.0f, slope_factor);
				last_constant_factor = constant_factor;
			}

			gltexture_t *lightmap_texture = (r_fullbright_cheatsafe || lm_idx < 0) ? greylightmap : lightmaps[lm_idx].texture;
			if (lastlightmap != lightmap_texture)
			{
				vulkan_globals.vk_cmd_bind_descriptor_sets (
					cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 1, 1, &lightmap_texture->descriptor_set, 0, NULL);
				lastlightmap = lightmap_texture;
			}
			if (emissive_enabled && lastemissive != emissive_texture)
			{
				vulkan_globals.vk_cmd_bind_descriptor_sets (
					cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 5, 1, &emissive_texture->descriptor_set, 0, NULL);
				lastemissive = emissive_texture;
			}
			gltexture_t *const emissive_detail_binding = emissive_detail_texture ? emissive_detail_texture : emissive_texture;
			if (emissive_enabled && lastemissivedetail != emissive_detail_binding)
			{
				vulkan_globals.vk_cmd_bind_descriptor_sets (
					cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 6, 1,
					&emissive_detail_binding->descriptor_set, 0, NULL);
				lastemissivedetail = emissive_detail_binding;
			}
			gltexture_t *const emissive_surface_indices = bandlimit_enabled ? lightmaps[lm_idx].surface_indices_texture : NULL;
			if (emissive_surface_indices && lastemissivesurfaceindices != emissive_surface_indices)
			{
				vulkan_globals.vk_cmd_bind_descriptor_sets (
					cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 7, 1,
					&emissive_surface_indices->descriptor_set, 0, NULL);
				lastemissivesurfaceindices = emissive_surface_indices;
			}
		}

		vulkan_globals.vk_cmd_draw_indexed_indirect (cbx->cb, indirect_buffer, i * sizeof (VkDrawIndexedIndirectCommand), 1, 0);
	}

	R_EndDebugUtilsLabel (cbx);
}

/*
=============
R_DrawIndirectBrushes_ShowTris
=============
*/
void R_DrawIndirectBrushes_ShowTris (cb_context_t *cbx)
{
	R_BindPipeline (
		cbx, VK_PIPELINE_BIND_POINT_GRAPHICS,
		r_showtris.value == 1 ? vulkan_globals.showtris_indirect_pipeline[R_MainPassPipelineVariant (cbx->render_pass_index)]
							  : vulkan_globals.showtris_indirect_depth_test_pipeline[R_MainPassPipelineVariant (cbx->render_pass_index)]);

	vulkan_globals.vk_cmd_bind_descriptor_sets (
		cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout.handle, 4, 1, &vulkan_globals.bmodel_instances_desc_set, 0, NULL);
	const uint32_t instance_base = ((uint32_t)bmodel_instances_index * MAX_MODELS) + 1;
	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 21 * sizeof (float), sizeof (uint32_t), &instance_base);

	VkDeviceSize offset = 0;
	vulkan_globals.vk_cmd_bind_vertex_buffers (cbx->cb, 0, 1, &bmodel_vertex_buffer, &offset);
	vulkan_globals.vk_cmd_bind_index_buffer (cbx->cb, indirect_index_buffer, 0, VK_INDEX_TYPE_UINT32);

	if (vulkan_globals.multi_draw_indirect)
		vulkan_globals.vk_cmd_draw_indexed_indirect (cbx->cb, indirect_buffer, 0, used_indirect_draws, sizeof (VkDrawIndexedIndirectCommand));
	else
		for (int i = 0; i < used_indirect_draws; i++)
			vulkan_globals.vk_cmd_draw_indexed_indirect (cbx->cb, indirect_buffer, i * sizeof (VkDrawIndexedIndirectCommand), 1, 0);
}

/*
=============================================================

	LIGHTMAPS

=============================================================
*/

/*
================
R_RenderDynamicLightmaps
called during rendering
================
*/
void R_RenderDynamicLightmaps (msurface_t *fa)
{
	byte	 *base;
	int		  maps;
	glRect_t *theRect;
	int		  smax, tmax;

	if (fa->flags & SURF_DRAWTILED) // johnfitz -- not a lightmapped surface
		return;

	// check for lightmap modification
	for (maps = 0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++)
		if (d_lightstylevalue[fa->styles[maps]] != fa->cached_light[maps])
			goto dynamic;

	if (fa->dlightframe == r_framecount // dynamic this frame
		|| fa->cached_dlight)			// dynamic previously
	{
	dynamic:
		if (r_dynamic.value)
		{
			struct lightmap_s *lm = &lightmaps[fa->lightmaptexturenum];
			lm->modified[Tasks_GetWorkerIndex ()] = true;
			theRect = &lm->rectchange;
			if (fa->light_t < theRect->t)
			{
				if (theRect->h)
					theRect->h += theRect->t - fa->light_t;
				theRect->t = fa->light_t;
			}
			if (fa->light_s < theRect->l)
			{
				if (theRect->w)
					theRect->w += theRect->l - fa->light_s;
				theRect->l = fa->light_s;
			}
			smax = (fa->extents[0] >> 4) + 1;
			tmax = (fa->extents[1] >> 4) + 1;
			if ((theRect->w + theRect->l) < (fa->light_s + smax))
				theRect->w = (fa->light_s - theRect->l) + smax;
			if ((theRect->h + theRect->t) < (fa->light_t + tmax))
				theRect->h = (fa->light_t - theRect->t) + tmax;
			base = lm->data;
			base += fa->light_t * LMBLOCK_WIDTH * LIGHTMAP_BYTES + fa->light_s * LIGHTMAP_BYTES;
			R_BuildLightMap (fa, base, LMBLOCK_WIDTH * LIGHTMAP_BYTES);
		}
	}
}

/*
========================
AllocBlock -- returns a texture number and the position inside it
========================
*/
static int AllocBlock (int w, int h, int *x, int *y)
{
	int i, j, k, l;
	int texnum;

	for (texnum = last_lightmap_allocated; texnum < MAX_SANITY_LIGHTMAPS; texnum++)
	{
		if (texnum == lightmap_count)
		{
			lightmap_count++;
			lightmaps = (struct lightmap_s *)Mem_Realloc (lightmaps, sizeof (*lightmaps) * lightmap_count);
			memset (&lightmaps[texnum], 0, sizeof (lightmaps[texnum]));
			lightmaps[texnum].data = (byte *)Mem_Alloc (LIGHTMAP_BYTES * LMBLOCK_WIDTH * LMBLOCK_HEIGHT);
			for (i = 0; i < MAXLIGHTMAPS * 3 / 4; ++i)
				lightmaps[texnum].lightstyle_data[i] = (byte *)Mem_Alloc (LIGHTMAP_BYTES * LMBLOCK_WIDTH * LMBLOCK_HEIGHT);
			lightmaps[texnum].surface_indices = (uint32_t *)Mem_Alloc (sizeof (uint32_t) * LMBLOCK_WIDTH * LMBLOCK_HEIGHT);
			memset (lightmaps[texnum].surface_indices, 0xFF, 4 * LMBLOCK_WIDTH * LMBLOCK_HEIGHT);
			lightmaps[texnum].workgroup_bounds = (lm_compute_workgroup_bounds_t *)Mem_Alloc (WORKGROUP_BOUNDS_BUFFER_SIZE);
			for (i = 0; i < (LMBLOCK_WIDTH / 8) * (LMBLOCK_HEIGHT / 8); ++i)
			{
				for (j = 0; j < 3; ++j)
				{
					lightmaps[texnum].workgroup_bounds[i].mins[j] = FLT_MAX;
					lightmaps[texnum].workgroup_bounds[i].maxs[j] = -FLT_MAX;
				}
				lightmaps[texnum].workgroup_bounds[i].submodel = LM_WORKGROUP_SUBMODEL_EMPTY;
			}
			for (l = 0; l < LMBLOCK_HEIGHT / LM_CULL_BLOCK_H; l++)
				for (k = 0; k < LMBLOCK_WIDTH / LM_CULL_BLOCK_W; k++)
					for (j = 0; j < 3; ++j)
					{
						lightmaps[texnum].global_bounds[l][k].mins[j] = FLT_MAX;
						lightmaps[texnum].global_bounds[l][k].maxs[j] = -FLT_MAX;
					}
			memset (lightmaps[texnum].cached_light, -1, sizeof (lightmaps[texnum].cached_light));
			memset (used_columns[texnum], 0, sizeof (used_columns[texnum]));
			last_lightmap_allocated = texnum;
		}

		i = SizeToBin (w);
		if (columns[i] < 0 || rows[i] + h - shelf_idx[i] * SHELF_HEIGHT > SHELF_HEIGHT) // need another shelf
		{
			while (used_columns[lightmap_idx[i]][shelf_idx[i]] + BinToSize (i) > LMBLOCK_WIDTH)
			{
				if (++shelf_idx[i] < SHELVES)
					continue;
				shelf_idx[i] = 0;
				if (++lightmap_idx[i] == lightmap_count)
					break;
			}
			if (lightmap_idx[i] == lightmap_count) // need another lightmap
				continue;

			columns[i] = used_columns[lightmap_idx[i]][shelf_idx[i]];
			used_columns[lightmap_idx[i]][shelf_idx[i]] += BinToSize (i);
			rows[i] = shelf_idx[i] * SHELF_HEIGHT;
		}
		*x = columns[i];
		*y = rows[i];
		rows[i] += h;
		return lightmap_idx[i];
	}

	Sys_Error ("AllocBlock: full");
	return 0; // johnfitz -- shut up compiler
}

mvertex_t *r_pcurrentvertbase;
qmodel_t  *currentmodel;

int nColinElim;

/*
===============
R_AssignSurfaceIndex
===============
*/
static void R_AssignSurfaceIndex (msurface_t *surf, uint32_t index, uint32_t *surface_indices, int stride)
{
	int width = (surf->extents[0] >> 4) + 1;
	int height = (surf->extents[1] >> 4) + 1;

	stride -= width;
	while (height-- > 0)
	{
		int i;
		for (i = 0; i < width; i++)
		{
			*surface_indices++ = index;
		}
		surface_indices += stride;
	}
}

/*
===============
R_FillLightstyleTexture
===============
*/
static void R_FillLightstyleTextures (msurface_t *surf, byte **lightstyles, int stride)
{
	int	  smax, tmax;
	byte *lightmap;
	int	  maps;

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	lightmap = surf->samples;

	// add all the lightmaps
	if (lightmap)
	{
		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; ++maps)
		{
			for (int s = 0; s < smax; s += CLAMP (1, smax - s - 1, 8))
				for (int t = 0; t < tmax; t += CLAMP (1, tmax - t - 1, 8))
					lightmaps[surf->lightmaptexturenum]
						.used_lightstyles[(surf->light_t + t) / LM_CULL_BLOCK_H][(surf->light_s + s) / LM_CULL_BLOCK_W][surf->styles[maps]] = true;
			if (maps % 4 != 3)
			{
				byte *outptr = lightstyles[maps / 4 * 3 + maps % 4];
				int	  height = tmax;
				while (height-- > 0)
				{
					int i;
					for (i = 0; i < smax; i++)
					{
						*outptr++ = *lightmap++;
						*outptr++ = *lightmap++;
						*outptr++ = *lightmap++;
						*outptr++ = 0;
					}
					outptr += stride - smax * LIGHTMAP_BYTES;
				}
			}
			else
			{
				for (int height = 0; height < tmax; ++height)
					for (int i = 0; i < smax; i++)
					{
						lightstyles[maps / 4 + 0][i * 4 + 3 + stride * height] = *lightmap++;
						lightstyles[maps / 4 + 1][i * 4 + 3 + stride * height] = *lightmap++;
						lightstyles[maps / 4 + 2][i * 4 + 3 + stride * height] = *lightmap++;
					}
			}
		}
	}
}

/*
===============
R_AssignWorkgroupBounds

FIXME: This doesn't account for moving bmodels
===============
*/
static void R_AssignWorkgroupBounds (msurface_t *surf, int submodel)
{
	struct lightmap_s			  *lm = &lightmaps[surf->lightmaptexturenum];
	lm_compute_workgroup_bounds_t *bounds = lm->workgroup_bounds;
	lm_compute_workgroup_bounds_t *global = &lm->global_bounds[0][0];
	const int					   smax = (surf->extents[0] >> 4) + 1;
	const int					   tmax = (surf->extents[1] >> 4) + 1;

	lm_compute_workgroup_bounds_t surf_bounds;
	for (int i = 0; i < 3; ++i)
	{
		surf_bounds.mins[i] = FLT_MAX;
		surf_bounds.maxs[i] = -FLT_MAX;
	}

	float *v = surf->polys->verts[0];
	for (int i = 0; i < surf->polys->numverts; ++i, v += VERTEXSIZE)
	{
		for (int j = 0; j < 3; ++j)
		{
			if (v[j] < surf_bounds.mins[j])
				surf_bounds.mins[j] = v[j];
			if (v[j] > surf_bounds.maxs[j])
				surf_bounds.maxs[j] = v[j];
		}
	}

	for (int s = 0; s < smax; s += CLAMP (1, smax - s - 1, 8))
	{
		for (int t = 0; t < tmax; t += CLAMP (1, tmax - t - 1, 8))
		{
			const int					   workgroup_x = (surf->light_s + s) / 8;
			const int					   workgroup_y = (surf->light_t + t) / 8;
			lm_compute_workgroup_bounds_t *workgroup_bounds = bounds + workgroup_x + (workgroup_y * (LMBLOCK_WIDTH / 8));
			const int					   cullblock_x = (surf->light_s + s) / LM_CULL_BLOCK_W;
			const int					   cullblock_y = (surf->light_t + t) / LM_CULL_BLOCK_H;
			lm_compute_workgroup_bounds_t *global_bounds = global + cullblock_x + (cullblock_y * (LMBLOCK_WIDTH / LM_CULL_BLOCK_W));
			// submodel surfaces have model space bounds that the shader transforms with the current entity transform,
			// a workgroup spanning more than one coordinate space can't be culled with a single AABB
			if ((workgroup_bounds->submodel == LM_WORKGROUP_SUBMODEL_EMPTY) || (workgroup_bounds->submodel == (uint32_t)submodel))
				workgroup_bounds->submodel = submodel;
			else
				workgroup_bounds->submodel = LM_WORKGROUP_SUBMODEL_MIXED;
			for (int i = 0; i < 3; ++i)
			{
				if (surf_bounds.mins[i] < workgroup_bounds->mins[i])
					workgroup_bounds->mins[i] = surf_bounds.mins[i];
				if (surf_bounds.maxs[i] > workgroup_bounds->maxs[i])
					workgroup_bounds->maxs[i] = surf_bounds.maxs[i];
			}
			if (submodel == 0)
			{
				for (int i = 0; i < 3; ++i)
				{
					if (surf_bounds.mins[i] < global_bounds->mins[i])
						global_bounds->mins[i] = surf_bounds.mins[i];
					if (surf_bounds.maxs[i] > global_bounds->maxs[i])
						global_bounds->maxs[i] = surf_bounds.maxs[i];
				}
			}
			else
				lm->block_has_submodels[cullblock_y][cullblock_x] = true;
		}
	}
}

/*
================
UpdateIndirectStructs
================
*/
static qboolean R_SurfaceUsesEmissiveAtlas (const msurface_t *surface)
{
	return surface->emissive_influence || surface->emissive_bounce_influence;
}

static void UpdateIndirectStructs (msurface_t *surf, qboolean is_bmodel, qboolean is_world_model, qboolean emissive_grouping)
{
	static int last;
	int		   i;
	const qboolean unique_liquid = r_emissive_rt_liquid_receivers.value > 0.0f && (surf->flags & SURF_DRAWTURB);
	const byte world_flags =
		(is_world_model ? INDIRECT_WORLD_MODEL : 0) |
		(is_world_model && emissive_grouping && R_SurfaceUsesEmissiveAtlas (surf) ? INDIRECT_EMISSIVE_INFLUENCE : 0);
	if (!unique_liquid && last < used_indirect_draws && indirect_draws[last].lightmap_idx == surf->lightmaptexturenum &&
		indirect_draws[last].texture == surf->texinfo->texture && indirect_draws[last].is_bmodel == is_bmodel &&
		indirect_draws[last].world_flags == world_flags)
	{
		surf->indirect_idx = last;
		indirect_draws[last].max_indices += 3 * (surf->numedges - 2);
		return;
	}
	for (i = unique_liquid ? used_indirect_draws : 0; i < used_indirect_draws; i++)
	{
		if (indirect_draws[i].lightmap_idx == surf->lightmaptexturenum && indirect_draws[i].texture == surf->texinfo->texture &&
			indirect_draws[i].is_bmodel == is_bmodel && indirect_draws[i].world_flags == world_flags)
		{
			surf->indirect_idx = last = i;
			indirect_draws[i].max_indices += 3 * (surf->numedges - 2);
			return;
		}
	}
	if (i == MAX_INDIRECT_DRAWS - 1)
	{
		indirect_ready = false;
		return;
	}
	++used_indirect_draws;
	surf->indirect_idx = last = i;
	indirect_draws[i].texture = surf->texinfo->texture;
	indirect_draws[i].surface = unique_liquid ? surf : NULL;
	indirect_draws[i].lightmap_idx = surf->lightmaptexturenum;
	indirect_draws[i].is_bmodel = is_bmodel;
	indirect_draws[i].world_flags = world_flags;
	indirect_draws[i].max_indices = 3 * (surf->numedges - 2);
}

/*
================
PrepareIndirectDraws
================
*/
static void PrepareIndirectDraws ()
{
	int total_indices = 0;
	for (int i = 0; i < used_indirect_draws; i++)
	{
		initial_indirect_buffer[i].indexCount = 0;
		initial_indirect_buffer[i].instanceCount = 1;
		initial_indirect_buffer[i].firstIndex = total_indices;
		initial_indirect_buffer[i].vertexOffset = 0;
		initial_indirect_buffer[i].firstInstance = 0;
		total_indices += indirect_draws[i].max_indices;
	}
}

/*
========================
GL_CreateSurfaceLightmap
========================
*/
static void GL_CreateSurfaceLightmap (msurface_t *surf, uint32_t surface_index)
{
	int		  i;
	byte	 *base;
	byte	 *lightstyles[MAXLIGHTMAPS * 3 / 4];
	uint32_t *surface_indices;

	assert (!(surf->flags & SURF_DRAWTILED));

	base = lightmaps[surf->lightmaptexturenum].data;
	base += (surf->light_t * LMBLOCK_WIDTH + surf->light_s) * LIGHTMAP_BYTES;
	R_BuildLightMap (surf, base, LMBLOCK_WIDTH * LIGHTMAP_BYTES);

	surface_indices = lightmaps[surf->lightmaptexturenum].surface_indices;
	surface_indices += (surf->light_t * LMBLOCK_WIDTH + surf->light_s);
	R_AssignSurfaceIndex (surf, surface_index, surface_indices, LMBLOCK_WIDTH);

	for (i = 0; i < MAXLIGHTMAPS * 3 / 4; ++i)
	{
		lightstyles[i] = lightmaps[surf->lightmaptexturenum].lightstyle_data[i];
		lightstyles[i] += (surf->light_t * LMBLOCK_WIDTH + surf->light_s) * LIGHTMAP_BYTES;
	}
	R_FillLightstyleTextures (surf, lightstyles, LMBLOCK_WIDTH * LIGHTMAP_BYTES);
}

/*
================
BuildSurfaceDisplayList -- called at level load time
================
*/
static void BuildSurfaceDisplayList (msurface_t *fa)
{
	int		  i, lindex, lnumverts;
	medge_t	 *pedges, *r_pedge;
	float	 *vec;
	float	  s, t, s0, t0, sdiv, tdiv;
	glpoly_t *poly;
	float	 *poly_vert;

	// reconstruct the polygon
	pedges = currentmodel->edges;
	lnumverts = fa->numedges;

	//
	// draw texture
	//
	poly = (glpoly_t *)Mem_Alloc (sizeof (glpoly_t) + (lnumverts - 4) * VERTEXSIZE * sizeof (float));
	poly->next = fa->polys;
	fa->polys = poly;
	poly->numverts = lnumverts;

	if (fa->flags & SURF_DRAWTURB)
	{
		// match Mod_PolyForUnlitSurface
		s0 = t0 = 0.f;
		sdiv = tdiv = 128.f;
	}
	else
	{
		s0 = fa->texinfo->vecs[0][3];
		t0 = fa->texinfo->vecs[1][3];
		sdiv = fa->texinfo->texture->width;
		tdiv = fa->texinfo->texture->height;
	}

	for (i = 0; i < lnumverts; i++)
	{
		lindex = currentmodel->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];
			vec = r_pcurrentvertbase[r_pedge->v[0]].position;
		}
		else
		{
			r_pedge = &pedges[-lindex];
			vec = r_pcurrentvertbase[r_pedge->v[1]].position;
		}
		s = DotProduct (vec, fa->texinfo->vecs[0]) + s0;
		s /= sdiv;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + t0;
		t /= tdiv;

		poly_vert = &poly->verts[0][0] + (i * VERTEXSIZE);
		VectorCopy (vec, poly_vert);
		poly_vert[3] = s;
		poly_vert[4] = t;

		// Q64 RERELEASE texture shift
		if (fa->texinfo->texture->shift > 0)
		{
			poly_vert[3] /= (2 * fa->texinfo->texture->shift);
			poly_vert[4] /= (2 * fa->texinfo->texture->shift);
		}

		//
		// lightmap texture coordinates
		//
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s -= fa->texturemins[0];
		s += fa->light_s * 16;
		s += 8;
		s /= LMBLOCK_WIDTH * 16; // fa->texinfo->texture->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t -= fa->texturemins[1];
		t += fa->light_t * 16;
		t += 8;
		t /= LMBLOCK_HEIGHT * 16; // fa->texinfo->texture->height;

		poly_vert[5] = s;
		poly_vert[6] = t;
	}

	// johnfitz -- removed gl_keeptjunctions code

	poly->numverts = lnumverts;
}

/*
==================
R_AllocateLightmapComputeBuffers
==================
*/
void R_AllocateLightmapComputeBuffers ()
{
	size_t lightstyles_buffer_size = MAX_LIGHTSTYLES * sizeof (float) * 2;
	size_t lights_buffer_size = MAX_DLIGHTS * 2 * sizeof (lm_compute_light_t) * 2;
	size_t submodel_transforms_buffer_size = MAX_MODELS * 12 * sizeof (float) * 2;
	size_t bmodel_instances_buffer_size = MAX_MODELS * sizeof (bmodel_instance_t) * 2;

	Sys_Printf ("Allocating lightstyles buffer (%u KB)\n", (int)lightstyles_buffer_size / 1024);
	Sys_Printf ("Allocating lights buffer (%u KB)\n", (int)lights_buffer_size / 1024);
	Sys_Printf ("Allocating submodel transforms buffer (%u KB)\n", (int)submodel_transforms_buffer_size / 1024);
	Sys_Printf ("Allocating bmodel instances buffer (%u KB)\n", (int)bmodel_instances_buffer_size / 1024);

	buffer_create_info_t buffer_create_infos[4] = {
		{&lightstyles_scales_buffer, lightstyles_buffer_size, 0, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, (void **)&lightstyles_scales_buffer_mapped, NULL,
		 "Lightstyle scales"},
		{&lights_buffer, lights_buffer_size, 0, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, (void **)&lights_buffer_mapped, NULL, "Lights"},
		{&submodel_transforms_buffer, submodel_transforms_buffer_size, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, (void **)&submodel_transforms_buffer_mapped, NULL,
		 "Submodel transforms"},
		{&bmodel_instances_buffer, bmodel_instances_buffer_size, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, (void **)&bmodel_instances_buffer_mapped, NULL,
		 "BModel instances"},
	};
	R_CreateBuffers (
		countof (buffer_create_infos), buffer_create_infos, &frame_upload_buffers_memory, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
		VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &num_vulkan_bmodel_allocations, "Frame upload buffers");
}

/*
==================
GL_AllocateSurfaceDataBuffer
==================
*/
static lm_compute_surface_data_t *GL_AllocateSurfaceDataBuffer ()
{
	size_t buffer_size = num_surfaces * sizeof (lm_compute_surface_data_t);

	R_FreeBuffer (surface_data_buffer, &surface_data_buffer_memory, &num_vulkan_bmodel_allocations);

	Sys_Printf ("Allocating lightmap compute surface data (%u KB)\n", (int)buffer_size / 1024);
	R_CreateBuffer (
		&surface_data_buffer, &surface_data_buffer_memory, buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL, "Lightmap compute surface data");

	VkCommandBuffer			   command_buffer;
	VkBuffer				   staging_buffer;
	int						   staging_offset;
	lm_compute_surface_data_t *staging_mem = (lm_compute_surface_data_t *)R_StagingAllocate (buffer_size, 1, &command_buffer, &staging_buffer, &staging_offset);

	VkBufferCopy region;
	region.srcOffset = staging_offset;
	region.dstOffset = 0;
	region.size = buffer_size;
	vkCmdCopyBuffer (command_buffer, staging_buffer, surface_data_buffer, 1, &region);

	return staging_mem;
}

/*
==================
GL_AllocateSurfaceSubmodelsBuffer
==================
*/
static void GL_AllocateSurfaceSubmodelsBuffer ()
{
	size_t buffer_size = num_surfaces * sizeof (uint32_t);

	R_FreeBuffer (surface_submodels_buffer, &surface_submodels_buffer_memory, &num_vulkan_bmodel_allocations);

	Sys_Printf ("Allocating surface submodel indices (%u KB)\n", (int)buffer_size / 1024);
	R_CreateBuffer (
		&surface_submodels_buffer, &surface_submodels_buffer_memory, buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL, "Surface submodel indices");
}

/*
==================
GL_AllocateIndirectBuffer
==================
*/
static VkDrawIndexedIndirectCommand *GL_AllocateIndirectBuffer (int num_used_indirect_draws)
{
	size_t buffer_size = num_used_indirect_draws * sizeof (VkDrawIndexedIndirectCommand);

	R_FreeBuffer (indirect_buffer, &indirect_buffer_memory, &num_vulkan_bmodel_allocations);

	Sys_Printf ("Allocating indirect draw data (%u KB, %d draws)\n", (int)buffer_size / 1024, num_used_indirect_draws);
	R_CreateBuffer (
		&indirect_buffer, &indirect_buffer_memory, buffer_size,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
		&num_vulkan_bmodel_allocations, NULL, "Indirect draw data");

	VkCommandBuffer				  command_buffer;
	VkBuffer					  staging_buffer;
	int							  staging_offset;
	VkDrawIndexedIndirectCommand *staging_mem =
		(VkDrawIndexedIndirectCommand *)R_StagingAllocate (buffer_size, 1, &command_buffer, &staging_buffer, &staging_offset);

	VkBufferCopy region;
	region.srcOffset = staging_offset;
	region.dstOffset = 0;
	region.size = buffer_size;
	vkCmdCopyBuffer (command_buffer, staging_buffer, indirect_buffer, 1, &region);

	return staging_mem;
}

/*
==================
GL_AllocateWorkgroupBoundsBuffers
==================
*/
static void GL_AllocateWorkgroupBoundsBuffers ()
{
	VkResult err;

	if (workgroup_bounds_buffer_memory.handle != VK_NULL_HANDLE)
	{
		R_FreeVulkanMemory (&workgroup_bounds_buffer_memory, &num_vulkan_bmodel_allocations);
	}

	for (int i = 0; i < lightmap_count; i++)
	{
		ZEROED_STRUCT (VkBufferCreateInfo, buffer_create_info);
		buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_create_info.size = WORKGROUP_BOUNDS_BUFFER_SIZE;
		buffer_create_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

		err = vkCreateBuffer (vulkan_globals.device, &buffer_create_info, NULL, &lightmaps[i].workgroup_bounds_buffer);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateBuffer failed with code %i", (int)err);
		GL_SetObjectName ((uint64_t)lightmaps[i].workgroup_bounds_buffer, VK_OBJECT_TYPE_BUFFER, "Workgroup bounds buffer");
	}

	int aligned_size = 0;
	if (lightmap_count > 0)
	{
		VkMemoryRequirements memory_requirements;
		vkGetBufferMemoryRequirements (vulkan_globals.device, lightmaps[0].workgroup_bounds_buffer, &memory_requirements);

		aligned_size = q_align (memory_requirements.size, memory_requirements.alignment);

		ZEROED_STRUCT (VkMemoryAllocateInfo, memory_allocate_info);
		memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		memory_allocate_info.allocationSize = lightmap_count * aligned_size;
		memory_allocate_info.memoryTypeIndex = GL_MemoryTypeFromProperties (memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);

		R_AllocateVulkanMemory (&workgroup_bounds_buffer_memory, &memory_allocate_info, VULKAN_MEMORY_TYPE_DEVICE, &num_vulkan_bmodel_allocations);
		GL_SetObjectName ((uint64_t)workgroup_bounds_buffer_memory.handle, VK_OBJECT_TYPE_DEVICE_MEMORY, "Workgroup bounds memory");
	}

	for (int i = 0; i < lightmap_count; i++)
	{
		err = vkBindBufferMemory (vulkan_globals.device, lightmaps[i].workgroup_bounds_buffer, workgroup_bounds_buffer_memory.handle, aligned_size * i);
		if (err != VK_SUCCESS)
			Sys_Error ("vkBindBufferMemory failed with code %i", (int)err);
	}
}

/*
===============
R_InitIndirectIndexBuffer
===============
*/
static void R_InitIndirectIndexBuffer (uint32_t size)
{
	R_FreeBuffer (indirect_index_buffer, &indirect_index_buffer_memory, &num_vulkan_bmodel_allocations);

	Sys_Printf ("Allocating indirect IBs (%u KB)\n", size / 1024);
	R_CreateBuffer (
		&indirect_index_buffer, &indirect_index_buffer_memory, size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL, "Indirect indices");
}

/*
===============
R_InitVisibilityBuffers (one bit per surface)
===============
*/
static void R_InitVisibilityBuffers (uint32_t size)
{
	R_FreeBuffer (dyn_visibility_buffer, &dyn_visibility_buffer_memory, &num_vulkan_bmodel_allocations);

	size = (size + 255) / 256 * 256 * 2;

	Sys_Printf ("Allocating visibility buffers (%u KB)\n", size / 1024);
	R_CreateBuffer (
		&dyn_visibility_buffer, &dyn_visibility_buffer_memory, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
		VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &num_vulkan_bmodel_allocations, NULL, "Dynamic visibility");

	void	*data;
	VkResult err = vkMapMemory (vulkan_globals.device, dyn_visibility_buffer_memory.handle, 0, size, 0, &data);
	if (err != VK_SUCCESS)
		Sys_Error ("vkMapMemory failed with code %i", (int)err);

	dyn_visibility_view = (unsigned char *)data;
	dyn_visibility_offset = size / 2;
}

/*
===============
R_UploadVisibility
===============
*/
static void R_UploadVisibility (byte *data, uint32_t size)
{
	// TODO: do we really need a read barrier for surfvis uploading ?
	Atomic_ReadBarrier ();

	memcpy (dyn_visibility_view + current_compute_buffer_index * dyn_visibility_offset, data, size);
	ZEROED_STRUCT (VkMappedMemoryRange, range);
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = dyn_visibility_buffer_memory.handle;
	range.size = VK_WHOLE_SIZE;
	vkFlushMappedMemoryRanges (vulkan_globals.device, 1, &range);
}

/*
===============
GL_SortSurfaces

Sorts surfs by number of used lightstyles, submodel and 3D position, then allocates lm blocks in that order and sets image bounds.
Grouping the surfaces of each movable submodel keeps them out of the lightmap compute workgroups of world surfaces so dlights
can be culled with a single coordinate space per workgroup
===============
*/
typedef struct
{
	msurface_t *surf;
	uint64_t	sortkey;
} surf_sort;

static int prepare_3d_interleave (int x) // bin x..xabcdefghij --> 0000a00b00c00d00e00f00g00h00i00j
{
	x = (x | (x << 16)) & 0x030000FF;
	x = (x | (x << 8)) & 0x0300F00F;
	x = (x | (x << 4)) & 0x030C30C3;
	x = (x | (x << 2)) & 0x09249249;
	return x;
}

static void GL_SortSurfaces (void)
{
	int			i;
	unsigned	j;
	msurface_t *surf;
	TEMP_ALLOC (surf_sort, surfs, num_surfaces * 2);
	int used_surfs = 0;
	int sort_bins[6][256];
	memset (sort_bins, 0, sizeof (sort_bins));
	float scale_x = 500.0f / q_max (1.0f, q_max (fabsf (cl.worldmodel->mins[0]), fabsf (cl.worldmodel->maxs[0])));
	float scale_y = 500.0f / q_max (1.0f, q_max (fabsf (cl.worldmodel->mins[1]), fabsf (cl.worldmodel->maxs[1])));
	float scale_z = 500.0f / q_max (1.0f, q_max (fabsf (cl.worldmodel->mins[2]), fabsf (cl.worldmodel->maxs[2])));
	int	  current_submodel = 0;
	for (j = 1; j < MAX_MODELS; j++)
	{
		qmodel_t *m = cl.model_precache[j];
		if (!m)
			break;
		if (m->name[0] == '*')
			continue;
		for (i = 0; i < m->numsurfaces; i++)
		{
			int submodel = 0;
			if (j == 1) // the worldmodel surface array also contains all movable submodel surfaces
			{
				while (((current_submodel + 1) < m->numsubmodels) && (i >= m->submodels[current_submodel + 1].firstface))
					++current_submodel;
				submodel = q_min (current_submodel, 0xFFFF);
			}
			surf = &m->surfaces[i];
			if (!(surf->flags & SURF_DRAWTILED))
			{
				int		 lindex = m->surfedges[surf->firstedge];
				float	*vec;
				medge_t *pedges, *r_pedge;
				pedges = m->edges;

				if (lindex > 0)
				{
					r_pedge = &pedges[lindex];
					vec = m->vertexes[r_pedge->v[0]].position;
				}
				else
				{
					r_pedge = &pedges[-lindex];
					vec = m->vertexes[r_pedge->v[1]].position;
				}

				int x = prepare_3d_interleave (((int)(vec[0] * scale_x)) + (1 << 9));
				int y = prepare_3d_interleave (((int)(vec[1] * scale_y)) + (1 << 9));
				int z = prepare_3d_interleave (((int)(vec[2] * scale_z)) + (1 << 9));

				unsigned last_lightstyle;
				for (last_lightstyle = 0; last_lightstyle < 3; last_lightstyle++) // saturate at 3, not 4
					if (surf->styles[last_lightstyle] == 0xFF)
						break;
				surfs[used_surfs].surf = surf;
				uint64_t sortkey = ((uint64_t)(3 - last_lightstyle) << 46) | ((uint64_t)submodel << 30) | (uint64_t)(z | y << 1 | x << 2);
				surfs[used_surfs++].sortkey = sortkey;
				for (int pass = 0; pass < 6; ++pass)
					sort_bins[pass][(sortkey >> (8 * pass)) % 256] += 1;
			}
		}
	}
	for (int pass = 0; pass < 6; ++pass)
	{
		surf_sort *from = pass % 2 ? surfs + num_surfaces : surfs;
		surf_sort *to = pass % 2 ? surfs : surfs + num_surfaces;
		for (i = 1; i < 256; ++i)
			sort_bins[pass][i] += sort_bins[pass][i - 1];
		for (i = used_surfs - 1; i >= 0; --i)
		{
			int key = (from[i].sortkey >> (8 * pass)) % 256;
			sort_bins[pass][key] -= 1;
			to[sort_bins[pass][key]] = from[i];
		}
	}
	for (i = 0; i < used_surfs; ++i)
	{
		surf = surfs[i].surf;
		surf->lightmaptexturenum = AllocBlock ((surf->extents[0] >> 4) + 1, (surf->extents[1] >> 4) + 1, &surf->light_s, &surf->light_t);
		for (j = 0; j < (3 - (unsigned)(surfs[i].sortkey >> 46)) + 1; j++)
		{
			unsigned short *w = &lightmaps[surf->lightmaptexturenum].lightstyle_rectused[j].w;
			unsigned short *h = &lightmaps[surf->lightmaptexturenum].lightstyle_rectused[j].h;
			*w = q_max (*w, (surf->extents[0] >> 4) + 1 + surf->light_s);
			*h = q_max (*h, (surf->extents[1] >> 4) + 1 + surf->light_t);
		}
	}
	TEMP_FREE (surfs);
}

/*
==================
GL_BuildLightmaps -- called at level load time

Builds the lightmap texture
with all the surfaces from all brush models
==================
*/
void GL_BuildLightmaps (void)
{
	int						   i, j;
	uint32_t				   surface_index = 0;
	lm_compute_surface_data_t *surface_data;
	msurface_t				  *surf;

	GL_WaitForDeviceIdle ();
	R_FreeEmissiveBandlimitDescriptorSets ();
	R_FreeEmissiveOccluderDescriptorSets ();
	R_FreeEmissiveBrushReceivers ();
	emissive_detail_scale = r_emissive_rt_resolution.value <= 1.0f ? 1 : r_emissive_rt_resolution.value <= 2.0f ? 2 : 4;

	r_framecount = 1; // no dlightcache

	// Spike -- wipe out all the lightmap data (johnfitz -- the gltexture objects were already freed by Mod_ClearAll)
	for (i = 0; i < lightmap_count; i++)
	{
		Mem_Free (lightmaps[i].data);
		R_FreeDescriptorSet (lightmaps[i].descriptor_set, &vulkan_globals.lightmap_compute_set_layout);
		if (lightmaps[i].emissive_coarse_descriptor_set != VK_NULL_HANDLE)
			R_FreeDescriptorSet (lightmaps[i].emissive_coarse_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
		if (lightmaps[i].emissive_detail_descriptor_set != VK_NULL_HANDLE)
			R_FreeDescriptorSet (lightmaps[i].emissive_detail_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
		if (lightmaps[i].emissive_transient_descriptor_set != VK_NULL_HANDLE)
			R_FreeDescriptorSet (lightmaps[i].emissive_transient_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
		if (lightmaps[i].emissive_transient_detail_descriptor_set != VK_NULL_HANDLE)
			R_FreeDescriptorSet (lightmaps[i].emissive_transient_detail_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
		if (lightmaps[i].emissive_radiance_overlay_descriptor_set != VK_NULL_HANDLE)
			R_FreeDescriptorSet (lightmaps[i].emissive_radiance_overlay_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
		if (lightmaps[i].emissive_radiance_overlay_detail_descriptor_set != VK_NULL_HANDLE)
			R_FreeDescriptorSet (lightmaps[i].emissive_radiance_overlay_detail_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
		if (lightmaps[i].emissive_radiance_descriptor_set != VK_NULL_HANDLE)
			R_FreeDescriptorSet (lightmaps[i].emissive_radiance_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
		if (lightmaps[i].emissive_radiance_detail_descriptor_set != VK_NULL_HANDLE)
			R_FreeDescriptorSet (lightmaps[i].emissive_radiance_detail_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
		if (lightmaps[i].workgroup_bounds_buffer != VK_NULL_HANDLE)
			vkDestroyBuffer (vulkan_globals.device, lightmaps[i].workgroup_bounds_buffer, NULL);
	}
	R_DeleteEmissiveBounceResources ();

	Mem_Free (lightmaps);
	lightmaps = NULL;
	last_lightmap_allocated = 0;
	lightmap_count = 0;
	emissive_detail_budget_limited = false;
	emissive_bandlimit_active = false;
	emissive_bandlimit_budget_limited = false;
	emissive_bandlimit_logical_bytes = emissive_bandlimit_allocated_bytes = emissive_bandlimit_peak_bytes = 0;
	SAFE_FREE (emissive_logical_tiles);
	SAFE_FREE (emissive_logical_tile_sources);
	SAFE_FREE (emissive_light_styles);
	SAFE_FREE (emissive_cacheable_lights);
	SAFE_FREE (emissive_light_modulations);
	SAFE_FREE (emissive_radiance_tiles);
	num_emissive_logical_tiles = 0;
	num_emissive_logical_tiles_total = 0;
	num_emissive_logical_tile_sources = 0;
	emissive_logical_tiles_built = false;
	num_emissive_radiance_tiles = 0;
	num_emissive_radiance_source_links = 0;
	num_emissive_radiance_tile_groups = 0;
	max_emissive_radiance_groups_per_tile = 0;
	num_emissive_modulation_groups = 0;
	emissive_radiance_coarse_pending = false;
	emissive_radiance_detail_pending = false;
	emissive_radiance_force_all_styled_tiles = false;
	emissive_modulations_pending = false;
	emissive_radiance_logged = false;
	emissive_visibility_available = false;
	emissive_radiance_cpu_time_us = 0;
	GL_ResetEmissiveRadianceTimestamp ();
	R_FreeBuffer (emissive_modulations_buffer, &emissive_modulations_buffer_memory, &num_vulkan_bmodel_allocations);
	R_FreeBuffer (emissive_visibility_buffer, &emissive_visibility_buffer_memory, &num_vulkan_bmodel_allocations);
	R_FreeBuffer (emissive_radiance_tiles_buffer, &emissive_radiance_tiles_buffer_memory, &num_vulkan_bmodel_allocations);
	emissive_modulations_buffer = VK_NULL_HANDLE;
	emissive_visibility_buffer = VK_NULL_HANDLE;
	emissive_radiance_tiles_buffer = VK_NULL_HANDLE;
	SAFE_FREE (transient_emissive_lights);
	SAFE_FREE (previous_transient_emissive_lights);
	SAFE_FREE (transient_emissive_light_ids);
	SAFE_FREE (previous_transient_emissive_light_ids);
	SAFE_FREE (transient_emissive_light_seeds);
	SAFE_FREE (transient_emissive_tiles);
	SAFE_FREE (transient_emissive_tile_sources);
	SAFE_FREE (transient_emissive_tile_surface_offsets);
	SAFE_FREE (transient_emissive_tile_surfaces);
	SAFE_FREE (transient_emissive_tile_generations);
	SAFE_FREE (transient_emissive_tile_indices);
	SAFE_FREE (transient_emissive_surface_influence_counts);
	SAFE_FREE (transient_emissive_surface_deltas);
	SAFE_FREE (transient_emissive_surface_delta_generations);
	SAFE_FREE (transient_emissive_touched_surfaces);
	num_transient_emissive_lights = 0;
	num_previous_transient_emissive_lights = 0;
	num_transient_emissive_tiles = 0;
	num_transient_emissive_tile_sources = 0;
	num_transient_emissive_tile_surfaces = 0;
	num_transient_emissive_total_tiles = 0;
	transient_emissive_tile_generation = 0;
	num_transient_emissive_touched_surfaces = 0;
	transient_emissive_surface_delta_generation = 0;
	transient_emissive_tile_surface_worldmodel = NULL;
	transient_emissive_pending = false;
	transient_emissive_detail_pending = false;
	transient_emissive_detail_ready = false;
	transient_emissive_generation = 0;
	transient_emissive_detail_published_generation = 0;
	transient_emissive_rejected_publications = 0;
	transient_emissive_initialized = false;
	transient_emissive_detail_cache_copied = false;
	transient_emissive_force_refresh = false;
	GL_ResetEmissiveTransientTimestamp ();
	R_FreeBuffer (transient_emissive_lights_buffer, &transient_emissive_lights_buffer_memory, &num_vulkan_bmodel_allocations);
	R_FreeBuffer (transient_emissive_light_seeds_buffer, &transient_emissive_light_seeds_buffer_memory, &num_vulkan_bmodel_allocations);
	R_FreeBuffer (transient_emissive_tiles_buffer, &transient_emissive_tiles_buffer_memory, &num_vulkan_bmodel_allocations);
	R_FreeBuffer (transient_emissive_tile_sources_buffer, &transient_emissive_tile_sources_buffer_memory, &num_vulkan_bmodel_allocations);
	transient_emissive_lights_buffer = VK_NULL_HANDLE;
	transient_emissive_light_seeds_buffer = VK_NULL_HANDLE;
	transient_emissive_tiles_buffer = VK_NULL_HANDLE;
	transient_emissive_tile_sources_buffer = VK_NULL_HANDLE;
	transient_emissive_lights_capacity = 0;
	transient_emissive_light_seeds_capacity = 0;
	transient_emissive_tiles_capacity = 0;
	transient_emissive_tile_sources_capacity = 0;
	num_surfaces = 0;
	memset (columns, -1, sizeof (columns));
	memset (lightmap_idx, 0, sizeof (lightmap_idx));
	memset (shelf_idx, 0, sizeof (shelf_idx));
	used_indirect_draws = 0;
	indirect_ready = true;
	// Emissive receiver classification activates and regroups these draws later.
	indirect_emissive_grouping = false;
	indirect_bmodel_start = INT_MAX;
	used_deps_data = 0;
	Mem_Free (brush_deps_data);

	num_worldmodel_submodels = q_min (cl.model_precache[1]->numsubmodels, MAX_MODELS);

	for (i = 1; i < MAX_MODELS; ++i)
	{
		qmodel_t *m = cl.model_precache[i];
		if (!m)
			break;
		if (m->name[0] == '*')
		{
			indirect_bmodel_start = q_min (indirect_bmodel_start, m->firstmodelsurface);
			continue;
		}
		num_surfaces += m->numsurfaces; // note: allocates unused space for SURF_DRAWTILED surfs
	}

	GL_SortSurfaces ();

	GL_AllocateSurfaceSubmodelsBuffer ();
	TEMP_ALLOC_ZEROED (uint32_t, surface_submodels, num_surfaces);

	surface_data = GL_AllocateSurfaceDataBuffer ();

	R_StagingBeginCopy ();
	unsigned int varray_index = 0;
	int			 current_submodel = 0;
	for (j = 1; j < MAX_MODELS; j++)
	{
		qmodel_t *m = cl.model_precache[j];
		if (!m)
			break;
		if (m->name[0] == '*')
			continue;
		r_pcurrentvertbase = m->vertexes;
		currentmodel = m;
		for (i = 0; i < m->numsurfaces; i++)
		{
			int submodel = 0;
			if (j == 1) // the worldmodel surface array also contains all movable submodel surfaces
			{
				while (((current_submodel + 1) < m->numsubmodels) && (i >= m->submodels[current_submodel + 1].firstface))
					++current_submodel;
				if (current_submodel < num_worldmodel_submodels)
					submodel = current_submodel;
			}
			surface_submodels[surface_index] = submodel;

			surf = &m->surfaces[i];
			if (!(surf->flags & SURF_DRAWTILED))
			{
				const qboolean no_dlights = j > 1;
				GL_CreateSurfaceLightmap (surf, surface_index | 0x80000000 * no_dlights);
				BuildSurfaceDisplayList (surf);
				if (!no_dlights)
					R_AssignWorkgroupBounds (surf, submodel);
			}
			if (indirect_ready)
				UpdateIndirectStructs (surf, INDIRECT_ZBIAS && surface_index >= indirect_bmodel_start, j == 1 && submodel == 0, indirect_emissive_grouping);

			lm_compute_surface_data_t *surf_data = &surface_data[surface_index];
			surf_data->packed_lightstyles = ((uint32_t)(surf->styles[0]) << 0) | ((uint32_t)(surf->styles[1]) << 8) | ((uint32_t)(surf->styles[2]) << 16) |
											((uint32_t)(surf->styles[3]) << 24);
			for (int k = 0; k < 3; ++k)
				surf_data->normal[k] = surf->plane->normal[k];
			surf_data->dist = surf->plane->dist;
			surf_data->packed_light_st = ((surf->light_s) & 0xFFFF) | (((surf->light_t) & 0xFFFF) << 16);
			surf_data->packed_tex_edgecount = surf->indirect_idx | !!(surf->flags & SURF_PLANEBACK) << 15 | surf->numedges << 16;
			surf->vbo_firstvert = varray_index;
			surf_data->vbo_offset = surf->vbo_firstvert;
			if (surf->numedges > 65535)
				indirect_ready = false;
			varray_index += surf->numedges;

			Vector4Copy (surf->texinfo->vecs[0], surf_data->vecs[0]);
			Vector4Copy (surf->texinfo->vecs[1], surf_data->vecs[1]);
			surf_data->vecs[0][3] -= surf->texturemins[0];
			surf_data->vecs[1][3] -= surf->texturemins[1];

			surface_index += 1;
		}
	}

	R_StagingEndCopy ();

	R_StagingUploadBuffer (surface_submodels_buffer, num_surfaces * sizeof (uint32_t), (byte *)surface_submodels);
	TEMP_FREE (surface_submodels);
}

/*
==================
GL_SetupIndirectDraws
==================
*/
void GL_SetupIndirectDraws ()
{

	if (!indirect_ready)
	{
		Con_Warning ("map exceeds indirect dispatch limits\n");
		return;
	}

	PrepareIndirectDraws ();
	VkDrawIndexedIndirectCommand *hw_indirect_buffer = GL_AllocateIndirectBuffer (used_indirect_draws);
	R_StagingBeginCopy ();
	memcpy (hw_indirect_buffer, initial_indirect_buffer, used_indirect_draws * sizeof (VkDrawIndexedIndirectCommand));
	R_StagingEndCopy ();

	R_InitIndirectIndexBuffer ((initial_indirect_buffer[used_indirect_draws - 1].firstIndex + indirect_draws[used_indirect_draws - 1].max_indices) * 4);
	R_InitVisibilityBuffers ((cl.worldmodel->numsurfaces + 31) / 8);

	if (vulkan_globals.indirect_compute_desc_set != VK_NULL_HANDLE)
		R_FreeDescriptorSet (vulkan_globals.indirect_compute_desc_set, &vulkan_globals.indirect_compute_set_layout);
	vulkan_globals.indirect_compute_desc_set = R_AllocateDescriptorSet (&vulkan_globals.indirect_compute_set_layout);

	ZEROED_STRUCT (VkDescriptorBufferInfo, indirect_draw_buffer_info);
	indirect_draw_buffer_info.buffer = indirect_buffer;
	indirect_draw_buffer_info.offset = 0;
	indirect_draw_buffer_info.range = VK_WHOLE_SIZE;

	ZEROED_STRUCT (VkDescriptorBufferInfo, surfaces_buffer_info);
	surfaces_buffer_info.buffer = surface_data_buffer;
	surfaces_buffer_info.offset = 0;
	surfaces_buffer_info.range = num_surfaces * sizeof (lm_compute_surface_data_t);

	ZEROED_STRUCT (VkDescriptorBufferInfo, visibility_buffer_info);
	visibility_buffer_info.buffer = dyn_visibility_buffer;
	visibility_buffer_info.offset = 0;
	visibility_buffer_info.range = VK_WHOLE_SIZE;

	ZEROED_STRUCT (VkDescriptorBufferInfo, index_buffer_info);
	index_buffer_info.buffer = indirect_index_buffer;
	index_buffer_info.offset = 0;
	index_buffer_info.range = VK_WHOLE_SIZE;

	ZEROED_STRUCT (VkDescriptorBufferInfo, indirect_surface_submodels_buffer_info);
	indirect_surface_submodels_buffer_info.buffer = surface_submodels_buffer;
	indirect_surface_submodels_buffer_info.offset = 0;
	indirect_surface_submodels_buffer_info.range = num_surfaces * sizeof (uint32_t);

	ZEROED_STRUCT (VkDescriptorBufferInfo, bmodel_instances_buffer_info);
	bmodel_instances_buffer_info.buffer = bmodel_instances_buffer;
	bmodel_instances_buffer_info.offset = 0;
	bmodel_instances_buffer_info.range = VK_WHOLE_SIZE;

	ZEROED_STRUCT_ARRAY (VkWriteDescriptorSet, indirect_d, 6);

	indirect_d[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	indirect_d[0].dstBinding = 0;
	indirect_d[0].dstArrayElement = 0;
	indirect_d[0].descriptorCount = 1;
	indirect_d[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	indirect_d[0].dstSet = vulkan_globals.indirect_compute_desc_set;
	indirect_d[0].pBufferInfo = &indirect_draw_buffer_info;

	indirect_d[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	indirect_d[1].dstBinding = 1;
	indirect_d[1].dstArrayElement = 0;
	indirect_d[1].descriptorCount = 1;
	indirect_d[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	indirect_d[1].dstSet = vulkan_globals.indirect_compute_desc_set;
	indirect_d[1].pBufferInfo = &surfaces_buffer_info;

	indirect_d[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	indirect_d[2].dstBinding = 2;
	indirect_d[2].dstArrayElement = 0;
	indirect_d[2].descriptorCount = 1;
	indirect_d[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	indirect_d[2].dstSet = vulkan_globals.indirect_compute_desc_set;
	indirect_d[2].pBufferInfo = &visibility_buffer_info;

	indirect_d[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	indirect_d[3].dstBinding = 3;
	indirect_d[3].dstArrayElement = 0;
	indirect_d[3].descriptorCount = 1;
	indirect_d[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	indirect_d[3].dstSet = vulkan_globals.indirect_compute_desc_set;
	indirect_d[3].pBufferInfo = &index_buffer_info;

	indirect_d[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	indirect_d[4].dstBinding = 4;
	indirect_d[4].dstArrayElement = 0;
	indirect_d[4].descriptorCount = 1;
	indirect_d[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	indirect_d[4].dstSet = vulkan_globals.indirect_compute_desc_set;
	indirect_d[4].pBufferInfo = &indirect_surface_submodels_buffer_info;

	indirect_d[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	indirect_d[5].dstBinding = 5;
	indirect_d[5].dstArrayElement = 0;
	indirect_d[5].descriptorCount = 1;
	indirect_d[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	indirect_d[5].dstSet = vulkan_globals.indirect_compute_desc_set;
	indirect_d[5].pBufferInfo = &bmodel_instances_buffer_info;

	vkUpdateDescriptorSets (vulkan_globals.device, countof (indirect_d), indirect_d, 0, NULL);

	for (int i = 0; i < cl.worldmodel->numleafs; i++)
		R_CalcDeps (NULL, &cl.worldmodel->leafs[i + 1]); // worldmodel->leafs is 1-based

	for (int j = 2; j < MAX_MODELS; j++)
	{
		qmodel_t *m = cl.model_precache[j];
		if (!m)
			break;
		if (m->name[0] == '*')
			R_CalcDeps (m, NULL);
	}
}

/*
==================
GL_RebuildIndirectDraws

Regroup indirect draws after an RT-emissive runtime toggle or receiver
classification change without rebuilding lightmaps or changing the surface-data
buffer referenced by their descriptors.
==================
*/
void GL_RebuildIndirectDraws (qboolean emissive_grouping, qboolean receiver_classification_changed)
{
	if (!cl.worldmodel || !indirect_ready || (!receiver_classification_changed && indirect_emissive_grouping == emissive_grouping))
		return;

	GL_WaitForDeviceIdle ();
	const double rebuild_start = Sys_DoubleTime ();
	const int previous_used_indirect_draws = used_indirect_draws;
	TEMP_ALLOC (indirectdraw_t, previous_indirect_draws, previous_used_indirect_draws);
	memcpy (previous_indirect_draws, indirect_draws, previous_used_indirect_draws * sizeof (*previous_indirect_draws));
	used_indirect_draws = 0;
	indirect_ready = true;

	TEMP_ALLOC (uint32_t, packed_tex_edgecounts, num_surfaces);
	TEMP_ALLOC (VkBufferCopy, copy_regions, num_surfaces);
	TEMP_ALLOC (msurface_t *, regrouped_surfaces, num_surfaces);
	TEMP_ALLOC (int, previous_indirect_indices, num_surfaces);
	uint32_t surface_index = 0;
	int		 current_submodel = 0;
	for (int j = 1; j < MAX_MODELS; ++j)
	{
		qmodel_t *const model = cl.model_precache[j];
		if (!model)
			break;
		if (model->name[0] == '*')
			continue;

		for (int i = 0; i < model->numsurfaces; ++i)
		{
			int submodel = 0;
			if (j == 1)
			{
				while ((current_submodel + 1) < model->numsubmodels && i >= model->submodels[current_submodel + 1].firstface)
					++current_submodel;
				if (current_submodel < num_worldmodel_submodels)
					submodel = current_submodel;
			}

			msurface_t *const surface = &model->surfaces[i];
			regrouped_surfaces[surface_index] = surface;
			previous_indirect_indices[surface_index] = surface->indirect_idx;
			UpdateIndirectStructs (surface, INDIRECT_ZBIAS && surface_index >= indirect_bmodel_start, j == 1 && submodel == 0, emissive_grouping);
			if (!indirect_ready)
				break;

			packed_tex_edgecounts[surface_index] = surface->indirect_idx | !!(surface->flags & SURF_PLANEBACK) << 15 | surface->numedges << 16;
			copy_regions[surface_index].srcOffset = surface_index * sizeof (*packed_tex_edgecounts);
			copy_regions[surface_index].dstOffset =
				surface_index * sizeof (lm_compute_surface_data_t) + offsetof (lm_compute_surface_data_t, packed_tex_edgecount);
			copy_regions[surface_index].size = sizeof (*packed_tex_edgecounts);
			++surface_index;
		}
		if (!indirect_ready)
			break;
	}

	if (indirect_ready)
	{
		assert (surface_index == (uint32_t)num_surfaces);
		VkCommandBuffer command_buffer;
		VkBuffer		staging_buffer;
		int				staging_offset;
		uint32_t *const staging_memory =
			(uint32_t *)R_StagingAllocate (num_surfaces * sizeof (*packed_tex_edgecounts), 4, &command_buffer, &staging_buffer, &staging_offset);
		for (uint32_t i = 0; i < surface_index; ++i)
			copy_regions[i].srcOffset += staging_offset;
		vkCmdCopyBuffer (command_buffer, staging_buffer, surface_data_buffer, surface_index, copy_regions);
		R_StagingBeginCopy ();
		memcpy (staging_memory, packed_tex_edgecounts, num_surfaces * sizeof (*packed_tex_edgecounts));
		R_StagingEndCopy ();

		indirect_emissive_grouping = emissive_grouping;
		used_deps_data = 0;
		Mem_Free (brush_deps_data);
		GL_SetupIndirectDraws ();
		Con_DPrintf (
			"RT emissives: rebuilt %d indirect draw%s in %.3f ms\n", used_indirect_draws, used_indirect_draws == 1 ? "" : "s",
			(Sys_DoubleTime () - rebuild_start) * 1000.0);
	}
	else
	{
		memcpy (indirect_draws, previous_indirect_draws, previous_used_indirect_draws * sizeof (*indirect_draws));
		used_indirect_draws = previous_used_indirect_draws;
		indirect_ready = true;
		for (uint32_t i = 0; i < surface_index; ++i)
			regrouped_surfaces[i]->indirect_idx = previous_indirect_indices[i];
		Con_Warning ("map exceeds indirect dispatch limits after RT-emissive regroup\n");
	}

	TEMP_FREE (previous_indirect_indices);
	TEMP_FREE (regrouped_surfaces);
	TEMP_FREE (copy_regions);
	TEMP_FREE (packed_tex_edgecounts);
	TEMP_FREE (previous_indirect_draws);
}

/*
==================
GL_UpdateLightmapDescriptorSets
==================
*/
void GL_UpdateLightmapDescriptorSets (void)
{
	GL_WaitForDeviceIdle ();

	vulkan_desc_set_layout_t *set_layout = &vulkan_globals.lightmap_compute_set_layout;

	if (lightmap_count && !lightmaps[0].texture->target_image_view)
		return;

	for (int i = 0; i < lightmap_count; i++)
	{
		struct lightmap_s *lm = &lightmaps[i];
		if (lm->descriptor_set)
			R_FreeDescriptorSet (lm->descriptor_set, set_layout);
		lm->descriptor_set = R_AllocateDescriptorSet (set_layout);
		GL_SetObjectName ((uint64_t)lm->descriptor_set, VK_OBJECT_TYPE_DESCRIPTOR_SET, va ("lightmap%07i compute desc set", i));

		ZEROED_STRUCT (VkDescriptorImageInfo, output_image_info);
		output_image_info.imageView = lm->texture->target_image_view;
		output_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		ZEROED_STRUCT (VkDescriptorImageInfo, surface_indices_image_info);
		surface_indices_image_info.imageView = lm->surface_indices_texture->image_view;
		surface_indices_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		ZEROED_STRUCT_ARRAY (VkDescriptorImageInfo, lightmap_images_infos, MAXLIGHTMAPS * 3 / 4);
		for (int j = 0; j < MAXLIGHTMAPS * 3 / 4; ++j)
		{
			lightmap_images_infos[j].imageView = lm->lightstyle_textures[j]->image_view;
			lightmap_images_infos[j].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}

		ZEROED_STRUCT (VkDescriptorBufferInfo, surfaces_data_buffer_info);
		surfaces_data_buffer_info.buffer = surface_data_buffer;
		surfaces_data_buffer_info.offset = 0;
		surfaces_data_buffer_info.range = num_surfaces * sizeof (lm_compute_surface_data_t);

		ZEROED_STRUCT (VkDescriptorBufferInfo, workgroup_bounds_buffer_info);
		workgroup_bounds_buffer_info.buffer = lm->workgroup_bounds_buffer;
		workgroup_bounds_buffer_info.offset = 0;
		workgroup_bounds_buffer_info.range = (LMBLOCK_WIDTH / 8) * (LMBLOCK_HEIGHT / 8) * sizeof (lm_compute_workgroup_bounds_t);

		ZEROED_STRUCT (VkDescriptorBufferInfo, lightstyle_scales_buffer_info);
		lightstyle_scales_buffer_info.buffer = lightstyles_scales_buffer;
		lightstyle_scales_buffer_info.offset = 0;
		lightstyle_scales_buffer_info.range = MAX_LIGHTSTYLES * sizeof (float);

		ZEROED_STRUCT (VkDescriptorBufferInfo, lights_buffer_info);
		lights_buffer_info.buffer = lights_buffer;
		lights_buffer_info.offset = 0;
		lights_buffer_info.range = MAX_DLIGHTS * 2 * sizeof (lm_compute_light_t);

		ZEROED_STRUCT (VkDescriptorBufferInfo, world_vertex_buffer_info);
		world_vertex_buffer_info.buffer = bmodel_vertex_buffer;
		world_vertex_buffer_info.offset = 0;
		world_vertex_buffer_info.range = VK_WHOLE_SIZE;

		ZEROED_STRUCT (VkDescriptorBufferInfo, surface_submodels_buffer_info);
		surface_submodels_buffer_info.buffer = surface_submodels_buffer;
		surface_submodels_buffer_info.offset = 0;
		surface_submodels_buffer_info.range = num_surfaces * sizeof (uint32_t);

		ZEROED_STRUCT (VkDescriptorBufferInfo, submodel_transforms_buffer_info);
		submodel_transforms_buffer_info.buffer = submodel_transforms_buffer;
		submodel_transforms_buffer_info.offset = 0;
		submodel_transforms_buffer_info.range = VK_WHOLE_SIZE;

		int num_writes = 0;
		ZEROED_STRUCT_ARRAY (VkWriteDescriptorSet, writes, 10);
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstBinding = num_writes++;
		writes[0].dstArrayElement = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[0].dstSet = lm->descriptor_set;
		writes[0].pImageInfo = &output_image_info;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstBinding = num_writes++;
		writes[1].dstArrayElement = 0;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[1].dstSet = lm->descriptor_set;
		writes[1].pImageInfo = &surface_indices_image_info;

		writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[2].dstBinding = num_writes++;
		writes[2].dstArrayElement = 0;
		writes[2].descriptorCount = MAXLIGHTMAPS * 3 / 4;
		writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[2].dstSet = lm->descriptor_set;
		writes[2].pImageInfo = lightmap_images_infos;

		writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[3].dstBinding = num_writes++;
		writes[3].dstArrayElement = 0;
		writes[3].descriptorCount = 1;
		writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[3].dstSet = lm->descriptor_set;
		writes[3].pBufferInfo = &surfaces_data_buffer_info;

		writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[4].dstBinding = num_writes++;
		writes[4].dstArrayElement = 0;
		writes[4].descriptorCount = 1;
		writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[4].dstSet = lm->descriptor_set;
		writes[4].pBufferInfo = &workgroup_bounds_buffer_info;

		writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[5].dstBinding = num_writes++;
		writes[5].dstArrayElement = 0;
		writes[5].descriptorCount = 1;
		writes[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		writes[5].dstSet = lm->descriptor_set;
		writes[5].pBufferInfo = &lightstyle_scales_buffer_info;

		writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[6].dstBinding = num_writes++;
		writes[6].dstArrayElement = 0;
		writes[6].descriptorCount = 1;
		writes[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		writes[6].dstSet = lm->descriptor_set;
		writes[6].pBufferInfo = &lights_buffer_info;

		writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[7].dstBinding = num_writes++;
		writes[7].dstArrayElement = 0;
		writes[7].descriptorCount = 1;
		writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[7].dstSet = lm->descriptor_set;
		writes[7].pBufferInfo = &world_vertex_buffer_info;

		writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[8].dstBinding = num_writes++;
		writes[8].dstArrayElement = 0;
		writes[8].descriptorCount = 1;
		writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[8].dstSet = lm->descriptor_set;
		writes[8].pBufferInfo = &surface_submodels_buffer_info;

		writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[9].dstBinding = num_writes++;
		writes[9].dstArrayElement = 0;
		writes[9].descriptorCount = 1;
		writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[9].dstSet = lm->descriptor_set;
		writes[9].pBufferInfo = &submodel_transforms_buffer_info;

		vkUpdateDescriptorSets (vulkan_globals.device, num_writes, writes, 0, NULL);
	}
}

/*
==================
GL_SetupLightmapCompute
==================
*/
void GL_SetupLightmapCompute (void)
{
	GL_AllocateWorkgroupBoundsBuffers ();

	//
	// upload all lightmaps that were filled
	//
	for (int i = 0; i < lightmap_count; i++)
	{
		struct lightmap_s *lm = &lightmaps[i];
		for (int j = 0; j < TASKS_MAX_WORKERS; ++j)
			lm->modified[j] = 0;
		lm->rectchange.l = LMBLOCK_WIDTH;
		lm->rectchange.t = LMBLOCK_HEIGHT;
		lm->rectchange.w = 0;
		lm->rectchange.h = 0;

		char name[32];
		q_snprintf (name, sizeof (name), "lightmap_%07i", i);

		lm->texture = TexMgr_LoadImage (
			cl.worldmodel, name, LMBLOCK_WIDTH, LMBLOCK_HEIGHT, SRC_LIGHTMAP, lm->data, "", (src_offset_t)lm->data, TEXPREF_LINEAR | TEXPREF_NOPICMIP);
		for (int j = 0; j < MAXLIGHTMAPS * 3 / 4; ++j)
		{
			q_snprintf (name, sizeof (name), "lightstyle%d_%07i", j, i);
			int size_w = lightmaps[i].lightstyle_rectused[j + 1].w;
			int size_h = lightmaps[i].lightstyle_rectused[j + 1].h;
			if (LMBLOCK_WIDTH - size_w < 16) // don't bother
				size_w = LMBLOCK_WIDTH;
			if (size_h == 0)
				lm->lightstyle_textures[j] = nulltexture;
			else
			{
				if (size_w < LMBLOCK_WIDTH) // this is not common and is easier than handling variable strides in TexMgr_LoadImage
					for (int row = 1; row < size_h; row++)
						memmove (lm->lightstyle_data[j] + size_w * row * 4, lm->lightstyle_data[j] + LMBLOCK_WIDTH * row * 4, size_w * 4);
				lm->lightstyle_textures[j] = TexMgr_LoadImage (
					cl.worldmodel, name, size_w, size_h, SRC_RGBA, lm->lightstyle_data[j], "", (src_offset_t)lm->data, TEXPREF_NEAREST | TEXPREF_NOPICMIP);
			}
			SAFE_FREE (lm->lightstyle_data[j]);
		}

		unsigned short *size_w = &lightmaps[i].lightstyle_rectused[0].w;
		unsigned short *size_h = &lightmaps[i].lightstyle_rectused[0].h;
		*size_w = (*size_w + 7) / 8 * 8;
		*size_h = (*size_h + 7) / 8 * 8;
		if (*size_w < LMBLOCK_WIDTH) // this is not common and is easier than handling variable strides in TexMgr_LoadImage
			for (int row = 1; row < *size_h; row++)
				memmove (lm->surface_indices + *size_w * row, lm->surface_indices + LMBLOCK_WIDTH * row, *size_w * 4);
		q_snprintf (name, sizeof (name), "surfindices_%07i", i);
		lm->surface_indices_texture = TexMgr_LoadImage (
			cl.worldmodel, name, *size_w, *size_h, SRC_SURF_INDICES, (byte *)lm->surface_indices, "", (src_offset_t)lm->surface_indices,
			TEXPREF_NEAREST | TEXPREF_NOPICMIP);
		SAFE_FREE (lm->surface_indices);

		for (int y = 0; y < LMBLOCK_HEIGHT / LM_CULL_BLOCK_H; y++)
			for (int x = 0; x < LMBLOCK_WIDTH / LM_CULL_BLOCK_W; x++)
				for (int l = 0; l < MAX_LIGHTSTYLES; l++)
					if (lightmaps[i].used_lightstyles[y][x][l])
						lightmaps[i].used_lightstyles[y][x][lightmaps[i].num_used_lightstyles[y][x]++] = l;
	}

	for (int i = 0; i < lightmap_count; i++)
	{
		struct lightmap_s *lm = &lightmaps[i];

		VkCommandBuffer command_buffer;
		VkBuffer		staging_buffer;
		int				staging_offset;
		byte		   *bounds_staging = R_StagingAllocate (WORKGROUP_BOUNDS_BUFFER_SIZE, 1, &command_buffer, &staging_buffer, &staging_offset);

		VkBufferCopy region;
		region.srcOffset = staging_offset;
		region.dstOffset = 0;
		region.size = WORKGROUP_BOUNDS_BUFFER_SIZE;
		vkCmdCopyBuffer (command_buffer, staging_buffer, lm->workgroup_bounds_buffer, 1, &region);

		R_StagingBeginCopy ();
		memcpy (bounds_staging, lm->workgroup_bounds, WORKGROUP_BOUNDS_BUFFER_SIZE);
		lm_compute_workgroup_bounds_t *staged_bounds = (lm_compute_workgroup_bounds_t *)bounds_staging;
		for (int j = 0; j < (LMBLOCK_WIDTH / 8) * (LMBLOCK_HEIGHT / 8); ++j)
			if (staged_bounds[j].submodel == LM_WORKGROUP_SUBMODEL_EMPTY)
				staged_bounds[j].submodel = 0; // empty workgroups cull like static world space
		R_StagingEndCopy ();
		SAFE_FREE (lm->workgroup_bounds);
	}

	// johnfitz -- warn about exceeding old limits
	// GLQuake limit was 64 textures of 128x128. Estimate how many 128x128 textures we would need
	// given that we are using lightmap_count of LMBLOCK_WIDTH x LMBLOCK_HEIGHT
	int i = lightmap_count * ((LMBLOCK_WIDTH / 128) * (LMBLOCK_HEIGHT / 128));
	if (i > 64)
		Con_DWarning ("%i lightmaps exceeds standard limit of 64.\n", i);
	// johnfitz
}

/*
==================
R_EmissiveLightmapTileOffsets
==================
*/
static int R_EmissiveLightmapTileOffsets (int *offsets)
{
	int total = 0;
	for (int i = 0; i < lightmap_count; ++i)
	{
		offsets[i] = -1;
		if (!lightmaps[i].emissive_detail_texture)
			continue;
		offsets[i] = total;
		const gltexture_t *const texture = lightmaps[i].surface_indices_texture;
		total += ((texture->width + 7) / 8) * ((texture->height + 7) / 8);
	}
	return total;
}

typedef struct emissive_surface_tile_rect_s
{
	int lightmap;
	int tiles_wide;
	int first_x;
	int first_y;
	int last_x;
	int last_y;
} emissive_surface_tile_rect_t;

static qboolean R_EmissiveSurfaceTileRect (const msurface_t *surface, emissive_surface_tile_rect_t *rect)
{
	rect->lightmap = surface->lightmaptexturenum;
	if (!surface->emissive_influence || rect->lightmap < 0 || rect->lightmap >= lightmap_count ||
		!lightmaps[rect->lightmap].emissive_detail_texture)
		return false;

	const gltexture_t *const texture = lightmaps[rect->lightmap].surface_indices_texture;
	rect->tiles_wide = (texture->width + 7) / 8;
	const int tiles_high = (texture->height + 7) / 8;
	const int surface_width = (surface->extents[0] >> 4) + 1;
	const int surface_height = (surface->extents[1] >> 4) + 1;
	rect->first_x = CLAMP (0, surface->light_s / 8, rect->tiles_wide - 1);
	rect->first_y = CLAMP (0, surface->light_t / 8, tiles_high - 1);
	rect->last_x = CLAMP (0, (surface->light_s + surface_width - 1) / 8, rect->tiles_wide - 1);
	rect->last_y = CLAMP (0, (surface->light_t + surface_height - 1) / 8, tiles_high - 1);
	return true;
}

static qboolean R_EmissiveSurfaceLightmapPoint (const msurface_t *surface, float s, float t, vec3_t point)
{
	const vec3_t *const normal = &surface->plane->normal;
	vec3_t			 pos_edge1, pos_edge2;
	if (fabsf ((*normal)[0]) > fabsf ((*normal)[2]))
	{
		pos_edge1[0] = -(*normal)[1];
		pos_edge1[1] = (*normal)[0];
		pos_edge1[2] = 0.0f;
	}
	else
	{
		pos_edge1[0] = 0.0f;
		pos_edge1[1] = -(*normal)[2];
		pos_edge1[2] = (*normal)[1];
	}
	CrossProduct (*normal, pos_edge1, pos_edge2);

	const float *const vec_s = surface->texinfo->vecs[0];
	const float *const vec_t = surface->texinfo->vecs[1];
	const float st_edge1_s = DotProduct (pos_edge1, vec_s);
	const float st_edge1_t = DotProduct (pos_edge1, vec_t);
	const float st_edge2_s = DotProduct (pos_edge2, vec_s);
	const float st_edge2_t = DotProduct (pos_edge2, vec_t);
	const float determinant = st_edge1_s * st_edge2_t - st_edge2_s * st_edge1_t;
	if (fabsf (determinant) < 0.000001f)
		return false;

	const float scale = 16.0f / determinant;
	vec3_t		tangent, bitangent;
	for (int i = 0; i < 3; ++i)
	{
		tangent[i] = scale * (st_edge2_t * pos_edge1[i] - st_edge1_t * pos_edge2[i]);
		bitangent[i] = scale * (-st_edge2_s * pos_edge1[i] + st_edge1_s * pos_edge2[i]);
	}

	const int surfedge = cl.worldmodel->surfedges[surface->firstedge];
	const int vertex_index = surfedge >= 0 ? cl.worldmodel->edges[surfedge].v[0] : cl.worldmodel->edges[-surfedge].v[1];
	const vec3_t *const first_vertex = &cl.worldmodel->vertexes[vertex_index].position;
	const float first_offset_s =
		-(DotProduct (*first_vertex, vec_s) + vec_s[3] - surface->texturemins[0]) / 16.0f;
	const float first_offset_t =
		-(DotProduct (*first_vertex, vec_t) + vec_t[3] - surface->texturemins[1]) / 16.0f;
	VectorCopy (*first_vertex, point);
	VectorMA (point, s + first_offset_s, tangent, point);
	VectorMA (point, t + first_offset_t, bitangent, point);
	return true;
}

static qboolean R_EmissiveSurfaceTileBounds (const msurface_t *surface, int tile_x, int tile_y, vec3_t mins, vec3_t maxs)
{
	const int surface_width = (surface->extents[0] >> 4) + 1;
	const int surface_height = (surface->extents[1] >> 4) + 1;
	const int first_s = q_max (tile_x * 8, surface->light_s);
	const int first_t = q_max (tile_y * 8, surface->light_t);
	const int last_s = q_min ((tile_x + 1) * 8, surface->light_s + surface_width);
	const int last_t = q_min ((tile_y + 1) * 8, surface->light_t + surface_height);
	if (first_s >= last_s || first_t >= last_t)
		return false;

	vec3_t corners[4];
	if (!R_EmissiveSurfaceLightmapPoint (surface, first_s - surface->light_s - 1.0f, first_t - surface->light_t - 1.0f, corners[0]) ||
		!R_EmissiveSurfaceLightmapPoint (surface, last_s - surface->light_s - 1.0f, first_t - surface->light_t - 1.0f, corners[1]) ||
		!R_EmissiveSurfaceLightmapPoint (surface, first_s - surface->light_s - 1.0f, last_t - surface->light_t - 1.0f, corners[2]) ||
		!R_EmissiveSurfaceLightmapPoint (surface, last_s - surface->light_s - 1.0f, last_t - surface->light_t - 1.0f, corners[3]))
	{
		const float *vertex = surface->polys->verts[0];
		VectorCopy (vertex, mins);
		VectorCopy (vertex, maxs);
		for (int i = 1; i < surface->polys->numverts; ++i)
		{
			vertex += VERTEXSIZE;
			for (int axis = 0; axis < 3; ++axis)
			{
				mins[axis] = q_min (mins[axis], vertex[axis]);
				maxs[axis] = q_max (maxs[axis], vertex[axis]);
			}
		}
		return true;
	}

	VectorCopy (corners[0], mins);
	VectorCopy (corners[0], maxs);
	for (int corner = 1; corner < countof (corners); ++corner)
		for (int axis = 0; axis < 3; ++axis)
		{
			mins[axis] = q_min (mins[axis], corners[corner][axis]);
			maxs[axis] = q_max (maxs[axis], corners[corner][axis]);
		}
	return true;
}

static qboolean R_EmissiveLightInfluencesTile (
	const msurface_t *surface, int tile_x, int tile_y, const emissive_light_t *light)
{
	vec3_t mins, maxs;
	if (!R_EmissiveSurfaceTileBounds (surface, tile_x, tile_y, mins, maxs))
		return false;

	float distance_squared = 0.0f;
	for (int axis = 0; axis < 3; ++axis)
	{
		float distance = 0.0f;
		if (light->origin[axis] < mins[axis])
			distance = mins[axis] - light->origin[axis];
		else if (light->origin[axis] > maxs[axis])
			distance = light->origin[axis] - maxs[axis];
		distance_squared += distance * distance;
	}
	return distance_squared < light->radius * light->radius;
}

/*
==================
R_BuildEmissiveLogicalTiles
==================
*/
static void R_BuildEmissiveLogicalTiles (void)
{
	if (emissive_logical_tiles_built || !cl.worldmodel || !R_EmissiveDetailAvailable ())
		return;

	int *const lightmap_offsets = Mem_Alloc (lightmap_count * sizeof (*lightmap_offsets));
	num_emissive_logical_tiles_total = R_EmissiveLightmapTileOffsets (lightmap_offsets);

	byte *const affected = Mem_Alloc (num_emissive_logical_tiles_total);
	memset (affected, 0, num_emissive_logical_tiles_total);
	msurface_t *const first_surface = &cl.worldmodel->surfaces[cl.worldmodel->firstmodelsurface];
	for (int i = 0; i < cl.worldmodel->nummodelsurfaces; ++i)
	{
		const msurface_t *const surface = &first_surface[i];
		emissive_surface_tile_rect_t rect;
		if (!R_EmissiveSurfaceTileRect (surface, &rect))
			continue;
		for (int y = rect.first_y; y <= rect.last_y; ++y)
			for (int x = rect.first_x; x <= rect.last_x; ++x)
				affected[lightmap_offsets[rect.lightmap] + y * rect.tiles_wide + x] = true;
	}

	for (int i = 0; i < num_emissive_logical_tiles_total; ++i)
		if (affected[i])
			++num_emissive_logical_tiles;
	if (num_emissive_logical_tiles)
		emissive_logical_tiles = Mem_Alloc (num_emissive_logical_tiles * sizeof (*emissive_logical_tiles));
	int tile_index = 0;
	for (int i = 0; i < lightmap_count; ++i)
	{
		if (lightmap_offsets[i] < 0)
			continue;
		const gltexture_t *const texture = lightmaps[i].surface_indices_texture;
		const int			 tiles_wide = (texture->width + 7) / 8;
		const int			 tiles_high = (texture->height + 7) / 8;
		for (int y = 0; y < tiles_high; ++y)
			for (int x = 0; x < tiles_wide; ++x)
				if (affected[lightmap_offsets[i] + y * tiles_wide + x])
				{
					emissive_logical_tiles[tile_index].first_source = 0;
					emissive_logical_tiles[tile_index].num_sources = 0;
					emissive_logical_tiles[tile_index].lightmap = i;
					emissive_logical_tiles[tile_index].x = x;
					emissive_logical_tiles[tile_index].y = y;
					++tile_index;
				}
	}
	assert (tile_index == num_emissive_logical_tiles);
	emissive_logical_tiles_built = true;
	Con_DPrintf (
		"RT emissives: %d/%d candidate 8x8 logical tile%s (%.1f%%, %" PRIu64 " CPU bytes)\n", num_emissive_logical_tiles,
		num_emissive_logical_tiles_total, num_emissive_logical_tiles == 1 ? "" : "s",
		num_emissive_logical_tiles_total ? 100.0 * num_emissive_logical_tiles / num_emissive_logical_tiles_total : 0.0,
		(uint64_t)num_emissive_logical_tiles * sizeof (*emissive_logical_tiles));
	Mem_Free (affected);
	Mem_Free (lightmap_offsets);
}

typedef struct emissive_tile_source_pair_s
{
	uint32_t tile;
	uint32_t source;
} emissive_tile_source_pair_t;

static int R_CompareEmissiveTileSourcePairs (const void *lhs_ptr, const void *rhs_ptr)
{
	const emissive_tile_source_pair_t *const lhs = lhs_ptr;
	const emissive_tile_source_pair_t *const rhs = rhs_ptr;
	if (lhs->tile != rhs->tile)
		return lhs->tile < rhs->tile ? -1 : 1;
	if (lhs->source != rhs->source)
		return lhs->source < rhs->source ? -1 : 1;
	return 0;
}

static int R_TransientEmissiveLightmapTileOffsets (int *offsets)
{
	int total = 0;
	for (int i = 0; i < lightmap_count; ++i)
	{
		offsets[i] = total;
		const gltexture_t *const texture = lightmaps[i].surface_indices_texture;
		total += ((texture->width + 7) / 8) * ((texture->height + 7) / 8);
	}
	return total;
}

static qboolean R_TransientEmissiveSurfaceTileRect (const msurface_t *surface, emissive_surface_tile_rect_t *rect)
{
	rect->lightmap = surface->lightmaptexturenum;
	if (surface->numedges < 3 || (surface->flags & SURF_DRAWTILED) || rect->lightmap < 0 || rect->lightmap >= lightmap_count)
		return false;
	const gltexture_t *const texture = lightmaps[rect->lightmap].surface_indices_texture;
	rect->tiles_wide = (texture->width + 7) / 8;
	const int tiles_high = (texture->height + 7) / 8;
	const int surface_width = (surface->extents[0] >> 4) + 1;
	const int surface_height = (surface->extents[1] >> 4) + 1;
	rect->first_x = CLAMP (0, surface->light_s / 8, rect->tiles_wide - 1);
	rect->first_y = CLAMP (0, surface->light_t / 8, tiles_high - 1);
	rect->last_x = CLAMP (0, (surface->light_s + surface_width - 1) / 8, rect->tiles_wide - 1);
	rect->last_y = CLAMP (0, (surface->light_t + surface_height - 1) / 8, tiles_high - 1);
	return true;
}

static qboolean R_TransientEmissiveLightsEqual (const emissive_light_t *a, const emissive_light_t *b)
{
	return !memcmp (a, b, sizeof (*a));
}

static qboolean R_TransientEmissiveSourcesEqual (const transient_emissive_source_t *sources, int count)
{
	if (count != num_transient_emissive_lights)
		return false;
	for (int i = 0; i < count; ++i)
		if (R_CompareTransientEmissiveSourceIds (&sources[i].id, &transient_emissive_light_ids[i]) ||
			!R_TransientEmissiveLightsEqual (&sources[i].light, &transient_emissive_lights[i]))
			return false;
	return true;
}

static int R_CompareTransientEmissiveTiles (const void *a_, const void *b_)
{
	const int a = *(const int *)a_;
	const int b = *(const int *)b_;
	return a < b ? -1 : a > b;
}

static void R_BuildTransientEmissiveTileSurfaces (const int *lightmap_offsets, int total_tiles)
{
	if (transient_emissive_tile_surface_worldmodel == cl.worldmodel)
		return;
	SAFE_FREE (transient_emissive_tile_surface_offsets);
	SAFE_FREE (transient_emissive_tile_surfaces);
	SAFE_FREE (transient_emissive_tile_generations);
	SAFE_FREE (transient_emissive_tile_indices);
	SAFE_FREE (transient_emissive_surface_influence_counts);
	SAFE_FREE (transient_emissive_surface_deltas);
	SAFE_FREE (transient_emissive_surface_delta_generations);
	SAFE_FREE (transient_emissive_touched_surfaces);
	transient_emissive_tile_surface_offsets = Mem_Alloc ((total_tiles + 1) * sizeof (*transient_emissive_tile_surface_offsets));
	memset (transient_emissive_tile_surface_offsets, 0, (total_tiles + 1) * sizeof (*transient_emissive_tile_surface_offsets));
	transient_emissive_tile_generations = Mem_Alloc (total_tiles * sizeof (*transient_emissive_tile_generations));
	transient_emissive_tile_indices = Mem_Alloc (total_tiles * sizeof (*transient_emissive_tile_indices));
	memset (transient_emissive_tile_generations, 0, total_tiles * sizeof (*transient_emissive_tile_generations));
	transient_emissive_tile_generation = 0;
	transient_emissive_surface_influence_counts = Mem_Alloc (cl.worldmodel->nummodelsurfaces * sizeof (*transient_emissive_surface_influence_counts));
	transient_emissive_surface_deltas = Mem_Alloc (cl.worldmodel->nummodelsurfaces * sizeof (*transient_emissive_surface_deltas));
	transient_emissive_surface_delta_generations = Mem_Alloc (cl.worldmodel->nummodelsurfaces * sizeof (*transient_emissive_surface_delta_generations));
	transient_emissive_touched_surfaces = Mem_Alloc (cl.worldmodel->nummodelsurfaces * sizeof (*transient_emissive_touched_surfaces));
	memset (transient_emissive_surface_influence_counts, 0,
		cl.worldmodel->nummodelsurfaces * sizeof (*transient_emissive_surface_influence_counts));
	memset (transient_emissive_surface_delta_generations, 0,
		cl.worldmodel->nummodelsurfaces * sizeof (*transient_emissive_surface_delta_generations));
	num_transient_emissive_touched_surfaces = 0;
	transient_emissive_surface_delta_generation = 0;
	msurface_t *const first_surface = &cl.worldmodel->surfaces[cl.worldmodel->firstmodelsurface];
	for (int surface_index = 0; surface_index < cl.worldmodel->nummodelsurfaces; ++surface_index)
	{
		emissive_surface_tile_rect_t rect;
		if (!R_TransientEmissiveSurfaceTileRect (&first_surface[surface_index], &rect))
			continue;
		for (int y = rect.first_y; y <= rect.last_y; ++y)
			for (int x = rect.first_x; x <= rect.last_x; ++x)
				++transient_emissive_tile_surface_offsets[lightmap_offsets[rect.lightmap] + y * rect.tiles_wide + x + 1];
	}
	for (int tile = 1; tile <= total_tiles; ++tile)
		transient_emissive_tile_surface_offsets[tile] += transient_emissive_tile_surface_offsets[tile - 1];
	num_transient_emissive_tile_surfaces = transient_emissive_tile_surface_offsets[total_tiles];
	num_transient_emissive_total_tiles = total_tiles;
	if (num_transient_emissive_tile_surfaces)
		transient_emissive_tile_surfaces = Mem_Alloc (num_transient_emissive_tile_surfaces * sizeof (*transient_emissive_tile_surfaces));
	uint32_t *const cursors = Mem_Alloc (total_tiles * sizeof (*cursors));
	memcpy (cursors, transient_emissive_tile_surface_offsets, total_tiles * sizeof (*cursors));
	for (int surface_index = 0; surface_index < cl.worldmodel->nummodelsurfaces; ++surface_index)
	{
		emissive_surface_tile_rect_t rect;
		if (!R_TransientEmissiveSurfaceTileRect (&first_surface[surface_index], &rect))
			continue;
		for (int y = rect.first_y; y <= rect.last_y; ++y)
			for (int x = rect.first_x; x <= rect.last_x; ++x)
			{
				const int tile = lightmap_offsets[rect.lightmap] + y * rect.tiles_wide + x;
				transient_emissive_tile_surfaces[cursors[tile]++] = surface_index;
			}
	}
	Mem_Free (cursors);
	transient_emissive_tile_surface_worldmodel = cl.worldmodel;
}

typedef struct transient_emissive_invalidation_s
{
	const emissive_light_t *light;
	const int				 *lightmap_offsets;
	int					**dirty_tiles;
	int					 *num_dirty_tiles;
	int					 *dirty_tile_capacity;
	int					  surface_influence_delta;
} transient_emissive_invalidation_t;

static void R_AddTransientEmissiveSurfaceDelta (int surface, int delta)
{
	if (!delta)
		return;
	if (transient_emissive_surface_delta_generations[surface] != transient_emissive_surface_delta_generation)
	{
		transient_emissive_surface_delta_generations[surface] = transient_emissive_surface_delta_generation;
		transient_emissive_surface_deltas[surface] = 0;
		transient_emissive_touched_surfaces[num_transient_emissive_touched_surfaces++] = surface;
	}
	transient_emissive_surface_deltas[surface] += delta;
}

static void R_MarkTransientEmissiveTileDirty (transient_emissive_invalidation_t *invalidation, int tile)
{
	if (!invalidation->dirty_tiles || transient_emissive_tile_generations[tile] == transient_emissive_tile_generation)
		return;
	transient_emissive_tile_generations[tile] = transient_emissive_tile_generation;
	if (*invalidation->num_dirty_tiles == *invalidation->dirty_tile_capacity)
	{
		*invalidation->dirty_tile_capacity = *invalidation->dirty_tile_capacity ? *invalidation->dirty_tile_capacity * 2 : 256;
		*invalidation->dirty_tiles = Mem_Realloc (
			*invalidation->dirty_tiles, *invalidation->dirty_tile_capacity * sizeof (**invalidation->dirty_tiles));
	}
	(*invalidation->dirty_tiles)[(*invalidation->num_dirty_tiles)++] = tile;
}

static void R_InvalidateTransientEmissiveNode (mnode_t *node, transient_emissive_invalidation_t *invalidation)
{
	if (node->contents < 0)
		return;
	const float distance = node->plane->type < 3 ? invalidation->light->origin[node->plane->type] - node->plane->dist
											 : DotProduct (invalidation->light->origin, node->plane->normal) - node->plane->dist;
	const float influence_radius = invalidation->light->radius;
	if (distance > influence_radius)
	{
		R_InvalidateTransientEmissiveNode (node->children[0], invalidation);
		return;
	}
	if (distance < -influence_radius)
	{
		R_InvalidateTransientEmissiveNode (node->children[1], invalidation);
		return;
	}
	msurface_t *const first_surface = &cl.worldmodel->surfaces[cl.worldmodel->firstmodelsurface];
	for (unsigned int i = 0; i < node->numsurfaces; ++i)
	{
		msurface_t *const surface = &cl.worldmodel->surfaces[node->firstsurface + i];
		if (surface < first_surface || surface >= first_surface + cl.worldmodel->nummodelsurfaces)
			continue;
		emissive_surface_tile_rect_t rect;
		if (!R_TransientEmissiveSurfaceTileRect (surface, &rect))
			continue;
		qboolean affected = false;
		for (int y = rect.first_y; y <= rect.last_y; ++y)
			for (int x = rect.first_x; x <= rect.last_x; ++x)
				if (R_EmissiveLightInfluencesTile (surface, x, y, invalidation->light))
				{
					R_MarkTransientEmissiveTileDirty (
						invalidation, invalidation->lightmap_offsets[rect.lightmap] + y * rect.tiles_wide + x);
					affected = true;
				}
		if (affected)
			R_AddTransientEmissiveSurfaceDelta (surface - first_surface, invalidation->surface_influence_delta);
	}
	R_InvalidateTransientEmissiveNode (node->children[0], invalidation);
	R_InvalidateTransientEmissiveNode (node->children[1], invalidation);
}

void R_InvalidateTransientEmissiveLights (void)
{
	transient_emissive_force_refresh = true;
}

/* Stable sampling seed from persistent source identity. Array positions shift
 * under compaction and origins move, so neither may feed the sample hash. */
static uint32_t R_TransientEmissiveSourceSeed (transient_emissive_source_id_t id)
{
	uint32_t seed = COM_HashBlock (&id.owner, sizeof (id.owner));
	seed ^= COM_HashBlock (&id.slot, sizeof (id.slot)) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
	seed ^= (uint32_t)id.kind + 0x9e3779b9u + (seed << 6) + (seed >> 2);
	return seed;
}

/* Cacheable sources never move, so immutable transport properties seed them. */
static uint32_t R_EmissiveLightSeed (const vec3_t origin, float radius)
{
	uint32_t seed = COM_HashBlock (origin, sizeof (vec3_t));
	seed ^= COM_HashBlock (&radius, sizeof (radius)) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
	return seed;
}

/* Shared transport invalidation for transient detail usability: emitter-list
 * changes, participating occluder changes, and any other transport invalidation
 * clear both completion-based readiness and same-frame publication, and advance
 * the generation so in-flight completions cannot reestablish readiness.
 * Radiance-only changes stay out: modulation never invalidates transport. */
static void R_InvalidateTransientEmissiveDetail (void)
{
	transient_emissive_detail_ready = false;
	transient_emissive_detail_published_generation = 0;
	if (++transient_emissive_generation == 0)
		++transient_emissive_generation;
}

/* Shared tail for transient tile-list publication: turns a flat-tile worklist into
 * the working tile array plus per-tile source lists for the current source set.
 * The caller supplies source-change dirty tiles or, for occluder movement, the
 * union of active influence; the flat list stays caller-owned, pairs are local. */
static void R_FinalizeTransientEmissiveTiles (int *flat_tiles, int num_flat_tiles, const int *lightmap_offsets)
{
	msurface_t *const first_surface = &cl.worldmodel->surfaces[cl.worldmodel->firstmodelsurface];
	SAFE_FREE (transient_emissive_tiles);
	SAFE_FREE (transient_emissive_tile_sources);
	num_transient_emissive_tiles = 0;
	num_transient_emissive_tile_sources = 0;
	if (num_flat_tiles > 1)
		qsort (flat_tiles, num_flat_tiles, sizeof (*flat_tiles), R_CompareTransientEmissiveTiles);
	num_transient_emissive_tiles = num_flat_tiles;
	if (num_transient_emissive_tiles)
		transient_emissive_tiles = Mem_Alloc (num_transient_emissive_tiles * sizeof (*transient_emissive_tiles));
	int lightmap = 0;
	for (int tile_index = 0; tile_index < num_flat_tiles; ++tile_index)
	{
		const int flat_tile = flat_tiles[tile_index];
		while (lightmap + 1 < lightmap_count && flat_tile >= lightmap_offsets[lightmap + 1])
			++lightmap;
		const gltexture_t *const texture = lightmaps[lightmap].surface_indices_texture;
		const int tiles_wide = (texture->width + 7) / 8;
		const int local_tile = flat_tile - lightmap_offsets[lightmap];
		transient_emissive_tiles[tile_index].first_source = 0;
		transient_emissive_tiles[tile_index].num_sources = 0;
		transient_emissive_tiles[tile_index].lightmap = lightmap;
		transient_emissive_tiles[tile_index].x = local_tile % tiles_wide;
		transient_emissive_tiles[tile_index].y = local_tile / tiles_wide;
		transient_emissive_tile_indices[flat_tile] = tile_index;
	}

	emissive_tile_source_pair_t *pairs = NULL;
	int num_pairs = 0;
	int pair_capacity = 0;
	for (int work_tile = 0; work_tile < num_flat_tiles; ++work_tile)
	{
		const int flat_tile = flat_tiles[work_tile];
		const int tile = transient_emissive_tile_indices[flat_tile];
		const emissive_logical_tile_t *const logical_tile = &transient_emissive_tiles[tile];
		for (uint32_t surface_link = transient_emissive_tile_surface_offsets[flat_tile];
			surface_link < transient_emissive_tile_surface_offsets[flat_tile + 1]; ++surface_link)
		{
			const msurface_t *const surface = &first_surface[transient_emissive_tile_surfaces[surface_link]];
			for (int light_index = 0; light_index < num_transient_emissive_lights; ++light_index)
				if (R_EmissiveLightInfluencesTile (surface, logical_tile->x, logical_tile->y, &transient_emissive_lights[light_index]))
				{
					if (num_pairs == pair_capacity)
					{
						pair_capacity = pair_capacity ? pair_capacity * 2 : 1024;
						pairs = Mem_Realloc (pairs, pair_capacity * sizeof (*pairs));
					}
					pairs[num_pairs].tile = tile;
					pairs[num_pairs].source = light_index;
					++num_pairs;
				}
		}
	}
	if (num_pairs > 1)
		qsort (pairs, num_pairs, sizeof (*pairs), R_CompareEmissiveTileSourcePairs);
	for (int i = 0; i < num_pairs; ++i)
		if (!num_transient_emissive_tile_sources || pairs[i].tile != pairs[num_transient_emissive_tile_sources - 1].tile ||
			pairs[i].source != pairs[num_transient_emissive_tile_sources - 1].source)
			pairs[num_transient_emissive_tile_sources++] = pairs[i];
	if (num_transient_emissive_tile_sources)
		transient_emissive_tile_sources = Mem_Alloc (num_transient_emissive_tile_sources * sizeof (*transient_emissive_tile_sources));
	for (int i = 0; i < num_transient_emissive_tile_sources; ++i)
	{
		emissive_logical_tile_t *const tile = &transient_emissive_tiles[pairs[i].tile];
		if (!tile->num_sources)
			tile->first_source = i;
		++tile->num_sources;
		transient_emissive_tile_sources[i] = pairs[i].source;
	}
	Mem_Free (pairs);
}

/* Rebuilds the transient worklist as the union of active influence after an
 * occluder move: the last source-change footprint no longer covers every
 * receiver whose visibility changed. Runs in the update task, after the TLAS
 * task detected the movement, so counts, capacities, and uploads stay ordered. */
static void R_RebuildTransientEmissiveOccluderTiles (cb_context_t *cbx)
{
	int *flat_tiles = NULL;
	int num_flat_tiles = 0;
	int flat_capacity = 0;
	for (int flat_tile = 0; flat_tile < num_transient_emissive_total_tiles; ++flat_tile)
	{
		qboolean influenced = false;
		for (uint32_t surface_link = transient_emissive_tile_surface_offsets[flat_tile];
			surface_link < transient_emissive_tile_surface_offsets[flat_tile + 1]; ++surface_link)
			if (transient_emissive_surface_influence_counts[transient_emissive_tile_surfaces[surface_link]] > 0)
			{
				influenced = true;
				break;
			}
		if (!influenced)
			continue;
		if (num_flat_tiles == flat_capacity)
		{
			flat_capacity = flat_capacity ? flat_capacity * 2 : 64;
			flat_tiles = Mem_Realloc (flat_tiles, flat_capacity * sizeof (*flat_tiles));
		}
		flat_tiles[num_flat_tiles++] = flat_tile;
	}
	if (!num_flat_tiles)
	{
		/* No active influence left: withhold replacement work and let selection fall
		 * back to direct-only output rather than publishing a vacuous generation. */
		transient_emissive_detail_pending = false;
		Mem_Free (flat_tiles);
		return;
	}
	int *const lightmap_offsets = Mem_Alloc (lightmap_count * sizeof (*lightmap_offsets));
	R_TransientEmissiveLightmapTileOffsets (lightmap_offsets);
	R_FinalizeTransientEmissiveTiles (flat_tiles, num_flat_tiles, lightmap_offsets);
	Mem_Free (flat_tiles);
	Mem_Free (lightmap_offsets);
	/* Union coverage can exceed the dirty-list capacities the last setter sized. */
	R_EnsureTransientEmissiveResources ();
	R_UpdateTransientEmissiveBuffer (
		cbx->cb, transient_emissive_tiles_buffer, transient_emissive_tiles,
		num_transient_emissive_tiles * sizeof (*transient_emissive_tiles));
	if (num_transient_emissive_tile_sources)
		R_UpdateTransientEmissiveBuffer (
			cbx->cb, transient_emissive_tile_sources_buffer, transient_emissive_tile_sources,
			num_transient_emissive_tile_sources * sizeof (*transient_emissive_tile_sources));
	Con_DPrintf (
		"RT emissives: occluder refresh rebuilt %d union tile%s with %d source link%s\n", num_transient_emissive_tiles,
		num_transient_emissive_tiles == 1 ? "" : "s", num_transient_emissive_tile_sources,
		num_transient_emissive_tile_sources == 1 ? "" : "s");
}

void R_SetTransientEmissiveLights (const transient_emissive_source_t *sources, int count)
{
	const double start_time = Sys_DoubleTime ();
	if (!cl.worldmodel || count < 0)
		return;
	assert (!count || sources);
	for (int i = 1; i < count; ++i)
		assert (R_CompareTransientEmissiveSourceIds (&sources[i - 1].id, &sources[i].id) < 0);
	if (!transient_emissive_force_refresh && R_TransientEmissiveSourcesEqual (sources, count))
		return;
	const qboolean force_refresh = transient_emissive_force_refresh;
	transient_emissive_force_refresh = false;
	R_InvalidateEmissiveBrushReceiverSources ();

	SAFE_FREE (previous_transient_emissive_lights);
	SAFE_FREE (previous_transient_emissive_light_ids);
	SAFE_FREE (transient_emissive_light_seeds);
	previous_transient_emissive_lights = transient_emissive_lights;
	previous_transient_emissive_light_ids = transient_emissive_light_ids;
	num_previous_transient_emissive_lights = num_transient_emissive_lights;
	transient_emissive_lights = NULL;
	transient_emissive_light_ids = NULL;
	num_transient_emissive_lights = count;
	if (!num_previous_transient_emissive_lights && count)
		emissive_bounce_transient_force_full_refresh = true;
	if (count)
	{
		transient_emissive_lights = Mem_Alloc (count * sizeof (*transient_emissive_lights));
		transient_emissive_light_ids = Mem_Alloc (count * sizeof (*transient_emissive_light_ids));
		if (emissive_bandlimit_active)
		{
			transient_emissive_light_seeds = Mem_Alloc (count * sizeof (*transient_emissive_light_seeds));
			for (int i = 0; i < count; ++i)
				transient_emissive_light_seeds[i] = R_TransientEmissiveSourceSeed (sources[i].id);
		}
		for (int i = 0; i < count; ++i)
		{
			transient_emissive_light_ids[i] = sources[i].id;
			transient_emissive_lights[i] = sources[i].light;
		}
	}

	R_AllocateEmissiveLightmaps ();
	int *const lightmap_offsets = Mem_Alloc (lightmap_count * sizeof (*lightmap_offsets));
	const int total_tiles = R_TransientEmissiveLightmapTileOffsets (lightmap_offsets);
	R_BuildTransientEmissiveTileSurfaces (lightmap_offsets, total_tiles);
	++transient_emissive_tile_generation;
	if (!transient_emissive_tile_generation)
	{
		memset (transient_emissive_tile_generations, 0, total_tiles * sizeof (*transient_emissive_tile_generations));
		++transient_emissive_tile_generation;
	}
	int *dirty_tiles = NULL;
	int num_dirty_tiles = 0;
	int dirty_tile_capacity = 0;
	num_transient_emissive_touched_surfaces = 0;
	++transient_emissive_surface_delta_generation;
	if (!transient_emissive_surface_delta_generation)
	{
		memset (transient_emissive_surface_delta_generations, 0,
			cl.worldmodel->nummodelsurfaces * sizeof (*transient_emissive_surface_delta_generations));
		++transient_emissive_surface_delta_generation;
	}
	qboolean regroup = false;
	int old_light_index = 0;
	int new_light_index = 0;
	int num_changed_sources = 0;
	int num_direct_classification_changes = 0;
	int num_effective_group_changes = 0;
	while (old_light_index < num_previous_transient_emissive_lights || new_light_index < count)
	{
		const transient_emissive_source_id_t *const old_id =
			old_light_index < num_previous_transient_emissive_lights ? &previous_transient_emissive_light_ids[old_light_index] : NULL;
		const transient_emissive_source_id_t *const new_id = new_light_index < count ? &transient_emissive_light_ids[new_light_index] : NULL;
		const int comparison = !old_id ? 1 : !new_id ? -1 : R_CompareTransientEmissiveSourceIds (old_id, new_id);
		const emissive_light_t *const old_light = comparison <= 0 ? &previous_transient_emissive_lights[old_light_index++] : NULL;
		const emissive_light_t *const new_light = comparison >= 0 ? &transient_emissive_lights[new_light_index++] : NULL;
		if (!force_refresh && old_light && new_light && R_TransientEmissiveLightsEqual (old_light, new_light))
			continue;
		++num_changed_sources;
		transient_emissive_invalidation_t invalidation = {
			NULL, lightmap_offsets, &dirty_tiles, &num_dirty_tiles, &dirty_tile_capacity, 0};
		if (old_light)
		{
			invalidation.light = old_light;
			invalidation.surface_influence_delta = -1;
			R_InvalidateTransientEmissiveNode (cl.worldmodel->nodes, &invalidation);
		}
		if (new_light)
		{
			invalidation.light = new_light;
			invalidation.surface_influence_delta = 1;
			R_InvalidateTransientEmissiveNode (cl.worldmodel->nodes, &invalidation);
		}
	}
	msurface_t *const first_surface = &cl.worldmodel->surfaces[cl.worldmodel->firstmodelsurface];
	for (int i = 0; i < num_transient_emissive_touched_surfaces; ++i)
	{
		const int surface_index = transient_emissive_touched_surfaces[i];
		msurface_t *const surface = &first_surface[surface_index];
		transient_emissive_surface_influence_counts[surface_index] += transient_emissive_surface_deltas[surface_index];
		assert (transient_emissive_surface_influence_counts[surface_index] >= 0);
		const qboolean influenced =
			surface->cacheable_emissive_influence || transient_emissive_surface_influence_counts[surface_index] > 0;
		if (surface->emissive_influence != influenced)
		{
			const qboolean old_group = R_SurfaceUsesEmissiveAtlas (surface);
			surface->emissive_influence = influenced;
			const qboolean group_changed = old_group != R_SurfaceUsesEmissiveAtlas (surface);
			++num_direct_classification_changes;
			num_effective_group_changes += group_changed;
			regroup |= group_changed;
		}
	}

	R_FinalizeTransientEmissiveTiles (dirty_tiles, num_dirty_tiles, lightmap_offsets);

	transient_emissive_pending = num_transient_emissive_tiles > 0;
	transient_emissive_detail_pending = false;
	R_InvalidateTransientEmissiveDetail ();
	R_EnsureTransientEmissiveResources ();
	if (!transient_emissive_initialized)
		transient_emissive_pending = true;
	if (transient_emissive_pending || transient_emissive_detail_pending)
	{
		if (++emissive_transient_direct_epoch == 0)
			++emissive_transient_direct_epoch;
		emissive_transient_bounce_epoch = 0;
		emissive_bounce_transient_refresh_pending = emissive_bounce_ready;
	}
	if (num_transient_emissive_tiles && vulkan_globals.ray_query && R_TransientEmissiveDetailAvailable ())
		GL_RequestAccelerationStructure (RT_AS_CONSUMER_TRANSIENT_EMISSIVES);
	if (regroup)
		GL_RebuildIndirectDraws (true, true);
	transient_emissive_cpu_time_us = (uint32_t)((Sys_DoubleTime () - start_time) * 1000000.0);
	Con_DPrintf (
		"RT emissives: %d transient source%s, %d changed; %d direct classification change%s, %d effective regroup change%s; "
		"updated %d logical tile%s with %d source link%s in %.3f ms CPU\n",
		count, count == 1 ? "" : "s", num_changed_sources, num_direct_classification_changes,
		num_direct_classification_changes == 1 ? "" : "s", num_effective_group_changes, num_effective_group_changes == 1 ? "" : "s",
		num_transient_emissive_tiles, num_transient_emissive_tiles == 1 ? "" : "s", num_transient_emissive_tile_sources,
		num_transient_emissive_tile_sources == 1 ? "" : "s", (double)transient_emissive_cpu_time_us / 1000.0);
	Mem_Free (dirty_tiles);
	Mem_Free (lightmap_offsets);
}

static void R_BuildEmissiveLogicalTileBounds (void)
{
	SAFE_FREE (emissive_logical_tile_mins);
	SAFE_FREE (emissive_logical_tile_maxs);
	SAFE_FREE (emissive_occluder_dirty_tile_bits);
	SAFE_FREE (emissive_occluder_tiles);
	num_emissive_occluder_dirty_tiles = 0;
	if (!num_emissive_logical_tiles)
		return;

	emissive_logical_tile_mins = Mem_Alloc (num_emissive_logical_tiles * sizeof (*emissive_logical_tile_mins));
	emissive_logical_tile_maxs = Mem_Alloc (num_emissive_logical_tiles * sizeof (*emissive_logical_tile_maxs));
	emissive_occluder_dirty_tile_bits = Mem_Alloc (num_emissive_logical_tiles * sizeof (*emissive_occluder_dirty_tile_bits));
	emissive_occluder_tiles = Mem_Alloc (num_emissive_logical_tiles * sizeof (*emissive_occluder_tiles));
	msurface_t *const first_surface = &cl.worldmodel->surfaces[cl.worldmodel->firstmodelsurface];
	for (int tile_index = 0; tile_index < num_emissive_logical_tiles; ++tile_index)
	{
		const emissive_logical_tile_t *const tile = &emissive_logical_tiles[tile_index];
		vec3_t *const mins = &emissive_logical_tile_mins[tile_index];
		vec3_t *const maxs = &emissive_logical_tile_maxs[tile_index];
		for (int axis = 0; axis < 3; ++axis)
		{
			(*mins)[axis] = FLT_MAX;
			(*maxs)[axis] = -FLT_MAX;
		}
		for (int surface_index = 0; surface_index < cl.worldmodel->nummodelsurfaces; ++surface_index)
		{
			const msurface_t *const surface = &first_surface[surface_index];
			emissive_surface_tile_rect_t rect;
			if (!R_EmissiveSurfaceTileRect (surface, &rect) || rect.lightmap != tile->lightmap || tile->x < rect.first_x ||
				tile->x > rect.last_x || tile->y < rect.first_y || tile->y > rect.last_y)
				continue;
			vec3_t surface_mins, surface_maxs;
			if (!R_EmissiveSurfaceTileBounds (surface, tile->x, tile->y, surface_mins, surface_maxs))
				continue;
			for (int axis = 0; axis < 3; ++axis)
			{
				(*mins)[axis] = q_min ((*mins)[axis], surface_mins[axis]);
				(*maxs)[axis] = q_max ((*maxs)[axis], surface_maxs[axis]);
			}
		}
		assert ((*mins)[0] <= (*maxs)[0] && (*mins)[1] <= (*maxs)[1] && (*mins)[2] <= (*maxs)[2]);
	}
}

static void R_BuildEmissiveLogicalTileSources (
	const emissive_light_t *lights, int num_lights, const emissive_surface_light_t *surface_lights, int num_surface_lights)
{
	SAFE_FREE (emissive_logical_tile_sources);
	num_emissive_logical_tile_sources = 0;
	for (int i = 0; i < num_emissive_logical_tiles; ++i)
	{
		emissive_logical_tiles[i].first_source = 0;
		emissive_logical_tiles[i].num_sources = 0;
	}
	if (!surface_lights || !num_surface_lights || !num_emissive_logical_tiles)
		return;

	int *const lightmap_offsets = Mem_Alloc (lightmap_count * sizeof (*lightmap_offsets));
	const int  total_tiles = R_EmissiveLightmapTileOffsets (lightmap_offsets);
	int *const tile_lookup = Mem_Alloc (total_tiles * sizeof (*tile_lookup));
	memset (tile_lookup, 0xFF, total_tiles * sizeof (*tile_lookup));
	for (int i = 0; i < num_emissive_logical_tiles; ++i)
	{
		const emissive_logical_tile_t *const tile = &emissive_logical_tiles[i];
		const int tiles_wide = (lightmaps[tile->lightmap].surface_indices_texture->width + 7) / 8;
		tile_lookup[lightmap_offsets[tile->lightmap] + tile->y * tiles_wide + tile->x] = i;
	}

	emissive_tile_source_pair_t *tile_sources = NULL;
	int						 num_tile_sources = 0;
	int						 tile_source_capacity = 0;
	msurface_t *const first_surface = &cl.worldmodel->surfaces[cl.worldmodel->firstmodelsurface];
	for (int i = 0; i < num_surface_lights; ++i)
	{
		const emissive_surface_light_t *const surface_light = &surface_lights[i];
		if (surface_light->surface >= (uint32_t)cl.worldmodel->nummodelsurfaces || surface_light->light >= (uint32_t)num_lights)
			continue;
		const msurface_t *const surface = &first_surface[surface_light->surface];
		emissive_surface_tile_rect_t rect;
		if (!R_EmissiveSurfaceTileRect (surface, &rect))
			continue;
		for (int y = rect.first_y; y <= rect.last_y; ++y)
			for (int x = rect.first_x; x <= rect.last_x; ++x)
			{
				if (!R_EmissiveLightInfluencesTile (surface, x, y, &lights[surface_light->light]))
					continue;
				const int tile = tile_lookup[lightmap_offsets[rect.lightmap] + y * rect.tiles_wide + x];
				assert (tile >= 0);
				if (num_tile_sources == tile_source_capacity)
				{
					tile_source_capacity = tile_source_capacity ? tile_source_capacity * 2 : 1024;
					tile_sources = Mem_Realloc (tile_sources, tile_source_capacity * sizeof (*tile_sources));
				}
				tile_sources[num_tile_sources].tile = tile;
				tile_sources[num_tile_sources].source = surface_light->light;
				++num_tile_sources;
			}
	}

	if (num_tile_sources > 1)
		qsort (tile_sources, num_tile_sources, sizeof (*tile_sources), R_CompareEmissiveTileSourcePairs);
	for (int i = 0; i < num_tile_sources; ++i)
		if (!num_emissive_logical_tile_sources || tile_sources[i].tile != tile_sources[num_emissive_logical_tile_sources - 1].tile ||
			tile_sources[i].source != tile_sources[num_emissive_logical_tile_sources - 1].source)
			tile_sources[num_emissive_logical_tile_sources++] = tile_sources[i];

	if (num_emissive_logical_tile_sources)
		emissive_logical_tile_sources = Mem_Alloc (num_emissive_logical_tile_sources * sizeof (*emissive_logical_tile_sources));
	for (int i = 0; i < num_emissive_logical_tile_sources; ++i)
	{
		emissive_logical_tile_t *const tile = &emissive_logical_tiles[tile_sources[i].tile];
		if (!tile->num_sources)
			tile->first_source = i;
		emissive_logical_tile_sources[i] = tile_sources[i].source;
		++tile->num_sources;
	}
	int affected_tiles = 0;
	for (int i = 0; i < num_emissive_logical_tiles; ++i)
		if (emissive_logical_tiles[i].num_sources)
			emissive_logical_tiles[affected_tiles++] = emissive_logical_tiles[i];
	num_emissive_logical_tiles = affected_tiles;
	if (num_emissive_logical_tiles)
		emissive_logical_tiles = Mem_Realloc (emissive_logical_tiles, num_emissive_logical_tiles * sizeof (*emissive_logical_tiles));
	else
		SAFE_FREE (emissive_logical_tiles);
	R_BuildEmissiveLogicalTileBounds ();
	Con_DPrintf (
		"RT emissives: %d affected 8x8 tile%s, %d tile-source link%s (%.2f per tile, %" PRIu64 " persistent CPU bytes)\n",
		num_emissive_logical_tiles, num_emissive_logical_tiles == 1 ? "" : "s", num_emissive_logical_tile_sources,
		num_emissive_logical_tile_sources == 1 ? "" : "s",
		num_emissive_logical_tiles ? (double)num_emissive_logical_tile_sources / num_emissive_logical_tiles : 0.0,
		(uint64_t)num_emissive_logical_tiles * sizeof (*emissive_logical_tiles) +
			(uint64_t)num_emissive_logical_tile_sources * sizeof (*emissive_logical_tile_sources));

	Mem_Free (tile_sources);
	Mem_Free (tile_lookup);
	Mem_Free (lightmap_offsets);
}

/*
==================
R_AllocateEmissiveLightmaps
==================
*/
void R_AllocateEmissiveLightmaps (void)
{
	if (!cl.worldmodel || !lightmap_count)
		return;

	const qboolean admission_was_limited = emissive_detail_budget_limited;
	qboolean *const world_lightmaps = Mem_Alloc (lightmap_count * sizeof (*world_lightmaps));
	memset (world_lightmaps, 0, lightmap_count * sizeof (*world_lightmaps));
	msurface_t *const first_surface = &cl.worldmodel->surfaces[cl.worldmodel->firstmodelsurface];
	for (int i = 0; i < cl.worldmodel->nummodelsurfaces; ++i)
	{
		const msurface_t *const surface = &first_surface[i];
		if (!(surface->flags & SURF_DRAWTILED) && surface->lightmaptexturenum >= 0 && surface->lightmaptexturenum < lightmap_count)
			world_lightmaps[surface->lightmaptexturenum] = true;
	}

	const uint64_t detail_budget_bytes = (uint64_t)EMISSIVE_DETAIL_MEMORY_BUDGET_MB * 1024 * 1024;
	uint64_t	   detail_logical_bytes = 0;
	uint64_t	   detail_required_bytes = 0;
	if (vulkan_globals.ray_query)
		for (int i = 0; i < lightmap_count; ++i)
			if (world_lightmaps[i])
			{
				const struct lightmap_s *const lightmap = &lightmaps[i];
				detail_logical_bytes += (uint64_t)lightmap->surface_indices_texture->width * R_EmissiveDetailScale () *
					lightmap->surface_indices_texture->height * R_EmissiveDetailScale () * 8;
			}
	if (detail_logical_bytes > detail_budget_bytes)
		emissive_detail_budget_limited = true;
	if (vulkan_globals.ray_query && !emissive_detail_budget_limited)
		for (int i = 0; i < lightmap_count; ++i)
			if (world_lightmaps[i])
			{
				const struct lightmap_s *const lightmap = &lightmaps[i];
				detail_required_bytes += TexMgr_RGBA16FImageMemorySize (
					lightmap->surface_indices_texture->width * R_EmissiveDetailScale (),
					lightmap->surface_indices_texture->height * R_EmissiveDetailScale ());
			}
	if (detail_required_bytes > detail_budget_bytes)
		emissive_detail_budget_limited = true;

	uint64_t detail_allocated_bytes = 0;
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		if (!world_lightmaps[i])
			continue;
		char name[32];
		const int width = lightmap->surface_indices_texture->width;
		const int height = lightmap->surface_indices_texture->height;
		if (!lightmap->emissive_texture)
		{
			q_snprintf (name, sizeof (name), "emissive_coarse_%07i", i);
			lightmap->emissive_texture = TexMgr_LoadImage (
				cl.worldmodel, name, width, height, SRC_RGBA16F, NULL, "", 0, TEXPREF_LINEAR | TEXPREF_NOPICMIP);
		}
		if (vulkan_globals.ray_query && !emissive_detail_budget_limited && !lightmap->emissive_detail_texture)
		{
			q_snprintf (name, sizeof (name), "emissive_detail_%07i", i);
			lightmap->emissive_detail_texture = TexMgr_LoadImage (
				cl.worldmodel, name, width * R_EmissiveDetailScale (), height * R_EmissiveDetailScale (), SRC_RGBA16F, NULL, "", 0,
				TEXPREF_LINEAR | TEXPREF_NOPICMIP);
		}
		if (lightmap->emissive_detail_texture)
			detail_allocated_bytes += GL_HeapGetAllocationSize (lightmap->emissive_detail_texture->allocation);
	}
	assert (detail_allocated_bytes <= detail_budget_bytes);
	if (emissive_detail_budget_limited && !admission_was_limited)
	{
		if (detail_logical_bytes > detail_budget_bytes)
			Con_DPrintf (
				"RT emissives: dense detail rejected (%" PRIu64 " logical bytes, %" PRIu64 " byte budget); using coarse fallback\n",
				detail_logical_bytes, detail_budget_bytes);
		else
			Con_DPrintf (
				"RT emissives: dense detail rejected (%" PRIu64 " required Vulkan bytes, %" PRIu64 " byte budget); using coarse fallback\n",
				detail_required_bytes, detail_budget_bytes);
	}
	R_BuildEmissiveLogicalTiles ();
	Mem_Free (world_lightmaps);
}

static void R_DeleteEmissiveBounceResources (void)
{
	for (int i = 0; i < lightmap_count; ++i)
	{
		if (lightmaps[i].emissive_bounce_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmaps[i].emissive_bounce_descriptor_set, &vulkan_globals.emissive_bounce_set_layout);
			lightmaps[i].emissive_bounce_descriptor_set = VK_NULL_HANDLE;
		}
		if (lightmaps[i].emissive_bounce_detail_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmaps[i].emissive_bounce_detail_descriptor_set, &vulkan_globals.emissive_bounce_set_layout);
			lightmaps[i].emissive_bounce_detail_descriptor_set = VK_NULL_HANDLE;
		}
		VkDescriptorSet *const bounce_sets[] = {
			&lightmaps[i].emissive_bounce_output_descriptor_set,
			&lightmaps[i].emissive_bounce_output_detail_descriptor_set,
			&lightmaps[i].emissive_bounce_transient_descriptor_set,
			&lightmaps[i].emissive_bounce_transient_detail_descriptor_set,
			&lightmaps[i].emissive_bounce_transient_output_descriptor_set,
			&lightmaps[i].emissive_bounce_transient_output_detail_descriptor_set,
		};
		for (int set = 0; set < countof (bounce_sets); ++set)
			if (*bounce_sets[set] != VK_NULL_HANDLE)
			{
				R_FreeDescriptorSet (*bounce_sets[set], &vulkan_globals.emissive_bounce_set_layout);
				*bounce_sets[set] = VK_NULL_HANDLE;
			}
		if (lightmaps[i].emissive_bounce_debug_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmaps[i].emissive_bounce_debug_descriptor_set, &vulkan_globals.emissive_bounce_set_layout);
			lightmaps[i].emissive_bounce_debug_descriptor_set = VK_NULL_HANDLE;
		}
	}
	if (emissive_bounce_surfaces_buffer != VK_NULL_HANDLE)
	{
		VkBuffer buffers[] = {emissive_bounce_surfaces_buffer, emissive_bounce_samples_buffer, emissive_bounce_taps_buffer,
			emissive_bounce_direct_buffer, emissive_bounce_reflectance_buffer, emissive_bounce_values_buffer, emissive_bounce_filtered_buffer};
		R_FreeBuffers (countof (buffers), buffers, &emissive_bounce_memory, &num_vulkan_bmodel_allocations);
	}
	if (emissive_bounce_counters_buffer != VK_NULL_HANDLE)
	{
		vkUnmapMemory (vulkan_globals.device, emissive_bounce_counters_memory.handle);
		R_FreeBuffer (emissive_bounce_counters_buffer, &emissive_bounce_counters_memory, &num_vulkan_bmodel_allocations);
	}
	emissive_bounce_surfaces_buffer = emissive_bounce_samples_buffer = emissive_bounce_taps_buffer = VK_NULL_HANDLE;
	emissive_bounce_direct_buffer = emissive_bounce_reflectance_buffer = emissive_bounce_values_buffer = VK_NULL_HANDLE;
	emissive_bounce_filtered_buffer = emissive_bounce_counters_buffer = VK_NULL_HANDLE;
	emissive_bounce_counters = NULL;
	SAFE_FREE (emissive_bounce_surface_metadata);
	SAFE_FREE (emissive_bounce_surface_mins);
	SAFE_FREE (emissive_bounce_surface_maxs);
	num_emissive_bounce_direct_texels = num_emissive_bounce_samples = emissive_bounce_rays_per_sample = 0;
	emissive_bounce_sample_spacing = 0;
	emissive_bounce_logical_bytes = emissive_bounce_required_bytes = 0;
	emissive_bounce_prepare_time_us = emissive_bounce_build_time_us = emissive_bounce_resolve_time_us = 0;
	emissive_bounce_filter_time_us = emissive_bounce_combine_time_us = 0;
	emissive_bounce_pending = emissive_bounce_building = emissive_bounce_recorded = emissive_bounce_ready = false;
	emissive_bounce_recombine_pending = emissive_bounce_cacheable_refresh_pending = emissive_bounce_transient_refresh_pending = false;
	emissive_bounce_cacheable_force_full_refresh = false;
	emissive_bounce_transient_outputs_initialized = emissive_bounce_transient_force_full_refresh = false;
	emissive_bounce_capture_owner = 0;
	emissive_cacheable_bounce_epoch = emissive_transient_bounce_epoch = 0;
	emissive_bounce_no_ray_refreshes = emissive_bounce_dirty_receiver_surfaces = emissive_bounce_refresh_cpu_time_us = 0;
	emissive_bounce_budget_limited = emissive_bounce_transient_budget_limited = false;
	emissive_bounce_gpu_time_valid = emissive_bounce_admission_attempted = false;
	emissive_bounce_debug_pending = emissive_bounce_debug_ready = emissive_bounce_debug_budget_limited = false;
	GL_ResetEmissiveBounceTimestamp ();
	GL_ResetEmissiveBounceRefreshTimestamp ();
}

static void R_SetEmissiveBounceInfluence (qboolean active)
{
	if (!cl.worldmodel)
		return;

	qboolean regroup = false;
	msurface_t *const first = &cl.worldmodel->surfaces[cl.worldmodel->firstmodelsurface];
	for (int i = 0; i < cl.worldmodel->nummodelsurfaces; ++i)
	{
		msurface_t *const surface = &first[i];
		const qboolean influenced =
			active && surface->lightmaptexturenum >= 0 && R_SurfaceInEmissiveWorldAccelerationStructure (surface);
		if (surface->emissive_bounce_influence != influenced)
		{
			const qboolean old_group = R_SurfaceUsesEmissiveAtlas (surface);
			surface->emissive_bounce_influence = influenced;
			regroup |= old_group != R_SurfaceUsesEmissiveAtlas (surface);
		}
	}
	if (regroup && indirect_emissive_grouping)
		GL_RebuildIndirectDraws (true, true);
}

static uint32_t R_EmissiveBounceSampleSpacing (void)
{
	return CLAMP (0, (int)r_emissive_rt_bounce_resolution.value, 1) ? 1 : 2;
}

static float R_EmissiveBounceReflectanceLift (void)
{
	return CLAMP (0.0f, r_emissive_rt_bounce_reflectance.value, 1.0f);
}

static void R_BuildEmissiveBounceResources (void)
{
	if (!vulkan_globals.ray_query || r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f || !cl.worldmodel ||
		!R_EmissiveDetailAvailable () || emissive_bounce_surfaces_buffer != VK_NULL_HANDLE || emissive_bounce_admission_attempted ||
		r_emissive_rt_bounce.value <= 0.0f || r_emissive_rt_bounce_strength.value <= 0.0f)
		return;
	emissive_bounce_admission_attempted = true;
	emissive_bounce_rays_per_sample = CLAMP (1, (int)r_emissive_rt_bounce_rays.value, EMISSIVE_BOUNCE_MAX_RAYS);
	emissive_bounce_sample_spacing = R_EmissiveBounceSampleSpacing ();
	const double start = Sys_DoubleTime ();
	emissive_bounce_surface_metadata = Mem_Alloc (num_surfaces * sizeof (*emissive_bounce_surface_metadata));
	emissive_bounce_surface_mins = Mem_Alloc (num_surfaces * sizeof (*emissive_bounce_surface_mins));
	emissive_bounce_surface_maxs = Mem_Alloc (num_surfaces * sizeof (*emissive_bounce_surface_maxs));
	emissive_bounce_surface_t *const surfaces = emissive_bounce_surface_metadata;
	memset (surfaces, 0xFF, num_surfaces * sizeof (*surfaces));
	msurface_t *const first = &cl.worldmodel->surfaces[cl.worldmodel->firstmodelsurface];
	for (int i = 0; i < cl.worldmodel->nummodelsurfaces; ++i)
	{
		msurface_t *const surface = &first[i];
		const uint32_t index = (uint32_t)(surface - cl.worldmodel->surfaces);
		R_EmissiveBounceSurfaceBounds (surface, emissive_bounce_surface_mins[index], emissive_bounce_surface_maxs[index]);
		if (!R_SurfaceInEmissiveWorldAccelerationStructure (surface) || surface->lightmaptexturenum < 0)
			continue;
		const uint32_t width = (surface->extents[0] >> 4) + 1, height = (surface->extents[1] >> 4) + 1;
		const uint32_t bounce_width = (width + emissive_bounce_sample_spacing - 1) / emissive_bounce_sample_spacing;
		const uint32_t bounce_height = (height + emissive_bounce_sample_spacing - 1) / emissive_bounce_sample_spacing;
		surfaces[index].direct_base = num_emissive_bounce_direct_texels;
		surfaces[index].bounce_base = num_emissive_bounce_samples;
		surfaces[index].packed_direct_size = width | (height << 16);
		surfaces[index].packed_bounce_size = bounce_width | (bounce_height << 16);
		num_emissive_bounce_direct_texels += width * height;
		num_emissive_bounce_samples += bounce_width * bounce_height;
	}
	if (!num_emissive_bounce_direct_texels || !num_emissive_bounce_samples)
	{
		SAFE_FREE (emissive_bounce_surface_metadata);
		SAFE_FREE (emissive_bounce_surface_mins);
		SAFE_FREE (emissive_bounce_surface_maxs);
		return;
	}
	emissive_bounce_sample_t *const samples = Mem_Alloc (num_emissive_bounce_samples * sizeof (*samples));
	vec4_t *const reflectance = Mem_Alloc (num_emissive_bounce_direct_texels * sizeof (*reflectance));
	for (int i = 0; i < cl.worldmodel->nummodelsurfaces; ++i)
	{
		msurface_t *const surface = &first[i];
		const uint32_t index = (uint32_t)(surface - cl.worldmodel->surfaces);
		const emissive_bounce_surface_t *const meta = &surfaces[index];
		if (meta->direct_base == UINT32_MAX)
			continue;
		const uint32_t width = meta->packed_direct_size & 0xFFFF, height = meta->packed_direct_size >> 16;
		const uint32_t bounce_width = meta->packed_bounce_size & 0xFFFF, bounce_height = meta->packed_bounce_size >> 16;
		const gltexture_t *const texture = surface->texinfo && surface->texinfo->texture ? surface->texinfo->texture->gltexture : NULL;
		for (uint32_t texel = 0; texel < width * height; ++texel)
		{
			vec4_t *const value = &reflectance[meta->direct_base + texel];
			for (int channel = 0; channel < 3; ++channel)
				(*value)[channel] = texture ? texture->diffuse_color[channel] : 0.5f;
			(*value)[3] = 0.0f;
		}
		for (uint32_t y = 0; y < bounce_height; ++y)
			for (uint32_t x = 0; x < bounce_width; ++x)
			{
				emissive_bounce_sample_t *const sample = &samples[meta->bounce_base + y * bounce_width + x];
				sample->surface = index;
				sample->packed_st = x | (y << 16);
			}
	}
	const size_t surface_bytes = num_surfaces * sizeof (*surfaces), sample_bytes = num_emissive_bounce_samples * sizeof (*samples);
	const size_t tap_bytes = (size_t)num_emissive_bounce_samples * emissive_bounce_rays_per_sample * sizeof (uint32_t);
	const size_t direct_bytes = (size_t)num_emissive_bounce_direct_texels * sizeof (vec4_t);
	const size_t bounce_bytes = (size_t)num_emissive_bounce_samples * sizeof (vec4_t);
	const size_t filtered_bytes = direct_bytes;
	uint64_t output_logical_bytes = 0, output_required_bytes = 0;
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		gltexture_t *const inputs[] = {lightmap->emissive_texture, lightmap->emissive_detail_texture};
		gltexture_t *const outputs[] = {lightmap->emissive_bounce_texture, lightmap->emissive_bounce_detail_texture};
		for (int detail = 0; detail < 2; ++detail)
			if (inputs[detail])
			{
				output_logical_bytes += (uint64_t)inputs[detail]->width * inputs[detail]->height * 8;
				output_required_bytes += outputs[detail] ? GL_HeapGetAllocationSize (outputs[detail]->allocation)
											 : TexMgr_RGBA16FImageMemorySize (inputs[detail]->width, inputs[detail]->height);
			}
		gltexture_t *const optional_outputs[] = {
			lightmap->emissive_transient_bounce_texture,
			lightmap->emissive_transient_bounce_detail_texture,
			lightmap->emissive_bounce_debug_texture,
		};
		for (int texture = 0; texture < countof (optional_outputs); ++texture)
			if (optional_outputs[texture])
			{
				output_logical_bytes += (uint64_t)optional_outputs[texture]->width * optional_outputs[texture]->height * 8;
				output_required_bytes += GL_HeapGetAllocationSize (optional_outputs[texture]->allocation);
			}
	}
	const VkBufferUsageFlags uploaded_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	emissive_bounce_logical_bytes =
		surface_bytes + sample_bytes + tap_bytes + direct_bytes * 3 + bounce_bytes + 2 * sizeof (uint32_t) + output_logical_bytes;
	emissive_bounce_required_bytes = R_EmissiveBufferMemorySize (surface_bytes, uploaded_usage) +
		R_EmissiveBufferMemorySize (sample_bytes, uploaded_usage) + R_EmissiveBufferMemorySize (tap_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) +
		R_EmissiveBufferMemorySize (direct_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) + R_EmissiveBufferMemorySize (direct_bytes, uploaded_usage) +
		R_EmissiveBufferMemorySize (bounce_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) +
		R_EmissiveBufferMemorySize (filtered_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) +
		R_EmissiveBufferMemorySize (2 * sizeof (uint32_t), uploaded_usage) + output_required_bytes;
	if (emissive_bounce_required_bytes > (uint64_t)EMISSIVE_BOUNCE_MEMORY_BUDGET_MB * 1024 * 1024)
		emissive_bounce_budget_limited = true;
	else
	{
		buffer_create_info_t infos[] = {
			{&emissive_bounce_surfaces_buffer, surface_bytes, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL, NULL, "Emissive bounce surfaces"},
			{&emissive_bounce_samples_buffer, sample_bytes, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL, NULL, "Emissive bounce samples"},
			{&emissive_bounce_taps_buffer, tap_bytes, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, NULL, NULL, "Emissive bounce taps"},
			{&emissive_bounce_direct_buffer, direct_bytes, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, NULL, NULL, "Emissive bounce direct"},
			{&emissive_bounce_reflectance_buffer, direct_bytes, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL, NULL, "Emissive bounce reflectance"},
			{&emissive_bounce_values_buffer, bounce_bytes, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, NULL, NULL, "Emissive bounce values"},
			{&emissive_bounce_filtered_buffer, filtered_bytes, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, NULL, NULL, "Emissive bounce filtered values"},
		};
		R_CreateBuffers (countof (infos), infos, &emissive_bounce_memory, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
			&num_vulkan_bmodel_allocations, "Emissive bounce");
		R_StagingUploadBuffer (emissive_bounce_surfaces_buffer, surface_bytes, (byte *)surfaces);
		R_StagingUploadBuffer (emissive_bounce_samples_buffer, sample_bytes, (byte *)samples);
		R_StagingUploadBuffer (emissive_bounce_reflectance_buffer, direct_bytes, (byte *)reflectance);
		R_CreateBuffer (&emissive_bounce_counters_buffer, &emissive_bounce_counters_memory, 2 * sizeof (uint32_t), uploaded_usage,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &num_vulkan_bmodel_allocations, NULL,
			"Emissive bounce counters");
		VkResult err = vkMapMemory (vulkan_globals.device, emissive_bounce_counters_memory.handle, 0, VK_WHOLE_SIZE, 0, (void **)&emissive_bounce_counters);
		if (err != VK_SUCCESS)
			Sys_Error ("vkMapMemory failed with code %i", (int)err);
		emissive_bounce_counters[0] = emissive_bounce_counters[1] = 0;
		for (int i = 0; i < lightmap_count; ++i)
		{
			struct lightmap_s *const lightmap = &lightmaps[i];
			gltexture_t *inputs[] = {lightmap->emissive_texture, lightmap->emissive_detail_texture};
			gltexture_t **outputs[] = {&lightmap->emissive_bounce_texture, &lightmap->emissive_bounce_detail_texture};
			for (int detail = 0; detail < 2; ++detail)
				if (inputs[detail] && !*outputs[detail])
				{
					char name[48];
					q_snprintf (name, sizeof (name), detail ? "emissive_bounce_detail_%07i" : "emissive_bounce_%07i", i);
					*outputs[detail] = TexMgr_LoadImage (
						cl.worldmodel, name, inputs[detail]->width, inputs[detail]->height, SRC_RGBA16F, NULL, "", 0,
						TEXPREF_LINEAR | TEXPREF_NOPICMIP);
				}
		}
		emissive_bounce_pending = true;
		R_SetEmissiveBounceInfluence (true);
		if (CLAMP (0, (int)r_emissive_rt_debug.value, 11) == 5)
			R_AllocateEmissiveBounceDebugLightmaps ();
	}
	emissive_bounce_prepare_time_us = (uint32_t)((Sys_DoubleTime () - start) * 1000000.0);
	Mem_Free (reflectance);
	Mem_Free (samples);
}

/*
==================
R_EmissiveDetailLightmapStats
==================
*/
void R_EmissiveDetailLightmapStats (
	int *count, uint64_t *logical_bytes, uint64_t *allocated_bytes, uint64_t *budget_bytes, qboolean *budget_limited, qboolean *pending,
	qboolean *as_active, qboolean *ready)
{
	*count = 0;
	*logical_bytes = 0;
	*allocated_bytes = 0;
	for (int i = 0; i < lightmap_count; ++i)
		if (lightmaps[i].emissive_detail_texture)
		{
			++*count;
			*logical_bytes += (uint64_t)lightmaps[i].emissive_detail_texture->width * lightmaps[i].emissive_detail_texture->height * 8;
			*allocated_bytes += GL_HeapGetAllocationSize (lightmaps[i].emissive_detail_texture->allocation);
		}
	*budget_bytes = (uint64_t)EMISSIVE_DETAIL_MEMORY_BUDGET_MB * 1024 * 1024;
	*budget_limited = emissive_detail_budget_limited;
	*pending = emissive_detail_pending || emissive_detail_building;
	*as_active = emissive_detail_building ||
		(emissive_detail_pending && R_EmissiveDirectAccelerationStructure () != VK_NULL_HANDLE && r_emissive_rt.value > 0.0f && gl_fullbrights.value > 0.0f);
	*ready = emissive_detail_ready;
}

/*
==================
R_EmissiveDetailDispatchCount
==================
*/
static int R_EmissiveDetailDispatchCount (void)
{
	int dispatches = 0;
	for (int i = 0; i < num_emissive_logical_tiles; ++i)
		if (!i || emissive_logical_tiles[i].lightmap != emissive_logical_tiles[i - 1].lightmap)
			++dispatches;
	return dispatches;
}

/*
==================
R_EmissiveDetailCompleted
==================
*/
void R_EmissiveDetailCompleted (void)
{
	emissive_detail_building = false;
	emissive_detail_ready = true;
	const int tiles = emissive_detail_recorded_tiles ? emissive_detail_recorded_tiles : num_emissive_logical_tiles;
	const int dispatches = emissive_detail_recorded_dispatches ? emissive_detail_recorded_dispatches : R_EmissiveDetailDispatchCount ();
	if (rs_emissive_detail_gputime_valid)
		Con_DPrintf (
			"RT emissives: completed %d detail tile%s in %d dispatch%s/%d workgroups in %.3f ms GPU\n", tiles,
			tiles == 1 ? "" : "s", dispatches, dispatches == 1 ? "" : "es",
			tiles * R_EmissiveDetailScale () * R_EmissiveDetailScale (),
			(double)rs_emissive_detail_gputime_us / 1000.0);
	else
		Con_DPrintf (
			"RT emissives: completed %d detail tile%s in %d dispatch%s/%d workgroups, GPU timing unavailable\n", tiles,
			tiles == 1 ? "" : "s", dispatches, dispatches == 1 ? "" : "es",
			tiles * R_EmissiveDetailScale () * R_EmissiveDetailScale ());
}

void R_EmissiveBounceCompleted (
	uint32_t build_time_us, uint32_t resolve_time_us, uint32_t filter_time_us, uint32_t combine_time_us, qboolean valid)
{
	emissive_bounce_build_time_us = build_time_us;
	emissive_bounce_resolve_time_us = resolve_time_us;
	emissive_bounce_filter_time_us = filter_time_us;
	emissive_bounce_combine_time_us = combine_time_us;
	emissive_bounce_gpu_time_valid = valid;
	emissive_bounce_building = emissive_bounce_recorded = false;
	emissive_bounce_ready = true;
	if (R_TransientEmissiveActive ())
	{
		emissive_transient_bounce_epoch = 0;
		emissive_bounce_transient_refresh_pending = true;
	}
	Con_DPrintf (
		"RT emissive bounce v%d: %u rays, %u valid/%u invalid taps, %" PRIu64 " allocated bytes, %.3f ms CPU prepare\n",
		EMISSIVE_BOUNCE_VERSION, num_emissive_bounce_samples * emissive_bounce_rays_per_sample, emissive_bounce_counters[0],
		emissive_bounce_counters[1], emissive_bounce_memory.size + emissive_bounce_counters_memory.size,
		(double)emissive_bounce_prepare_time_us / 1000.0);
}

void R_EmissiveBounceStats (
	uint32_t *direct_texels, uint32_t *samples, uint32_t *rays, uint32_t *valid_taps, uint32_t *invalid_taps, uint64_t *logical_bytes,
	uint64_t *allocated_bytes, uint64_t *budget_bytes, uint32_t *prepare_time_us, uint32_t *build_time_us, uint32_t *resolve_time_us,
	uint32_t *filter_time_us, uint32_t *combine_time_us, uint32_t *refresh_cpu_time_us, uint32_t *no_ray_refreshes,
	uint32_t *dirty_receiver_surfaces, uint32_t *cacheable_direct_epoch,
	uint32_t *cacheable_bounce_epoch, uint32_t *transient_direct_epoch, uint32_t *transient_bounce_epoch, qboolean *gpu_time_valid,
	qboolean *budget_limited, qboolean *pending, qboolean *ready)
{
	*direct_texels = num_emissive_bounce_direct_texels;
	*samples = num_emissive_bounce_samples;
	*rays = num_emissive_bounce_samples * emissive_bounce_rays_per_sample;
	*valid_taps = emissive_bounce_ready && emissive_bounce_counters ? emissive_bounce_counters[0] : 0;
	*invalid_taps = emissive_bounce_ready && emissive_bounce_counters ? emissive_bounce_counters[1] : 0;
	*logical_bytes = emissive_bounce_logical_bytes;
	*allocated_bytes = emissive_bounce_memory.size + emissive_bounce_counters_memory.size;
	for (int i = 0; i < lightmap_count; ++i)
	{
		gltexture_t *const textures[] = {
			lightmaps[i].emissive_bounce_texture,
			lightmaps[i].emissive_bounce_detail_texture,
			lightmaps[i].emissive_transient_bounce_texture,
			lightmaps[i].emissive_transient_bounce_detail_texture,
			lightmaps[i].emissive_bounce_debug_texture,
		};
		for (int texture = 0; texture < countof (textures); ++texture)
			if (textures[texture])
				*allocated_bytes += GL_HeapGetAllocationSize (textures[texture]->allocation);
	}
	*budget_bytes = (uint64_t)EMISSIVE_BOUNCE_MEMORY_BUDGET_MB * 1024 * 1024;
	*prepare_time_us = emissive_bounce_prepare_time_us;
	*build_time_us = emissive_bounce_build_time_us;
	*resolve_time_us = emissive_bounce_resolve_time_us;
	*filter_time_us = emissive_bounce_filter_time_us;
	*combine_time_us = emissive_bounce_combine_time_us;
	*refresh_cpu_time_us = emissive_bounce_refresh_cpu_time_us;
	*no_ray_refreshes = emissive_bounce_no_ray_refreshes;
	*dirty_receiver_surfaces = emissive_bounce_dirty_receiver_surfaces;
	*cacheable_direct_epoch = emissive_cacheable_direct_epoch;
	*cacheable_bounce_epoch = emissive_cacheable_bounce_epoch;
	*transient_direct_epoch = emissive_transient_direct_epoch;
	*transient_bounce_epoch = emissive_transient_bounce_epoch;
	*gpu_time_valid = emissive_bounce_gpu_time_valid;
	*budget_limited = emissive_bounce_budget_limited || emissive_bounce_transient_budget_limited;
	*pending = emissive_bounce_pending || emissive_bounce_building || emissive_bounce_recombine_pending ||
			   emissive_bounce_cacheable_refresh_pending || emissive_bounce_transient_refresh_pending;
	*ready = emissive_bounce_ready;
}

/*
==================
R_EmissiveDetailReady
==================
*/
qboolean R_EmissiveDetailReady (void)
{
	return emissive_detail_ready;
}

int R_EmissiveDetailScale (void)
{
	return emissive_detail_scale;
}

qboolean R_TransientEmissiveDetailReady (void)
{
	return transient_emissive_detail_ready;
}

/* Current-generation detail recorded into this frame's update commands, ahead of
 * any same-frame consumer. Draw recording runs after the update task; the
 * dispatcher's compute-to-compute/fragment image barriers, submitted on one queue
 * in index order ahead of the draws, guarantee a published generation is produced
 * before draws execute. Submission order alone is not a memory dependency. */
qboolean R_TransientEmissiveDetailPublished (void)
{
	return transient_emissive_detail_published_generation != 0 &&
		transient_emissive_detail_published_generation == transient_emissive_generation;
}

qboolean R_TransientEmissiveActive (void)
{
	return num_transient_emissive_lights > 0;
}

/* Whether world-draw recording must wait for this frame's update task. Deliberately
 * conservative: lightstyle and occluder changes discovered after graph construction
 * can still create publication work, so any active transient consumer keeps the
 * ordering. Selective bypass for settled frames comes only after a complete
 * pre-graph frame-update decision exists. */
qboolean R_TransientEmissivePublicationNeeded (void)
{
	return r_emissive_rt.value > 0.0f && gl_fullbrights.value > 0.0f && R_TransientEmissiveActive ();
}

void R_LatchEmissiveResolvedTextures (void)
{
	emissive_bounce_cacheable_latched = false;
	if (!emissive_bounce_ready || emissive_bounce_surfaces_buffer == VK_NULL_HANDLE || r_emissive_rt_bounce.value <= 0.0f ||
		r_emissive_rt_bounce_strength.value <= 0.0f)
		return;

	const qboolean cacheable_refresh = emissive_bounce_cacheable_refresh_pending || emissive_radiance_coarse_pending ||
		emissive_radiance_detail_pending;
	const qboolean cacheable_refresh_ready = !R_EmissiveDetailAvailable () || emissive_detail_ready;
	if (cacheable_refresh)
		emissive_bounce_cacheable_latched = cacheable_refresh_ready;
	else
		emissive_bounce_cacheable_latched = emissive_cacheable_bounce_epoch == emissive_cacheable_direct_epoch;
}

void R_EmissiveResolvedTextures (int lightmap_index, gltexture_t **coarse, gltexture_t **detail)
{
	*coarse = *detail = NULL;
	if (lightmap_index < 0 || lightmap_index >= lightmap_count)
		return;
	struct lightmap_s *const lightmap = &lightmaps[lightmap_index];
	const qboolean transient = R_TransientEmissiveActive () && lightmap->emissive_transient_texture;
	*coarse = transient ? lightmap->emissive_transient_texture : lightmap->emissive_texture;
	*detail = transient ? lightmap->emissive_transient_detail_texture : lightmap->emissive_detail_texture;
	if (!emissive_bounce_ready || r_emissive_rt_bounce.value <= 0.0f || r_emissive_rt_bounce_strength.value <= 0.0f)
		return;
	/* Transient bounce output is current exactly when its epoch matches the current
	 * direct input: every refresh re-establishes the match, and a changed field zeroes
	 * the bounce epoch first. Epochs also match for same-frame refreshes the pre-task
	 * latch cannot observe. */
	const qboolean use_bounce = transient ? emissive_transient_bounce_epoch == emissive_transient_direct_epoch
									: emissive_bounce_cacheable_latched;
	if (!use_bounce)
		return;
	gltexture_t *const bounce_coarse = transient ? lightmap->emissive_transient_bounce_texture : lightmap->emissive_bounce_texture;
	gltexture_t *const bounce_detail = transient ? lightmap->emissive_transient_bounce_detail_texture : lightmap->emissive_bounce_detail_texture;
	if (bounce_coarse)
		*coarse = bounce_coarse;
	if (bounce_detail)
		*detail = bounce_detail;
}

void R_TransientEmissiveDetailCompleted (uint32_t generation)
{
	if (generation == transient_emissive_generation)
	{
		transient_emissive_detail_ready = true;
		if (emissive_bounce_ready)
			emissive_bounce_transient_refresh_pending = true;
	}
	else
		++transient_emissive_rejected_publications;
}

/*
==================
R_EmissiveDetailAvailable
==================
*/
qboolean R_EmissiveDetailAvailable (void)
{
	if (num_emissive_modulation_groups && !emissive_visibility_available)
		return false;
	for (int i = 0; i < lightmap_count; ++i)
		if (lightmaps[i].emissive_detail_texture)
			return true;
	return false;
}

static qboolean R_TransientEmissiveDetailAvailable (void)
{
	for (int i = 0; i < lightmap_count; ++i)
		if (lightmaps[i].emissive_transient_detail_texture)
			return true;
	return false;
}

/*
==================
R_EmissiveTileStats
==================
*/
void R_EmissiveTileStats (
	int *affected_tiles, int *total_tiles, int *source_links, int *dispatches, uint64_t *cpu_bytes, uint64_t *gpu_bytes)
{
	*affected_tiles = num_emissive_logical_tiles;
	*total_tiles = num_emissive_logical_tiles_total;
	*source_links = num_emissive_logical_tile_sources;
	*dispatches = R_EmissiveDetailDispatchCount ();
	*cpu_bytes = num_emissive_logical_tiles * sizeof (*emissive_logical_tiles) +
		num_emissive_logical_tile_sources * sizeof (*emissive_logical_tile_sources);
	*gpu_bytes = emissive_tiles_buffer_memory.size + emissive_tile_sources_buffer_memory.size;
}

void R_TransientEmissiveStats (
	int *lights, int *tiles, int *source_links, uint64_t *cpu_bytes, uint64_t *gpu_bytes, uint32_t *cpu_time_us,
	uint32_t *rejected_publications, qboolean *pending, qboolean *detail_ready)
{
	*lights = num_transient_emissive_lights;
	*tiles = num_transient_emissive_tiles;
	*source_links = num_transient_emissive_tile_sources;
	*cpu_bytes = (uint64_t)num_transient_emissive_lights * sizeof (*transient_emissive_lights) +
				 (transient_emissive_light_seeds ? (uint64_t)num_transient_emissive_lights * sizeof (*transient_emissive_light_seeds) : 0) +
				 (uint64_t)num_previous_transient_emissive_lights * sizeof (*previous_transient_emissive_lights) +
				 (uint64_t)num_transient_emissive_lights * sizeof (*transient_emissive_light_ids) +
				 (uint64_t)num_previous_transient_emissive_lights * sizeof (*previous_transient_emissive_light_ids) +
				 (uint64_t)num_transient_emissive_tiles * sizeof (*transient_emissive_tiles) +
				 (uint64_t)num_transient_emissive_tile_sources * sizeof (*transient_emissive_tile_sources) +
				 (uint64_t)(num_transient_emissive_total_tiles + 1) * sizeof (*transient_emissive_tile_surface_offsets) +
				 (uint64_t)num_transient_emissive_tile_surfaces * sizeof (*transient_emissive_tile_surfaces) +
				 (uint64_t)num_transient_emissive_total_tiles * (sizeof (*transient_emissive_tile_generations) + sizeof (*transient_emissive_tile_indices)) +
				 (uint64_t)(cl.worldmodel ? cl.worldmodel->nummodelsurfaces : 0) *
					 (sizeof (*transient_emissive_surface_influence_counts) + sizeof (*transient_emissive_surface_deltas) +
					  sizeof (*transient_emissive_surface_delta_generations) + sizeof (*transient_emissive_touched_surfaces));
	*gpu_bytes =
		transient_emissive_lights_buffer_memory.size + transient_emissive_tiles_buffer_memory.size + transient_emissive_tile_sources_buffer_memory.size;
	for (int i = 0; i < lightmap_count; ++i)
	{
		if (lightmaps[i].emissive_transient_texture)
			*gpu_bytes += GL_HeapGetAllocationSize (lightmaps[i].emissive_transient_texture->allocation);
		if (lightmaps[i].emissive_transient_detail_texture)
			*gpu_bytes += GL_HeapGetAllocationSize (lightmaps[i].emissive_transient_detail_texture->allocation);
	}
	*cpu_time_us = transient_emissive_cpu_time_us;
	*rejected_publications = transient_emissive_rejected_publications;
	*pending = transient_emissive_pending || transient_emissive_detail_pending;
	*detail_ready = transient_emissive_detail_ready;
}

static void R_InvalidateEmissiveBrushReceiverSources (void)
{
	if (++emissive_brush_receiver_source_generation == 0)
		emissive_brush_receiver_source_generation = 1;
}

static void R_InvalidateEmissiveBrushReceiverRadiance (const qboolean changed_styles[MAX_LIGHTSTYLES])
{
	for (int receiver_index = 0; receiver_index < emissive_brush_receiver_count; ++receiver_index)
	{
		emissive_brush_receiver_t *const receiver = &emissive_brush_receivers[receiver_index];
		qboolean						 changed = false;
		for (int source = 0; source < receiver->source_count; ++source)
		{
			const uint32_t encoded_index = receiver->source_indices[source];
			if (encoded_index & 0x80000000u)
				continue;
			const byte style = emissive_light_styles[encoded_index];
			if (style != 255 && changed_styles[style])
			{
				changed = true;
				break;
			}
		}
		if (!changed)
			continue;
		if (!receiver->dirty)
			receiver->radiance_only = true;
		receiver->dirty = true;
		receiver->radiance_signature = R_EmissiveBrushReceiverRadianceSignature (receiver->source_indices, receiver->source_count);
	}
}

static emissive_brush_receiver_t *R_FindEmissiveBrushReceiver (uint32_t receiver_instance_id, qmodel_t *model)
{
	for (int i = 0; i < emissive_brush_receiver_count; ++i)
		if (emissive_brush_receivers[i].receiver_instance_id == receiver_instance_id && emissive_brush_receivers[i].model == model)
			return &emissive_brush_receivers[i];
	return NULL;
}

static void R_FreeEmissiveBrushReceiverDescriptorSets (void)
{
	for (int receiver_index = 0; receiver_index < emissive_brush_receiver_count; ++receiver_index)
		for (int layer_index = 0; layer_index < emissive_brush_receivers[receiver_index].num_layers; ++layer_index)
		{
			emissive_brush_receiver_layer_t *const layer = &emissive_brush_receivers[receiver_index].layers[layer_index];
			VkDescriptorSet *const				   sets[] = {&layer->coarse_descriptor_set, &layer->detail_descriptor_set};
			for (int detail = 0; detail < countof (sets); ++detail)
				if (*sets[detail] != VK_NULL_HANDLE)
				{
					R_FreeDescriptorSet (*sets[detail], &vulkan_globals.emissive_brush_receiver_set_layout);
					*sets[detail] = VK_NULL_HANDLE;
				}
		}
}

static void R_FreeEmissiveBrushReceivers (void)
{
	for (int i = 0; i < emissive_brush_receiver_count; ++i)
	{
		emissive_brush_receiver_t *const receiver = &emissive_brush_receivers[i];
		for (int layer_index = 0; layer_index < receiver->num_layers; ++layer_index)
		{
			emissive_brush_receiver_layer_t *const layer = &receiver->layers[layer_index];
			if (layer->coarse_descriptor_set != VK_NULL_HANDLE)
				R_FreeDescriptorSet (layer->coarse_descriptor_set, &vulkan_globals.emissive_brush_receiver_set_layout);
			if (layer->detail_descriptor_set != VK_NULL_HANDLE)
				R_FreeDescriptorSet (layer->detail_descriptor_set, &vulkan_globals.emissive_brush_receiver_set_layout);
			for (int detail = 0; detail < 2; ++detail)
			{
				R_FreeBuffer (layer->visibility_buffers[detail], &layer->visibility_memories[detail], &num_vulkan_bmodel_allocations);
				layer->visibility_buffers[detail] = VK_NULL_HANDLE;
			}
		}
		R_FreeBuffer (receiver->source_indices_buffer, &receiver->source_indices_memory, &num_vulkan_bmodel_allocations);
		receiver->source_indices_buffer = VK_NULL_HANDLE;
		Mem_Free (receiver->layers);
	}
	Mem_Free (emissive_brush_receivers);
	emissive_brush_receivers = NULL;
	emissive_brush_receiver_count = emissive_brush_receiver_capacity = 0;
	Mem_Free (emissive_alias_receivers);
	emissive_alias_receivers = NULL;
	emissive_alias_receiver_count = emissive_alias_receiver_capacity = 0;
	emissive_brush_receiver_allocated_bytes = 0;
	emissive_brush_receiver_budget_limited = false;
	emissive_brush_receiver_uploaded_source_generation = 0;
	Atomic_StoreUInt32 (&emissive_brush_receiver_updates, 0);
	Atomic_StoreUInt32 (&emissive_brush_receiver_dispatches, 0);
	Atomic_StoreUInt32 (&emissive_brush_receiver_no_ray_dispatches, 0);
	Atomic_StoreUInt32 (&emissive_brush_receiver_transform_invalidations, 0);
	Atomic_StoreUInt32 (&emissive_clustered_alias_receivers, 0);
	Atomic_StoreUInt32 (&emissive_clustered_alias_builds, 0);
	Atomic_StoreUInt32 (&emissive_clustered_alias_source_evaluations, 0);
	Atomic_StoreUInt32 (&emissive_clustered_alias_shadow_tests, 0);
	Atomic_StoreUInt32 (&emissive_clustered_alias_shadow_rejections, 0);
	Atomic_StoreUInt32 (&emissive_clustered_alias_contributors, 0);
	GL_ResetEmissiveBrushReceiverTimestamp ();
	R_InvalidateEmissiveBrushReceiverSources ();
}

typedef struct emissive_brush_receiver_bounds_s
{
	int		 min_s, min_t, max_s, max_t;
	qboolean used;
} emissive_brush_receiver_bounds_t;

static int R_EmissiveBrushReceiverSurfaceBase (const qmodel_t *model)
{
	if (!model)
		return -1;
	if (model->name[0] == '*' && cl.worldmodel && model->surfaces == cl.worldmodel->surfaces)
		return model->firstmodelsurface;

	int surface_base = 0;
	for (int model_index = 1; model_index < MAX_MODELS; ++model_index)
	{
		const qmodel_t *const candidate = cl.model_precache[model_index];
		if (!candidate)
			break;
		if (candidate->name[0] == '*')
			continue;
		if (candidate == model)
			return surface_base + model->firstmodelsurface;
		surface_base += candidate->numsurfaces;
	}
	return -1;
}

static qboolean R_BuildEmissiveBrushReceiverLayers (emissive_brush_receiver_t *receiver)
{
	qmodel_t *const model = receiver->model;
	const int surface_base = R_EmissiveBrushReceiverSurfaceBase (model);
	if (!cl.worldmodel || surface_base < 0 || model->nummodelsurfaces <= 0 || surface_base + model->nummodelsurfaces > num_surfaces)
		return false;

	emissive_brush_receiver_bounds_t *const bounds = Mem_Alloc (lightmap_count * sizeof (*bounds));
	memset (bounds, 0, lightmap_count * sizeof (*bounds));
	for (int lightmap = 0; lightmap < lightmap_count; ++lightmap)
		bounds[lightmap].min_s = bounds[lightmap].min_t = INT_MAX;

	int				  num_layers = 0;
	msurface_t *const first_surface = &model->surfaces[model->firstmodelsurface];
	for (int i = 0; i < model->nummodelsurfaces; ++i)
	{
		const msurface_t *const surface = &first_surface[i];
		const int				lightmap = surface->lightmaptexturenum;
		if ((surface->flags & SURF_DRAWTILED) || lightmap < 0 || lightmap >= lightmap_count)
			continue;
		emissive_brush_receiver_bounds_t *const layer_bounds = &bounds[lightmap];
		if (!layer_bounds->used)
		{
			layer_bounds->used = true;
			++num_layers;
		}
		layer_bounds->min_s = q_min (layer_bounds->min_s, surface->light_s);
		layer_bounds->min_t = q_min (layer_bounds->min_t, surface->light_t);
		layer_bounds->max_s = q_max (layer_bounds->max_s, surface->light_s + (surface->extents[0] >> 4) + 1);
		layer_bounds->max_t = q_max (layer_bounds->max_t, surface->light_t + (surface->extents[1] >> 4) + 1);
	}
	if (!num_layers)
	{
		Mem_Free (bounds);
		return false;
	}

	const VkBufferUsageFlags visibility_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	uint64_t				 coarse_required = R_EmissiveBufferMemorySize (sizeof (receiver->source_indices), visibility_usage), detail_required = 0;
	for (int lightmap = 0; lightmap < lightmap_count; ++lightmap)
		if (bounds[lightmap].used)
		{
			bounds[lightmap].min_s &= ~7;
			bounds[lightmap].min_t &= ~7;
			bounds[lightmap].max_s = (bounds[lightmap].max_s + 7) & ~7;
			bounds[lightmap].max_t = (bounds[lightmap].max_t + 7) & ~7;
			const int width = bounds[lightmap].max_s - bounds[lightmap].min_s;
			const int height = bounds[lightmap].max_t - bounds[lightmap].min_t;
			coarse_required +=
				TexMgr_RGBA16FImageMemorySize (width, height) + R_EmissiveBufferMemorySize ((uint64_t)width * height * sizeof (uint32_t), visibility_usage);
			detail_required +=
				TexMgr_RGBA16FImageMemorySize (width * R_EmissiveDetailScale (), height * R_EmissiveDetailScale ()) +
				R_EmissiveBufferMemorySize (
					(uint64_t)width * R_EmissiveDetailScale () * height * R_EmissiveDetailScale () * sizeof (uint32_t), visibility_usage);
		}
	const uint64_t budget = (uint64_t)EMISSIVE_BRUSH_RECEIVER_MEMORY_BUDGET_MB * 1024 * 1024;
	if (emissive_brush_receiver_allocated_bytes + coarse_required > budget)
	{
		emissive_brush_receiver_budget_limited = true;
		Mem_Free (bounds);
		return false;
	}
	const qboolean allocate_detail = emissive_brush_receiver_allocated_bytes + coarse_required + detail_required <= budget;
	if (!allocate_detail)
		emissive_brush_receiver_budget_limited = true;

	receiver->layers = Mem_Alloc (num_layers * sizeof (*receiver->layers));
	memset (receiver->layers, 0, num_layers * sizeof (*receiver->layers));
	receiver->num_layers = num_layers;
	buffer_create_info_t source_indices_info = {
		&receiver->source_indices_buffer,		 sizeof (receiver->source_indices), 0, visibility_usage, (void **)&receiver->source_indices_mapped, NULL,
		"Emissive brush receiver source indices"};
	R_CreateBuffers (
		1, &source_indices_info, &receiver->source_indices_memory, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0,
		&num_vulkan_bmodel_allocations, "Emissive brush receiver source indices");
	memcpy (receiver->source_indices_mapped, receiver->source_indices, sizeof (receiver->source_indices));
	receiver->allocated_bytes += receiver->source_indices_memory.size;
	int layer_index = 0;
	for (int lightmap = 0; lightmap < lightmap_count; ++lightmap)
	{
		const emissive_brush_receiver_bounds_t *const layer_bounds = &bounds[lightmap];
		if (!layer_bounds->used)
			continue;
		emissive_brush_receiver_layer_t *const layer = &receiver->layers[layer_index++];
		layer->lightmap = lightmap;
		layer->atlas_offset[0] = layer_bounds->min_s;
		layer->atlas_offset[1] = layer_bounds->min_t;
		layer->width = layer_bounds->max_s - layer_bounds->min_s;
		layer->height = layer_bounds->max_t - layer_bounds->min_t;
		uint32_t *const indices = Mem_Alloc ((size_t)layer->width * layer->height * sizeof (*indices));
		memset (indices, 0xFF, (size_t)layer->width * layer->height * sizeof (*indices));
		for (int surface_index = 0; surface_index < model->nummodelsurfaces; ++surface_index)
		{
			const msurface_t *const surface = &first_surface[surface_index];
			if ((surface->flags & SURF_DRAWTILED) || surface->lightmaptexturenum != lightmap)
				continue;
			const int gpu_surface_index = surface_base + surface_index;
			const int width = (surface->extents[0] >> 4) + 1;
			const int height = (surface->extents[1] >> 4) + 1;
			for (int t = 0; t < height; ++t)
				for (int s = 0; s < width; ++s)
					indices[(surface->light_t - layer_bounds->min_t + t) * layer->width + surface->light_s - layer_bounds->min_s + s] =
						(uint32_t)gpu_surface_index;
		}
		char name[64];
		const uint32_t model_hash = COM_HashBlock (model->name, strlen (model->name));
		q_snprintf (name, sizeof (name), "emissive_receiver_indices_%08x_%08x_%03i", receiver->receiver_instance_id, model_hash, lightmap);
		layer->surface_indices_texture = TexMgr_LoadImage (
			model, name, layer->width, layer->height, SRC_SURF_INDICES, (byte *)indices, "", (src_offset_t)indices, TEXPREF_NEAREST | TEXPREF_NOPICMIP);
		Mem_Free (indices);
		q_snprintf (name, sizeof (name), "emissive_receiver_coarse_%08x_%08x_%03i", receiver->receiver_instance_id, model_hash, lightmap);
		layer->coarse_texture =
			TexMgr_LoadImage (model, name, layer->width, layer->height, SRC_RGBA16F, NULL, "", 0, TEXPREF_LINEAR | TEXPREF_NOPICMIP);
		q_snprintf (name, sizeof (name), "emissive_receiver_visibility_%08x_%08x_%03i", receiver->receiver_instance_id, model_hash, lightmap);
		R_CreateBuffer (
			&layer->visibility_buffers[0], &layer->visibility_memories[0], (uint64_t)layer->width * layer->height * sizeof (uint32_t), visibility_usage,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL, name);
		if (allocate_detail)
		{
			q_snprintf (name, sizeof (name), "emissive_receiver_detail_%08x_%08x_%03i", receiver->receiver_instance_id, model_hash, lightmap);
			layer->detail_texture = TexMgr_LoadImage (
				model, name, layer->width * R_EmissiveDetailScale (), layer->height * R_EmissiveDetailScale (), SRC_RGBA16F, NULL, "", 0,
				TEXPREF_LINEAR | TEXPREF_NOPICMIP);
			q_snprintf (
				name, sizeof (name), "emissive_receiver_detail_visibility_%08x_%08x_%03i", receiver->receiver_instance_id, model_hash, lightmap);
			R_CreateBuffer (
				&layer->visibility_buffers[1], &layer->visibility_memories[1],
				(uint64_t)layer->width * R_EmissiveDetailScale () * layer->height * R_EmissiveDetailScale () * sizeof (uint32_t), visibility_usage,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL, name);
		}
		receiver->allocated_bytes += GL_HeapGetAllocationSize (layer->coarse_texture->allocation);
		receiver->allocated_bytes += layer->visibility_memories[0].size;
		if (layer->detail_texture)
		{
			receiver->allocated_bytes += GL_HeapGetAllocationSize (layer->detail_texture->allocation);
			receiver->allocated_bytes += layer->visibility_memories[1].size;
		}
	}
	emissive_brush_receiver_allocated_bytes += receiver->allocated_bytes;
	Mem_Free (bounds);
	return true;
}

static qboolean R_EmissiveBrushReceiverSourceIntersectsBounds (const emissive_light_t *source, const vec3_t mins, const vec3_t maxs)
{
	float distance_squared = 0.0f;
	for (int axis = 0; axis < 3; ++axis)
	{
		const float distance = source->origin[axis] < mins[axis]   ? mins[axis] - source->origin[axis]
							   : source->origin[axis] > maxs[axis] ? source->origin[axis] - maxs[axis]
																   : 0.0f;
		distance_squared += distance * distance;
	}
	return distance_squared < source->radius * source->radius;
}

static uint32_t R_EmissiveBrushReceiverRadianceSignature (const uint32_t *source_indices, int source_count)
{
	uint32_t signature = 2166136261u;
	for (int i = 0; i < source_count; ++i)
	{
		const uint32_t encoded_index = source_indices[i];
		if (encoded_index & 0x80000000u)
			continue;
		const float	   modulation = emissive_light_modulations ? emissive_light_modulations[encoded_index] : 1.0f;
		const uint32_t modulation_hash = COM_HashBlock (&modulation, sizeof (modulation));
		signature ^= modulation_hash + encoded_index * 0x27d4eb2du + (signature << 6) + (signature >> 2);
	}
	return signature;
}

static uint32_t R_EmissiveBrushReceiverSourceSignature (
	const qmodel_t *model, const vec4_t transform[3], vec3_t world_mins, vec3_t world_maxs, uint32_t source_indices[EMISSIVE_BRUSH_RECEIVER_MAX_SOURCES],
	int *source_count, uint32_t *radiance_signature)
{
	for (int axis = 0; axis < 3; ++axis)
	{
		world_mins[axis] = FLT_MAX;
		world_maxs[axis] = -FLT_MAX;
	}
	for (int corner = 0; corner < 8; ++corner)
	{
		const vec4_t model_corner = {
			(corner & 1) ? model->maxs[0] : model->mins[0], (corner & 2) ? model->maxs[1] : model->mins[1], (corner & 4) ? model->maxs[2] : model->mins[2],
			1.0f};
		vec3_t world_corner;
		for (int row = 0; row < 3; ++row)
			world_corner[row] =
				transform[row][0] * model_corner[0] + transform[row][1] * model_corner[1] + transform[row][2] * model_corner[2] + transform[row][3];
		for (int axis = 0; axis < 3; ++axis)
		{
			world_mins[axis] = q_min (world_mins[axis], world_corner[axis]);
			world_maxs[axis] = q_max (world_maxs[axis], world_corner[axis]);
		}
	}

	uint32_t signature = 2166136261u;
	memset (source_indices, 0, EMISSIVE_BRUSH_RECEIVER_MAX_SOURCES * sizeof (*source_indices));
	*source_count = 0;
	const int total_lights = num_emissive_lights + num_transient_emissive_lights;
	for (int light_index = 0; light_index < total_lights; ++light_index)
	{
		const qboolean				  transient = light_index >= num_emissive_lights;
		const int					  source_index = transient ? light_index - num_emissive_lights : light_index;
		const emissive_light_t *const source = transient ? &transient_emissive_lights[source_index] : &emissive_cacheable_lights[source_index];
		if (!R_EmissiveBrushReceiverSourceIntersectsBounds (source, world_mins, world_maxs))
			continue;
		const uint32_t encoded_index = (transient ? 0x80000000u : 0u) | (uint32_t)source_index;
		const uint32_t source_hash = COM_HashBlock (source, sizeof (*source));
		signature ^= source_hash + 0x9e3779b9u + (signature << 6) + (signature >> 2);
		signature ^= encoded_index * 0x85ebca6bu;
		if (*source_count < EMISSIVE_BRUSH_RECEIVER_MAX_SOURCES)
			source_indices[*source_count] = encoded_index;
		++*source_count;
	}
	*radiance_signature = R_EmissiveBrushReceiverRadianceSignature (source_indices, q_min (*source_count, EMISSIVE_BRUSH_RECEIVER_MAX_SOURCES));
	return signature;
}

static void R_UpdateEmissiveBrushReceiverEntity (entity_t *entity, uint32_t receiver_instance_id)
{
	qmodel_t *const model = entity->model;
	if (!model || model->needload || model->type != mod_brush)
		return;
	const float alpha = ENTALPHA_DECODE (entity->alpha);
	if (alpha <= 0.0f || (alpha < 1.0f && r_emissive_rt_translucent_receivers.value <= 0.0f))
		return;
	const qboolean inline_bsp = model->name[0] == '*' && model->surfaces == cl.worldmodel->surfaces;
	if (!inline_bsp && r_emissive_rt_external_bsp.value <= 0.0f)
		return;

	vec3_t entity_angles;
	VectorCopy (entity->angles, entity_angles);
	entity_angles[0] = -entity_angles[0];
	float model_matrix[16];
	IdentityMatrix (model_matrix);
	R_RotateForEntity (model_matrix, entity->origin, entity_angles, entity->netstate.scale);
	vec4_t transform[3];
	for (int row = 0; row < 3; ++row)
		for (int col = 0; col < 4; ++col)
			transform[row][col] = model_matrix[col * 4 + row];
	vec3_t		   world_mins, world_maxs;
	uint32_t	   source_indices[EMISSIVE_BRUSH_RECEIVER_MAX_SOURCES], radiance_signature;
	int			   source_count;
	const uint32_t transport_signature =
		R_EmissiveBrushReceiverSourceSignature (model, transform, world_mins, world_maxs, source_indices, &source_count, &radiance_signature);
	if (!source_count || source_count > EMISSIVE_BRUSH_RECEIVER_MAX_SOURCES)
		return;

	emissive_brush_receiver_t *receiver = R_FindEmissiveBrushReceiver (receiver_instance_id, model);
	if (!receiver)
	{
		if (emissive_brush_receiver_count == emissive_brush_receiver_capacity)
		{
			emissive_brush_receiver_capacity = q_max (16, emissive_brush_receiver_capacity * 2);
			emissive_brush_receivers = Mem_Realloc (emissive_brush_receivers, emissive_brush_receiver_capacity * sizeof (*emissive_brush_receivers));
		}
		receiver = &emissive_brush_receivers[emissive_brush_receiver_count++];
		memset (receiver, 0, sizeof (*receiver));
		receiver->receiver_instance_id = receiver_instance_id;
		receiver->model = model;
		receiver->source_count = source_count;
		memcpy (receiver->source_indices, source_indices, sizeof (source_indices));
		receiver->transport_signature = transport_signature;
		receiver->radiance_signature = radiance_signature;
		if (!R_BuildEmissiveBrushReceiverLayers (receiver))
			return;
		memcpy (receiver->transform, transform, sizeof (transform));
		receiver->dirty = true;
		receiver->radiance_only = false;
	}
	// A retained record with no layers is an explicit admission failure. Keep the
	// entity on the ordinary per-entity lightmap path instead of suppressing that
	// fallback and publishing an empty receiver.
	if (!receiver->num_layers)
		return;

	const qboolean transform_changed = memcmp (receiver->transform, transform, sizeof (transform)) != 0;
	const qboolean transport_changed = receiver->transport_signature != transport_signature;
	if (transform_changed || transport_changed)
	{
		if (transform_changed && receiver->ready)
			Atomic_IncrementUInt32 (&emissive_brush_receiver_transform_invalidations);
		memcpy (receiver->transform, transform, sizeof (transform));
		receiver->transport_signature = transport_signature;
		receiver->source_count = source_count;
		memcpy (receiver->source_indices, source_indices, sizeof (source_indices));
		memcpy (receiver->source_indices_mapped, source_indices, sizeof (source_indices));
		receiver->dirty = true;
		receiver->radiance_only = false;
	}
	else if (receiver->radiance_signature != radiance_signature)
	{
		if (!receiver->dirty)
			receiver->radiance_only = true;
		receiver->dirty = true;
	}
	receiver->radiance_signature = radiance_signature;
	receiver->entity = entity;
	receiver->active = true;
}

static emissive_alias_receiver_t *R_FindEmissiveAliasReceiver (entity_t *entity, qmodel_t *model)
{
	for (int i = 0; i < emissive_alias_receiver_count; ++i)
		if (emissive_alias_receivers[i].entity == entity && emissive_alias_receivers[i].model == model)
			return &emissive_alias_receivers[i];
	return NULL;
}

static void R_EmissiveAliasReceiverFrame (const entity_t *entity, vec3_t center, vec3_t forward, vec3_t right, vec3_t up)
{
	vec3_t local_center, entity_angles;
	VectorAdd (entity->model->mins, entity->model->maxs, local_center);
	VectorScale (local_center, 0.5f * ENTSCALE_DECODE (entity->netstate.scale), local_center);
	VectorCopy (entity->angles, entity_angles);
	AngleVectors (entity_angles, forward, right, up);
	VectorCopy (entity->origin, center);
	VectorMA (center, local_center[0], forward, center);
	VectorMA (center, -local_center[1], right, center);
	VectorMA (center, local_center[2], up, center);
}

static int R_EmissiveAliasLightLimit (void)
{
	return CLAMP (0, (int)r_emissive_rt_model_lights.value, EMISSIVE_CLUSTERED_LIGHTS);
}

static void R_EmissiveOccluderStateBounds (const emissive_occluder_state_t *state, vec3_t mins, vec3_t maxs)
{
	vec3_t origin, angles;
	VectorCopy (state->origin, origin);
	VectorCopy (state->angles, angles);
	angles[0] = -angles[0];
	float model_matrix[16];
	IdentityMatrix (model_matrix);
	R_RotateForEntity (model_matrix, origin, angles, state->scale);
	for (int axis = 0; axis < 3; ++axis)
	{
		mins[axis] = FLT_MAX;
		maxs[axis] = -FLT_MAX;
	}
	for (int corner = 0; corner < 8; ++corner)
	{
		const vec4_t local = {
			(corner & 1) ? state->model->maxs[0] : state->model->mins[0],
			(corner & 2) ? state->model->maxs[1] : state->model->mins[1],
			(corner & 4) ? state->model->maxs[2] : state->model->mins[2], 1.0f};
		vec3_t world;
		for (int row = 0; row < 3; ++row)
			world[row] = model_matrix[row] * local[0] + model_matrix[4 + row] * local[1] + model_matrix[8 + row] * local[2] + model_matrix[12 + row];
		for (int axis = 0; axis < 3; ++axis)
		{
			mins[axis] = q_min (mins[axis], world[axis]);
			maxs[axis] = q_max (maxs[axis], world[axis]);
		}
	}
}

static void R_MarkEmissiveOccluderStateTiles (const emissive_occluder_state_t *state)
{
	if (!emissive_occluder_dirty_tile_bits || !emissive_logical_tile_mins || !emissive_logical_tile_maxs)
		return;
	vec3_t occluder_mins, occluder_maxs;
	R_EmissiveOccluderStateBounds (state, occluder_mins, occluder_maxs);
	for (int axis = 0; axis < 3; ++axis)
	{
		occluder_mins[axis] -= EMISSIVE_OCCLUDER_BOUNDS_PADDING;
		occluder_maxs[axis] += EMISSIVE_OCCLUDER_BOUNDS_PADDING;
	}
	for (int tile_index = 0; tile_index < num_emissive_logical_tiles; ++tile_index)
	{
		if (emissive_occluder_dirty_tile_bits[tile_index])
			continue;
		const emissive_logical_tile_t *const tile = &emissive_logical_tiles[tile_index];
		for (uint32_t source_link = tile->first_source; source_link < tile->first_source + tile->num_sources; ++source_link)
		{
			const emissive_light_t *const source = &emissive_cacheable_lights[emissive_logical_tile_sources[source_link]];
			qboolean intersects = true;
			for (int axis = 0; axis < 3; ++axis)
			{
				const float ray_min = q_min (source->origin[axis], emissive_logical_tile_mins[tile_index][axis]);
				const float ray_max = q_max (source->origin[axis], emissive_logical_tile_maxs[tile_index][axis]);
				if (ray_max < occluder_mins[axis] || ray_min > occluder_maxs[axis])
				{
					intersects = false;
					break;
				}
			}
			if (intersects)
			{
				emissive_occluder_dirty_tile_bits[tile_index] = true;
				break;
			}
		}
	}
}

static void R_BuildEmissiveOccluderDirtyTiles (
	const emissive_occluder_state_t *previous_states, int previous_count, const emissive_occluder_state_t *current_states, int current_count)
{
	emissive_occluder_full_detail_refresh = !emissive_occluder_state_valid || !emissive_detail_ready || !emissive_occluder_dirty_tile_bits;
	emissive_occluder_partial_detail_refresh = false;
	if (emissive_occluder_full_detail_refresh)
	{
		emissive_occluder_dirty_tiles_valid = false;
		return;
	}
	qboolean same_bounds = previous_count == current_count;
	for (int i = 0; same_bounds && i < current_count; ++i)
		same_bounds = previous_states[i].entity == current_states[i].entity && previous_states[i].model == current_states[i].model &&
			previous_states[i].scale == current_states[i].scale && !memcmp (previous_states[i].origin, current_states[i].origin, sizeof (current_states[i].origin)) &&
			!memcmp (previous_states[i].angles, current_states[i].angles, sizeof (current_states[i].angles));
	if (same_bounds && emissive_occluder_dirty_tiles_valid)
	{
		emissive_occluder_partial_detail_refresh = num_emissive_occluder_dirty_tiles > 0;
		return;
	}

	num_emissive_occluder_dirty_tiles = 0;
	memset (emissive_occluder_dirty_tile_bits, 0, num_emissive_logical_tiles * sizeof (*emissive_occluder_dirty_tile_bits));
	for (int current = 0; current < current_count; ++current)
	{
		int previous = 0;
		while (previous < previous_count && (previous_states[previous].entity != current_states[current].entity ||
			previous_states[previous].model != current_states[current].model))
			++previous;
		if (previous < previous_count && !memcmp (&previous_states[previous], &current_states[current], sizeof (current_states[current])))
			continue;
		if (previous < previous_count)
			R_MarkEmissiveOccluderStateTiles (&previous_states[previous]);
		R_MarkEmissiveOccluderStateTiles (&current_states[current]);
	}
	for (int previous = 0; previous < previous_count; ++previous)
	{
		int current = 0;
		while (current < current_count && (current_states[current].entity != previous_states[previous].entity ||
			current_states[current].model != previous_states[previous].model))
			++current;
		if (current == current_count)
			R_MarkEmissiveOccluderStateTiles (&previous_states[previous]);
	}
	for (int tile = 0; tile < num_emissive_logical_tiles; ++tile)
		if (emissive_occluder_dirty_tile_bits[tile])
			emissive_occluder_tiles[num_emissive_occluder_dirty_tiles++] = emissive_logical_tiles[tile];
	emissive_occluder_full_detail_refresh = num_emissive_occluder_dirty_tiles == num_emissive_logical_tiles;
	emissive_occluder_partial_detail_refresh = num_emissive_occluder_dirty_tiles > 0 && !emissive_occluder_full_detail_refresh;
	emissive_occluder_dirty_tiles_valid = !emissive_occluder_full_detail_refresh;
}

static qboolean R_UpdateEmissiveOccluderState (void)
{
	const int occluder_tier = CLAMP (0, (int)r_emissive_rt_occluders.value, 2);
	if (occluder_tier <= 0 || !cl.worldmodel)
	{
		emissive_occluder_state_count = 0;
		emissive_occluder_state_valid = false;
		emissive_occluder_worldmodel = cl.worldmodel;
		return false;
	}
	const double update_time = Sys_DoubleTime ();
	if (emissive_occluder_state_valid && update_time < emissive_occluder_next_update_time)
		return false;
	emissive_occluder_next_update_time = update_time + EMISSIVE_OCCLUDER_UPDATE_INTERVAL;

	if (emissive_occluder_worldmodel != cl.worldmodel)
	{
		emissive_occluder_state_count = 0;
		emissive_occluder_state_valid = false;
		emissive_occluder_worldmodel = cl.worldmodel;
	}

	const int max_states = cl.num_entities + cl.num_statics;
	if (max_states > emissive_occluder_state_capacity)
	{
		emissive_occluder_state_capacity = max_states;
		emissive_occluder_states = Mem_Realloc (
			emissive_occluder_states, emissive_occluder_state_capacity * sizeof (*emissive_occluder_states));
		emissive_occluder_state_scratch = Mem_Realloc (
			emissive_occluder_state_scratch, emissive_occluder_state_capacity * sizeof (*emissive_occluder_state_scratch));
	}

	int state_count = 0;
	for (int i = 0; i < max_states; ++i)
	{
		entity_t *const entity = i < cl.num_entities ? &cl.entities[i] : cl.static_entities[i - cl.num_entities];
		qmodel_t *const model = entity->model;
		if (!model || model->needload || ((entity->alpha != ENTALPHA_DEFAULT) && (ENTALPHA_DECODE (entity->alpha) < 1.0f)))
			continue;
		const qboolean brush = model->type == mod_brush && model->blas != VK_NULL_HANDLE;
		const qboolean alias = occluder_tier > 1 && model->type == mod_alias && entity->blas_data &&
			entity->blas_data->blas != VK_NULL_HANDLE && !entity->blas_data->needs_initial_build && entity->blas_data->model == model &&
			!R_EmissiveAliasEntityIsSource (entity);
		if (!brush && !alias)
			continue;

		emissive_occluder_state_t *const state = &emissive_occluder_state_scratch[state_count++];
		memset (state, 0, sizeof (*state));
		state->entity = entity;
		state->model = model;
		state->scale = entity->netstate.scale;
		for (int axis = 0; axis < 3; ++axis)
		{
			state->origin[axis] = floorf (entity->origin[axis] / EMISSIVE_OCCLUDER_POSITION_QUANTUM + 0.5f) * EMISSIVE_OCCLUDER_POSITION_QUANTUM;
			state->angles[axis] = floorf (entity->angles[axis] * (EMISSIVE_OCCLUDER_ANGLE_STEPS / 360.0f) + 0.5f) *
				(360.0f / EMISSIVE_OCCLUDER_ANGLE_STEPS);
		}
		if (alias)
		{
			aliashdr_t *const header = (aliashdr_t *)Mod_Extradata (model);
			if (header)
			{
				lerpdata_t lerp;
				R_SetupAliasFrame (entity, header, &lerp);
				state->pose1 = lerp.pose1;
				state->pose2 = lerp.pose2;
			}
		}
	}

	const qboolean changed = !emissive_occluder_state_valid || state_count != emissive_occluder_state_count ||
		(state_count && memcmp (emissive_occluder_state_scratch, emissive_occluder_states, state_count * sizeof (*emissive_occluder_states)));
	if (!changed)
		return false;

	qboolean brush_changed = !emissive_occluder_state_valid;
	for (int current = 0; !brush_changed && current < state_count; ++current)
		if (emissive_occluder_state_scratch[current].model->type == mod_brush)
		{
			int previous = 0;
			while (previous < emissive_occluder_state_count &&
				(emissive_occluder_states[previous].entity != emissive_occluder_state_scratch[current].entity ||
					emissive_occluder_states[previous].model != emissive_occluder_state_scratch[current].model))
				++previous;
			brush_changed = previous == emissive_occluder_state_count ||
				memcmp (&emissive_occluder_states[previous], &emissive_occluder_state_scratch[current], sizeof (emissive_occluder_states[previous]));
		}
	for (int previous = 0; !brush_changed && previous < emissive_occluder_state_count; ++previous)
		if (emissive_occluder_states[previous].model->type == mod_brush)
		{
			int current = 0;
			while (current < state_count &&
				(emissive_occluder_state_scratch[current].entity != emissive_occluder_states[previous].entity ||
					emissive_occluder_state_scratch[current].model != emissive_occluder_states[previous].model))
				++current;
			brush_changed = current == state_count;
		}
	if (brush_changed && ++emissive_brush_occluder_generation == 0)
		++emissive_brush_occluder_generation;
	R_BuildEmissiveOccluderDirtyTiles (
		emissive_occluder_states, emissive_occluder_state_count, emissive_occluder_state_scratch, state_count);
	emissive_occluder_state_t *const previous = emissive_occluder_states;
	emissive_occluder_states = emissive_occluder_state_scratch;
	emissive_occluder_state_scratch = previous;
	emissive_occluder_state_count = state_count;
	emissive_occluder_state_valid = true;
	emissive_live_as_dirty = true;
	return true;
}

static void R_UpdateEmissiveAliasReceiverEntity (entity_t *entity)
{
	qmodel_t *const model = entity->model;
	if (!model || model->needload || model->type != mod_alias)
		return;
	emissive_alias_receiver_t *receiver = R_FindEmissiveAliasReceiver (entity, model);
	if (!receiver)
	{
		if (emissive_alias_receiver_count == emissive_alias_receiver_capacity)
		{
			emissive_alias_receiver_capacity = q_max (64, emissive_alias_receiver_capacity * 2);
			emissive_alias_receivers = Mem_Realloc (emissive_alias_receivers, emissive_alias_receiver_capacity * sizeof (*emissive_alias_receivers));
		}
		receiver = &emissive_alias_receivers[emissive_alias_receiver_count++];
		memset (receiver, 0, sizeof (*receiver));
		receiver->entity = entity;
		receiver->model = model;
	}
	receiver->active = true;
	if (receiver->ready && receiver->source_generation == emissive_brush_receiver_source_generation &&
		receiver->occluder_generation == emissive_brush_occluder_generation &&
		!memcmp (receiver->origin, entity->origin, sizeof (receiver->origin)) && !memcmp (receiver->angles, entity->angles, sizeof (receiver->angles)) &&
		receiver->scale == entity->netstate.scale)
		return;

	VectorCopy (entity->origin, receiver->origin);
	VectorCopy (entity->angles, receiver->angles);
	receiver->scale = entity->netstate.scale;
	receiver->source_generation = emissive_brush_receiver_source_generation;
	receiver->occluder_generation = emissive_brush_occluder_generation;
	memset (receiver->candidates, 0, sizeof (receiver->candidates));
	vec3_t center, forward, right, up;
	R_EmissiveAliasReceiverFrame (entity, center, forward, right, up);
	const int total_lights = num_emissive_lights + num_transient_emissive_lights;
	for (int light_index = 0; light_index < total_lights; ++light_index)
	{
		Atomic_IncrementUInt32 (&emissive_clustered_alias_source_evaluations);
		const qboolean				  transient = light_index >= num_emissive_lights;
		const int					  source_index = transient ? light_index - num_emissive_lights : light_index;
		const emissive_light_t *const source = transient ? &transient_emissive_lights[source_index] : &emissive_cacheable_lights[source_index];
		vec3_t						  light_vector;
		VectorSubtract (source->origin, center, light_vector);
		const float distance = VectorLength (light_vector);
		if (distance <= 0.001f || distance >= source->radius)
			continue;
		const float falloff = 1.0f - distance / source->radius;
		const float luminance = 0.2126f * source->color[0] + 0.7152f * source->color[1] + 0.0722f * source->color[2];
		const float score = source->intensity * falloff * q_max (luminance, 0.0f);
		if (score <= receiver->candidates[EMISSIVE_CLUSTERED_CANDIDATES - 1].base_score)
			continue;
		int destination = EMISSIVE_CLUSTERED_CANDIDATES - 1;
		while (destination > 0 && score > receiver->candidates[destination - 1].base_score)
		{
			receiver->candidates[destination] = receiver->candidates[destination - 1];
			--destination;
		}
		emissive_clustered_candidate_t *const candidate = &receiver->candidates[destination];
		memset (candidate, 0, sizeof (*candidate));
		candidate->source_index = (transient ? 0x80000000u : 0u) | (uint32_t)source_index;
		candidate->base_score = score;
		VectorScale (light_vector, 1.0f / distance, light_vector);
		candidate->direction[0] = DotProduct (light_vector, forward);
		candidate->direction[1] = -DotProduct (light_vector, right);
		candidate->direction[2] = DotProduct (light_vector, up);
		candidate->direction[3] = 1.0f;
		VectorScale (source->color, source->intensity * falloff, candidate->color);
		candidate->color[3] = 1.0f;
	}
	for (int candidate_index = 0; candidate_index < EMISSIVE_CLUSTERED_CANDIDATES; ++candidate_index)
	{
		emissive_clustered_candidate_t *const candidate = &receiver->candidates[candidate_index];
		if (candidate->base_score <= 0.0f)
			break;
		const qboolean				  transient = (candidate->source_index & 0x80000000u) != 0u;
		const uint32_t				  source_index = candidate->source_index & 0x7FFFFFFFu;
		const emissive_light_t *const source = transient ? &transient_emissive_lights[source_index] : &emissive_cacheable_lights[source_index];
		vec3_t						  light_origin;
		VectorCopy (source->origin, light_origin);
		Atomic_IncrementUInt32 (&emissive_clustered_alias_shadow_tests);
		if (CLAMP (0, (int)r_emissive_rt_occluders.value, 2) > 0)
		{
			vec3_t impact, normal;
			candidate->visible = CL_TraceLine (center, light_origin, impact, normal, NULL) >= 0.999f;
		}
		else
		{
			trace_t trace;
			memset (&trace, 0, sizeof (trace));
			trace.fraction = 1.0f;
			SV_RecursiveHullCheck (cl.worldmodel->hulls, center, light_origin, &trace, CONTENTMASK_ANYSOLID);
			candidate->visible = !trace.allsolid && trace.fraction >= 0.999f;
		}
		if (!candidate->visible)
			Atomic_IncrementUInt32 (&emissive_clustered_alias_shadow_rejections);
	}
	receiver->ready = true;
	Atomic_IncrementUInt32 (&emissive_clustered_alias_builds);
}

void R_UpdateEmissiveBrushReceivers (void)
{
	for (int i = 0; i < emissive_brush_receiver_count; ++i)
		emissive_brush_receivers[i].active = false;
	for (int i = 0; i < emissive_alias_receiver_count; ++i)
		emissive_alias_receivers[i].active = false;
	if (!cl.worldmodel || !vulkan_globals.ray_query || r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f ||
		(!num_emissive_lights && !num_transient_emissive_lights))
		return;
	if (emissive_occluder_receiver_refresh_pending)
	{
		emissive_occluder_receiver_refresh_pending = false;
		for (int i = 0; i < emissive_brush_receiver_count; ++i)
		{
			emissive_brush_receivers[i].dirty = true;
			emissive_brush_receivers[i].radiance_only = false;
		}
	}
	const qboolean alias_receivers_enabled = R_EmissiveAliasLightLimit () > 0;
	for (int i = 1; i < cl.num_entities; ++i)
	{
		R_UpdateEmissiveBrushReceiverEntity (&cl.entities[i], (uint32_t)i);
		if (alias_receivers_enabled)
			R_UpdateEmissiveAliasReceiverEntity (&cl.entities[i]);
	}
	for (int i = 0; i < cl.num_statics; ++i)
	{
		R_UpdateEmissiveBrushReceiverEntity (cl.static_entities[i], 0x80000000u | (uint32_t)i);
		if (alias_receivers_enabled)
			R_UpdateEmissiveAliasReceiverEntity (cl.static_entities[i]);
	}
}

qboolean R_EmissiveBrushReceiverActive (entity_t *entity)
{
	for (int i = 0; i < emissive_brush_receiver_count; ++i)
		if (emissive_brush_receivers[i].active && emissive_brush_receivers[i].entity == entity)
			return true;
	return false;
}

qboolean R_EmissiveBrushReceiverTextures (
	entity_t *entity, int lightmap, gltexture_t **coarse, gltexture_t **detail, gltexture_t **surface_indices, uint32_t atlas_offset[2])
{
	*coarse = *detail = NULL;
	*surface_indices = NULL;
	atlas_offset[0] = atlas_offset[1] = 0;
	for (int i = 0; i < emissive_brush_receiver_count; ++i)
	{
		const emissive_brush_receiver_t *const receiver = &emissive_brush_receivers[i];
		if (!receiver->active || !receiver->ready || receiver->entity != entity)
			continue;
		for (int layer_index = 0; layer_index < receiver->num_layers; ++layer_index)
		{
			const emissive_brush_receiver_layer_t *const layer = &receiver->layers[layer_index];
			if (layer->lightmap != lightmap)
				continue;
			*coarse = layer->coarse_texture;
			*detail = layer->detail_texture;
			*surface_indices = layer->surface_indices_texture;
			atlas_offset[0] = layer->atlas_offset[0];
			atlas_offset[1] = layer->atlas_offset[1];
			return *coarse != NULL;
		}
	}
	return false;
}

void R_EmissiveBrushReceiverStats (
	int *records, int *active, int *ready, int *dirty, int *layers, uint64_t *allocated_bytes, uint64_t *budget_bytes, qboolean *budget_limited,
	uint32_t *updates, uint32_t *dispatches, uint32_t *no_ray_dispatches, uint32_t *transform_invalidations)
{
	*records = emissive_brush_receiver_count;
	*active = *ready = *dirty = *layers = 0;
	for (int i = 0; i < emissive_brush_receiver_count; ++i)
	{
		const emissive_brush_receiver_t *const receiver = &emissive_brush_receivers[i];
		*active += receiver->active;
		*ready += receiver->active && receiver->ready;
		*dirty += receiver->active && receiver->dirty;
		*layers += receiver->num_layers;
	}
	*allocated_bytes = emissive_brush_receiver_allocated_bytes;
	*budget_bytes = (uint64_t)EMISSIVE_BRUSH_RECEIVER_MEMORY_BUDGET_MB * 1024 * 1024;
	*budget_limited = emissive_brush_receiver_budget_limited;
	*updates = Atomic_LoadUInt32 (&emissive_brush_receiver_updates);
	*dispatches = Atomic_LoadUInt32 (&emissive_brush_receiver_dispatches);
	*no_ray_dispatches = Atomic_LoadUInt32 (&emissive_brush_receiver_no_ray_dispatches);
	*transform_invalidations = Atomic_LoadUInt32 (&emissive_brush_receiver_transform_invalidations);
}

int R_EmissiveClusteredAliasLights (const entity_t *entity, emissive_clustered_light_t lights[EMISSIVE_CLUSTERED_LIGHTS])
{
	const int light_limit = R_EmissiveAliasLightLimit ();
	if (!entity || !entity->model || !cl.worldmodel || r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f || r_fullbright_cheatsafe ||
		r_lightmap_cheatsafe || light_limit == 0)
		return 0;
	Atomic_IncrementUInt32 (&emissive_clustered_alias_receivers);
	const emissive_alias_receiver_t *receiver = NULL;
	for (int i = 0; i < emissive_alias_receiver_count; ++i)
		if (emissive_alias_receivers[i].active && emissive_alias_receivers[i].ready && emissive_alias_receivers[i].entity == entity)
		{
			receiver = &emissive_alias_receivers[i];
			break;
		}
	if (!receiver)
		return 0;
	int	  selected[EMISSIVE_CLUSTERED_LIGHTS];
	float selected_scores[EMISSIVE_CLUSTERED_LIGHTS];
	for (int i = 0; i < EMISSIVE_CLUSTERED_LIGHTS; ++i)
	{
		selected[i] = -1;
		selected_scores[i] = 0.0f;
	}
	for (int candidate_index = 0; candidate_index < EMISSIVE_CLUSTERED_CANDIDATES; ++candidate_index)
	{
		const emissive_clustered_candidate_t *const candidate = &receiver->candidates[candidate_index];
		if (candidate->base_score <= 0.0f)
			break;
		if (!candidate->visible)
			continue;
		const qboolean transient = (candidate->source_index & 0x80000000u) != 0u;
		const uint32_t source_index = candidate->source_index & 0x7FFFFFFFu;
		const float	   modulation = transient || !emissive_light_modulations ? 1.0f : emissive_light_modulations[source_index];
		const float	   score = candidate->base_score * modulation;
		if (score <= selected_scores[light_limit - 1])
			continue;
		int destination = light_limit - 1;
		while (destination > 0 && score > selected_scores[destination - 1])
		{
			selected[destination] = selected[destination - 1];
			selected_scores[destination] = selected_scores[destination - 1];
			--destination;
		}
		selected[destination] = candidate_index;
		selected_scores[destination] = score;
	}
	int num_selected = 0;
	for (int i = 0; i < light_limit && selected[i] >= 0; ++i)
	{
		const emissive_clustered_candidate_t *const candidate = &receiver->candidates[selected[i]];
		const qboolean								transient = (candidate->source_index & 0x80000000u) != 0u;
		const uint32_t								source_index = candidate->source_index & 0x7FFFFFFFu;
		const float									modulation = transient || !emissive_light_modulations ? 1.0f : emissive_light_modulations[source_index];
		memcpy (lights[i].direction, candidate->direction, sizeof (lights[i].direction));
		VectorScale (candidate->color, modulation, lights[i].color);
		lights[i].color[3] = 1.0f;
		Atomic_IncrementUInt32 (&emissive_clustered_alias_contributors);
		++num_selected;
	}
	return num_selected;
}

qboolean R_EmissiveApproximatePointLight (const vec3_t origin, const vec3_t normal, int source_budget, vec3_t color)
{
	color[0] = color[1] = color[2] = 0.0f;
	if (!cl.worldmodel || r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f || r_fullbright_cheatsafe || r_lightmap_cheatsafe)
		return false;
	const int total_lights = num_emissive_lights + num_transient_emissive_lights;
	const int evaluated_lights = q_min (total_lights, q_max (0, source_budget));
	for (int light_index = 0; light_index < evaluated_lights; ++light_index)
	{
		const qboolean transient = light_index >= num_emissive_lights;
		const int source_index = transient ? light_index - num_emissive_lights : light_index;
		const emissive_light_t *const source = transient ? &transient_emissive_lights[source_index] : &emissive_cacheable_lights[source_index];
		const float modulation = transient || !emissive_light_modulations ? 1.0f : emissive_light_modulations[source_index];
		if (modulation <= 0.0f)
			continue;
		vec3_t light_vector;
		VectorSubtract (source->origin, origin, light_vector);
		const float distance = VectorLength (light_vector);
		if (distance <= 0.001f || distance >= source->radius)
			continue;
		VectorScale (light_vector, 1.0f / distance, light_vector);
		const float lambert = fabsf (DotProduct (normal, light_vector));
		if (lambert <= 0.0f)
			continue;
		qboolean visible;
		vec3_t trace_origin, trace_target;
		VectorCopy (origin, trace_origin);
		VectorCopy (source->origin, trace_target);
		if (CLAMP (0, (int)r_emissive_rt_occluders.value, 2) > 0)
		{
			vec3_t impact, impact_normal;
			visible = CL_TraceLine (trace_origin, trace_target, impact, impact_normal, NULL) >= 0.999f;
		}
		else
		{
			trace_t trace;
			memset (&trace, 0, sizeof (trace));
			trace.fraction = 1.0f;
			SV_RecursiveHullCheck (cl.worldmodel->hulls, trace_origin, trace_target, &trace, CONTENTMASK_ANYSOLID);
			visible = !trace.allsolid && trace.fraction >= 0.999f;
		}
		if (!visible)
			continue;
		const float scale = modulation * source->intensity * (1.0f - distance / source->radius) * lambert;
		VectorMA (color, scale, source->color, color);
	}
	return color[0] > 0.0f || color[1] > 0.0f || color[2] > 0.0f;
}

void R_EmissiveClusteredAliasStats (
	int *records, int *active, int *ready, uint32_t *receivers, uint32_t *builds, uint32_t *source_evaluations, uint32_t *shadow_tests,
	uint32_t *shadow_rejections, uint32_t *contributors)
{
	*records = emissive_alias_receiver_count;
	*active = *ready = 0;
	for (int i = 0; i < emissive_alias_receiver_count; ++i)
	{
		*active += emissive_alias_receivers[i].active;
		*ready += emissive_alias_receivers[i].active && emissive_alias_receivers[i].ready;
	}
	*receivers = Atomic_LoadUInt32 (&emissive_clustered_alias_receivers);
	*builds = Atomic_LoadUInt32 (&emissive_clustered_alias_builds);
	*source_evaluations = Atomic_LoadUInt32 (&emissive_clustered_alias_source_evaluations);
	*shadow_tests = Atomic_LoadUInt32 (&emissive_clustered_alias_shadow_tests);
	*shadow_rejections = Atomic_LoadUInt32 (&emissive_clustered_alias_shadow_rejections);
	*contributors = Atomic_LoadUInt32 (&emissive_clustered_alias_contributors);
}

static VkDescriptorSet R_AllocateEmissiveBrushReceiverDescriptorSet (
	const emissive_brush_receiver_t *receiver, const emissive_brush_receiver_layer_t *layer, gltexture_t *output, int detail, const char *kind)
{
	VkDescriptorSet descriptor_set = R_AllocateDescriptorSet (&vulkan_globals.emissive_brush_receiver_set_layout);
	GL_SetObjectName (
		(uint64_t)descriptor_set, VK_OBJECT_TYPE_DESCRIPTOR_SET,
		va ("emissive brush receiver %08x lm %d %s", receiver->receiver_instance_id, layer->lightmap, kind));
	VkDescriptorImageInfo images[2];
	memset (images, 0, sizeof (images));
	images[0].imageView = output->target_image_view;
	images[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	images[1].imageView = layer->surface_indices_texture->image_view;
	images[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	const VkBuffer		   fallback = num_emissive_lights ? emissive_lights_buffer : transient_emissive_lights_buffer;
	VkDescriptorBufferInfo buffers[7] = {
		{surface_data_buffer, 0, num_surfaces * sizeof (lm_compute_surface_data_t)},
		{bmodel_vertex_buffer, 0, VK_WHOLE_SIZE},
		{num_emissive_lights ? emissive_lights_buffer : fallback, 0, VK_WHOLE_SIZE},
		{num_emissive_lights ? emissive_modulations_buffer : fallback, 0, VK_WHOLE_SIZE},
		{num_transient_emissive_lights ? transient_emissive_lights_buffer : fallback, 0, VK_WHOLE_SIZE},
		{receiver->source_indices_buffer, 0, sizeof (receiver->source_indices)},
		{layer->visibility_buffers[detail], 0, VK_WHOLE_SIZE},
	};
	VkWriteDescriptorSet writes[9];
	memset (writes, 0, sizeof (writes));
	for (int binding = 0; binding < countof (writes); ++binding)
	{
		writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[binding].dstSet = descriptor_set;
		writes[binding].dstBinding = binding;
		writes[binding].descriptorCount = 1;
		writes[binding].descriptorType = binding == 0	? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
										 : binding == 1 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
														: VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		if (binding < 2)
			writes[binding].pImageInfo = &images[binding];
		else
			writes[binding].pBufferInfo = &buffers[binding - 2];
	}
	vkUpdateDescriptorSets (vulkan_globals.device, countof (writes), writes, 0, NULL);
	return descriptor_set;
}

static void R_UpdateEmissiveBrushReceiverLightmaps (cb_context_t *cbx)
{
	VkAccelerationStructureKHR direct_tlas = R_EmissiveDirectAccelerationStructure ();
	qboolean dirty = false;
	for (int i = 0; i < emissive_brush_receiver_count; ++i)
		dirty |= emissive_brush_receivers[i].active && emissive_brush_receivers[i].dirty;
	if (!dirty || direct_tlas == VK_NULL_HANDLE || vulkan_globals.emissive_brush_receiver_pipeline.handle == VK_NULL_HANDLE ||
		(!num_emissive_lights && !num_transient_emissive_lights))
		return;

	R_BeginDebugUtilsLabel (cbx, "Update Emissive Brush Receivers");
	GL_BeginEmissiveBrushReceiverTimestamp (cbx);
	const vulkan_pipeline_t *const pipeline = &vulkan_globals.emissive_brush_receiver_pipeline;
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
	qboolean visibility_written = false;
	ZEROED_STRUCT (VkWriteDescriptorSetAccelerationStructureKHR, tlas_info);
	tlas_info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	tlas_info.accelerationStructureCount = 1;
	tlas_info.pAccelerationStructures = &direct_tlas;
	ZEROED_STRUCT (VkWriteDescriptorSet, tlas_write);
	tlas_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	tlas_write.pNext = &tlas_info;
	tlas_write.dstBinding = 0;
	tlas_write.descriptorCount = 1;
	tlas_write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	vulkan_globals.vk_cmd_push_descriptor_set (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 1, 1, &tlas_write);

	if (emissive_brush_receiver_uploaded_source_generation != emissive_brush_receiver_source_generation && num_transient_emissive_lights)
	{
		R_UpdateTransientEmissiveBuffer (
			cbx->cb, transient_emissive_lights_buffer, transient_emissive_lights, num_transient_emissive_lights * sizeof (*transient_emissive_lights));
		ZEROED_STRUCT (VkMemoryBarrier, memory_barrier);
		memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier (cbx->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
	}
	emissive_brush_receiver_uploaded_source_generation = emissive_brush_receiver_source_generation;

	for (int receiver_index = 0; receiver_index < emissive_brush_receiver_count; ++receiver_index)
	{
		emissive_brush_receiver_t *const receiver = &emissive_brush_receivers[receiver_index];
		if (!receiver->active || !receiver->dirty)
			continue;
		for (int layer_index = 0; layer_index < receiver->num_layers; ++layer_index)
		{
			emissive_brush_receiver_layer_t *const layer = &receiver->layers[layer_index];
			gltexture_t							  *outputs[2] = {layer->coarse_texture, layer->detail_texture};
			VkDescriptorSet						  *sets[2] = {&layer->coarse_descriptor_set, &layer->detail_descriptor_set};
			for (int detail = 0; detail < 2; ++detail)
			{
				gltexture_t *const output = outputs[detail];
				if (!output)
					continue;
				if (*sets[detail] == VK_NULL_HANDLE)
					*sets[detail] = R_AllocateEmissiveBrushReceiverDescriptorSet (receiver, layer, output, detail, detail ? "detail" : "coarse");
				R_EmissiveComputeImageBarrier (cbx, output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
				vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, sets[detail], 0, NULL);
				emissive_brush_receiver_push_constants_t constants;
				memset (&constants, 0, sizeof (constants));
				constants.cacheable_count = num_emissive_lights;
				constants.transient_count = num_transient_emissive_lights;
				constants.coordinate_scale = detail ? R_EmissiveDetailScale () : 1;
				constants.atlas_offset_x = layer->atlas_offset[0];
				constants.atlas_offset_y = layer->atlas_offset[1];
				constants.receiver_instance_id = receiver->receiver_instance_id;
				constants.source_count = receiver->source_count;
				constants.radiance_only = receiver->radiance_only;
				memcpy (constants.transform, receiver->transform, sizeof (constants.transform));
				constants.occluder_mask = R_EmissiveOccluderMask ();
				R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
				vkCmdDispatch (cbx->cb, (output->width + 7) / 8, (output->height + 7) / 8, 1);
				Atomic_IncrementUInt32 (&emissive_brush_receiver_dispatches);
				if (receiver->radiance_only)
					Atomic_IncrementUInt32 (&emissive_brush_receiver_no_ray_dispatches);
				else
					visibility_written = true;
				R_PublishEmissiveBounceImage (cbx, output);
			}
		}
		receiver->dirty = false;
		receiver->radiance_only = false;
		receiver->ready = true;
		Atomic_IncrementUInt32 (&emissive_brush_receiver_updates);
	}
	if (visibility_written)
	{
		ZEROED_STRUCT (VkMemoryBarrier, visibility_barrier);
		visibility_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		visibility_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		visibility_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier (
			cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &visibility_barrier, 0, NULL, 0, NULL);
	}
	GL_EndEmissiveBrushReceiverTimestamp (cbx);
	R_EndDebugUtilsLabel (cbx);
}

/*
==================
R_EmissiveLightmapStats
==================
*/
void R_EmissiveLightmapStats (int *count, uint64_t *logical_bytes, uint64_t *allocated_bytes)
{
	*count = 0;
	*logical_bytes = 0;
	*allocated_bytes = 0;
	for (int i = 0; i < lightmap_count; ++i)
		if (lightmaps[i].emissive_texture)
		{
			++*count;
			*logical_bytes += (uint64_t)lightmaps[i].emissive_texture->width * lightmaps[i].emissive_texture->height * 8;
			*allocated_bytes += GL_HeapGetAllocationSize (lightmaps[i].emissive_texture->allocation);
		}
}

/*
==================
R_AllocateEmissiveComputeDescriptorSet
==================
*/
static VkDescriptorSet R_AllocateEmissiveComputeDescriptorSet (
	const struct lightmap_s *lightmap, const gltexture_t *output_texture, const gltexture_t *cacheable_texture, const VkDescriptorBufferInfo source_buffers[6],
	const char *kind, int lightmap_index)
{
	VkDescriptorSet descriptor_set = R_AllocateDescriptorSet (&vulkan_globals.emissive_compute_set_layout);
	GL_SetObjectName ((uint64_t)descriptor_set, VK_OBJECT_TYPE_DESCRIPTOR_SET, va ("emissive %s %07i desc set", kind, lightmap_index));

	VkDescriptorImageInfo image_infos[3];
	memset (image_infos, 0, sizeof (image_infos));
	image_infos[0].imageView = output_texture->target_image_view;
	image_infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	image_infos[1].imageView = lightmap->surface_indices_texture->image_view;
	image_infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	image_infos[2].imageView = (cacheable_texture ? cacheable_texture : output_texture)->image_view;
	image_infos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkDescriptorBufferInfo buffer_infos[9];
	memset (buffer_infos, 0, sizeof (buffer_infos));
	buffer_infos[0].buffer = surface_data_buffer;
	buffer_infos[0].range = num_surfaces * sizeof (lm_compute_surface_data_t);
	buffer_infos[1].buffer = bmodel_vertex_buffer;
	buffer_infos[1].range = VK_WHOLE_SIZE;
	buffer_infos[2].buffer = surface_submodels_buffer;
	buffer_infos[2].range = num_surfaces * sizeof (uint32_t);
	buffer_infos[3] = source_buffers[0];
	buffer_infos[4] = source_buffers[1];
	buffer_infos[5] = source_buffers[2];
	buffer_infos[6] = source_buffers[3];
	buffer_infos[7] = source_buffers[4];
	buffer_infos[8] = source_buffers[5];

	VkWriteDescriptorSet writes[12];
	memset (writes, 0, sizeof (writes));
	for (int binding = 0; binding < countof (writes); ++binding)
	{
		writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[binding].dstBinding = binding;
		writes[binding].descriptorCount = 1;
		writes[binding].dstSet = descriptor_set;
		writes[binding].descriptorType = binding == 0	? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
										 : binding == 1 || binding == 8 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
														: VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		if (binding < 2 || binding == 8)
			writes[binding].pImageInfo = &image_infos[binding == 8 ? 2 : binding];
		else
			writes[binding].pBufferInfo = &buffer_infos[binding < 8 ? binding - 2 : binding - 3];
	}
	vkUpdateDescriptorSets (vulkan_globals.device, countof (writes), writes, 0, NULL);
	return descriptor_set;
}

static void R_FreeEmissiveOccluderDescriptorSets (void)
{
	if (emissive_occluder_detail_descriptor_sets)
		for (int i = 0; i < lightmap_count; ++i)
			if (emissive_occluder_detail_descriptor_sets[i] != VK_NULL_HANDLE)
				R_FreeDescriptorSet (emissive_occluder_detail_descriptor_sets[i], &vulkan_globals.emissive_compute_set_layout);
	SAFE_FREE (emissive_occluder_detail_descriptor_sets);
}

static void R_FreeTransientEmissiveDescriptorSets (void)
{
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		if (lightmap->emissive_transient_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmap->emissive_transient_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
			lightmap->emissive_transient_descriptor_set = VK_NULL_HANDLE;
		}
		if (lightmap->emissive_transient_detail_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmap->emissive_transient_detail_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
			lightmap->emissive_transient_detail_descriptor_set = VK_NULL_HANDLE;
		}
		VkDescriptorSet *const bandlimit_sets[] = {&lightmap->emissive_bandlimit_transient_descriptor_set};
		for (int set = 0; set < countof (bandlimit_sets); ++set)
			if (*bandlimit_sets[set] != VK_NULL_HANDLE)
			{
				R_FreeDescriptorSet (*bandlimit_sets[set], &vulkan_globals.emissive_compute_set_layout);
				*bandlimit_sets[set] = VK_NULL_HANDLE;
			}
	}
}

static void R_FreeEmissiveRadianceOverlayDescriptorSets (void)
{
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		if (lightmap->emissive_radiance_overlay_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmap->emissive_radiance_overlay_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
			lightmap->emissive_radiance_overlay_descriptor_set = VK_NULL_HANDLE;
		}
		if (lightmap->emissive_radiance_overlay_detail_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmap->emissive_radiance_overlay_detail_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
			lightmap->emissive_radiance_overlay_detail_descriptor_set = VK_NULL_HANDLE;
		}
		if (lightmap->emissive_radiance_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmap->emissive_radiance_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
			lightmap->emissive_radiance_descriptor_set = VK_NULL_HANDLE;
		}
		if (lightmap->emissive_radiance_detail_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmap->emissive_radiance_detail_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
			lightmap->emissive_radiance_detail_descriptor_set = VK_NULL_HANDLE;
		}
	}
}

static void R_CreateEmissiveRadianceOverlayDescriptorSets (void)
{
	R_FreeEmissiveRadianceOverlayDescriptorSets ();
	if (emissive_radiance_tiles_buffer == VK_NULL_HANDLE || emissive_lights_buffer == VK_NULL_HANDLE)
		return;
	const VkDescriptorBufferInfo source_buffers[6] = {
		{emissive_lights_buffer, 0, VK_WHOLE_SIZE},
		{emissive_radiance_tiles_buffer, 0, VK_WHOLE_SIZE},
		{emissive_tile_sources_buffer != VK_NULL_HANDLE ? emissive_tile_sources_buffer : emissive_lights_buffer, 0,
		 emissive_tile_sources_buffer != VK_NULL_HANDLE ? VK_WHOLE_SIZE : sizeof (uint32_t)},
		{emissive_modulations_buffer, 0, VK_WHOLE_SIZE},
		{emissive_visibility_available ? emissive_visibility_buffer : emissive_modulations_buffer, 0, VK_WHOLE_SIZE},
		{emissive_light_seeds_buffer != VK_NULL_HANDLE ? emissive_light_seeds_buffer : emissive_lights_buffer, 0, VK_WHOLE_SIZE},
	};
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		if (lightmap->emissive_texture)
			lightmap->emissive_radiance_descriptor_set =
				R_AllocateEmissiveComputeDescriptorSet (lightmap, lightmap->emissive_texture, NULL, source_buffers, "radiance", i);
		if (emissive_visibility_available && lightmap->emissive_detail_texture)
			lightmap->emissive_radiance_detail_descriptor_set =
				R_AllocateEmissiveComputeDescriptorSet (lightmap, lightmap->emissive_detail_texture, NULL, source_buffers, "radiance detail", i);
		if (lightmap->emissive_transient_texture && lightmap->emissive_texture)
			lightmap->emissive_radiance_overlay_descriptor_set = R_AllocateEmissiveComputeDescriptorSet (
				lightmap, lightmap->emissive_transient_texture, lightmap->emissive_texture, source_buffers, "radiance overlay", i);
		if (emissive_visibility_available && lightmap->emissive_transient_detail_texture && lightmap->emissive_detail_texture)
			lightmap->emissive_radiance_overlay_detail_descriptor_set = R_AllocateEmissiveComputeDescriptorSet (
				lightmap, lightmap->emissive_transient_detail_texture, lightmap->emissive_detail_texture, source_buffers, "radiance overlay detail", i);
	}
}

static VkDeviceSize R_EmissiveBufferMemorySize (VkDeviceSize size, VkBufferUsageFlags usage)
{
	ZEROED_STRUCT (VkBufferCreateInfo, create_info);
	create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	create_info.size = size;
	create_info.usage = usage;
	VkBuffer	   buffer;
	const VkResult result = vkCreateBuffer (vulkan_globals.device, &create_info, NULL, &buffer);
	if (result != VK_SUCCESS)
		Sys_Error ("vkCreateBuffer failed with code %i", (int)result);
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements (vulkan_globals.device, buffer, &requirements);
	vkDestroyBuffer (vulkan_globals.device, buffer, NULL);
	return requirements.size;
}

static size_t R_TransientEmissiveCapacity (size_t required)
{
	size_t capacity = 256;
	while (capacity < required)
		capacity *= 2;
	return capacity;
}

static void R_TransientEmissiveSourceBuffers (VkDescriptorBufferInfo source_buffers[6])
{
	source_buffers[0].buffer = transient_emissive_lights_buffer;
	source_buffers[0].offset = 0;
	source_buffers[0].range = transient_emissive_lights_capacity;
	source_buffers[1].buffer = transient_emissive_tiles_buffer;
	source_buffers[1].offset = 0;
	source_buffers[1].range = transient_emissive_tiles_capacity;
	source_buffers[2].buffer = transient_emissive_tile_sources_buffer;
	source_buffers[2].offset = 0;
	source_buffers[2].range = transient_emissive_tile_sources_capacity;
	source_buffers[3].buffer = transient_emissive_lights_buffer;
	source_buffers[3].offset = 0;
	source_buffers[3].range = transient_emissive_lights_capacity;
	source_buffers[4].buffer = transient_emissive_lights_buffer;
	source_buffers[4].offset = 0;
	source_buffers[4].range = transient_emissive_lights_capacity;
	source_buffers[5].buffer = transient_emissive_light_seeds_buffer != VK_NULL_HANDLE ? transient_emissive_light_seeds_buffer
																 : transient_emissive_lights_buffer;
	source_buffers[5].offset = 0;
	source_buffers[5].range = VK_WHOLE_SIZE;
}

static void R_EnsureTransientEmissiveResources (void)
{
	const size_t   lights_size = q_max ((size_t)num_transient_emissive_lights * sizeof (*transient_emissive_lights), sizeof (uint32_t));
	const size_t   tiles_size = q_max ((size_t)num_transient_emissive_tiles * sizeof (*transient_emissive_tiles), sizeof (uint32_t));
	const size_t   sources_size = q_max ((size_t)num_transient_emissive_tile_sources * sizeof (*transient_emissive_tile_sources), sizeof (uint32_t));
	/* Seeds are independent of the other buffers: band limiting can need them with
	 * no capacity growth, and ordinary growth must never leave a stale seed handle. */
	const size_t   seeds_required = emissive_bandlimit_active
						 ? R_TransientEmissiveCapacity (q_max ((size_t)num_transient_emissive_lights * sizeof (uint32_t), sizeof (uint32_t)))
						 : 0;
	const qboolean seeds_grow = seeds_required > 0 &&
		(transient_emissive_light_seeds_buffer == VK_NULL_HANDLE || transient_emissive_light_seeds_capacity < seeds_required);
	const qboolean grow = lights_size > transient_emissive_lights_capacity || tiles_size > transient_emissive_tiles_capacity ||
						  sources_size > transient_emissive_tile_sources_capacity;
	if (grow || seeds_grow)
	{
		if (transient_emissive_lights_buffer != VK_NULL_HANDLE || emissive_brush_receiver_count)
			GL_WaitForDeviceIdle ();
		R_FreeEmissiveBrushReceiverDescriptorSets ();
		R_FreeTransientEmissiveDescriptorSets ();
		if (grow)
		{
			R_FreeBuffer (transient_emissive_lights_buffer, &transient_emissive_lights_buffer_memory, &num_vulkan_bmodel_allocations);
			R_FreeBuffer (transient_emissive_tiles_buffer, &transient_emissive_tiles_buffer_memory, &num_vulkan_bmodel_allocations);
			R_FreeBuffer (transient_emissive_tile_sources_buffer, &transient_emissive_tile_sources_buffer_memory, &num_vulkan_bmodel_allocations);
			transient_emissive_lights_capacity = R_TransientEmissiveCapacity (lights_size);
			transient_emissive_tiles_capacity = R_TransientEmissiveCapacity (tiles_size);
			transient_emissive_tile_sources_capacity = R_TransientEmissiveCapacity (sources_size);
			R_CreateBuffer (
				&transient_emissive_lights_buffer, &transient_emissive_lights_buffer_memory, transient_emissive_lights_capacity,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL,
				"Transient emissive source lights");
			R_CreateBuffer (
				&transient_emissive_tiles_buffer, &transient_emissive_tiles_buffer_memory, transient_emissive_tiles_capacity,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL,
				"Transient emissive logical tiles");
			R_CreateBuffer (
				&transient_emissive_tile_sources_buffer, &transient_emissive_tile_sources_buffer_memory, transient_emissive_tile_sources_capacity,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
				&num_vulkan_bmodel_allocations, NULL, "Transient emissive tile source indices");
		}
		if (seeds_grow)
		{
			R_FreeBuffer (transient_emissive_light_seeds_buffer, &transient_emissive_light_seeds_buffer_memory, &num_vulkan_bmodel_allocations);
			transient_emissive_light_seeds_buffer = VK_NULL_HANDLE;
			transient_emissive_light_seeds_capacity = seeds_required;
			R_CreateBuffer (
				&transient_emissive_light_seeds_buffer, &transient_emissive_light_seeds_buffer_memory, transient_emissive_light_seeds_capacity,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL,
				"Transient emissive source sampling seeds");
		}
	}

	qboolean created_texture = false;
	uint64_t detail_required_bytes = 0;
	for (int i = 0; i < lightmap_count; ++i)
	{
		const struct lightmap_s *const lightmap = &lightmaps[i];
		if (lightmap->emissive_detail_texture)
			detail_required_bytes += GL_HeapGetAllocationSize (lightmap->emissive_detail_texture->allocation);
		if (lightmap->emissive_transient_detail_texture)
			detail_required_bytes += GL_HeapGetAllocationSize (lightmap->emissive_transient_detail_texture->allocation);
		else if (lightmap->emissive_detail_texture)
			detail_required_bytes += TexMgr_RGBA16FImageMemorySize (
				lightmap->emissive_detail_texture->width, lightmap->emissive_detail_texture->height);
	}
	const uint64_t retained_required_bytes = emissive_bandlimit_active ? emissive_visibility_buffer_memory.size : 0;
	const qboolean transient_detail_admitted =
		detail_required_bytes + retained_required_bytes <= (uint64_t)EMISSIVE_DETAIL_MEMORY_BUDGET_MB * 1024 * 1024;
	emissive_bandlimit_peak_bytes = q_max (
		emissive_bandlimit_peak_bytes, detail_required_bytes + retained_required_bytes);
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		if (!lightmap->emissive_texture)
			continue;
		char name[48];
		if (!lightmap->emissive_transient_texture)
		{
			q_snprintf (name, sizeof (name), "emissive_transient_%07i", i);
			lightmap->emissive_transient_texture = TexMgr_LoadImage (
				cl.worldmodel, name, lightmap->emissive_texture->width, lightmap->emissive_texture->height, SRC_RGBA16F, NULL, "", 0,
				TEXPREF_LINEAR | TEXPREF_NOPICMIP);
			created_texture = true;
		}
		if (transient_detail_admitted && lightmap->emissive_detail_texture && !lightmap->emissive_transient_detail_texture)
		{
			q_snprintf (name, sizeof (name), "emissive_transient_detail_%07i", i);
			lightmap->emissive_transient_detail_texture = TexMgr_LoadImage (
				cl.worldmodel, name, lightmap->emissive_detail_texture->width, lightmap->emissive_detail_texture->height, SRC_RGBA16F, NULL, "", 0,
				TEXPREF_LINEAR | TEXPREF_NOPICMIP);
			created_texture = true;
		}
	}
	if (created_texture)
	{
		transient_emissive_initialized = false;
		transient_emissive_detail_cache_copied = false;
	}
	if (grow || seeds_grow || created_texture || (lightmap_count && lightmaps[0].emissive_transient_descriptor_set == VK_NULL_HANDLE))
	{
		VkDescriptorBufferInfo source_buffers[6];
		R_TransientEmissiveSourceBuffers (source_buffers);
		R_FreeTransientEmissiveDescriptorSets ();
		for (int i = 0; i < lightmap_count; ++i)
		{
			struct lightmap_s *const lightmap = &lightmaps[i];
			if (lightmap->emissive_transient_texture)
				lightmap->emissive_transient_descriptor_set = R_AllocateEmissiveComputeDescriptorSet (
					lightmap, lightmap->emissive_transient_texture, lightmap->emissive_texture, source_buffers, "transient coarse", i);
			if (lightmap->emissive_transient_detail_texture)
				lightmap->emissive_transient_detail_descriptor_set = R_AllocateEmissiveComputeDescriptorSet (
					lightmap, lightmap->emissive_transient_detail_texture, lightmap->emissive_detail_texture, source_buffers, "transient detail", i);
			if (emissive_bandlimit_active && lightmap->emissive_transient_detail_texture)
			{
				lightmap->emissive_bandlimit_transient_descriptor_set = R_AllocateEmissiveComputeDescriptorSet (
					lightmap, lightmap->emissive_transient_detail_texture, lightmap->emissive_detail_texture, source_buffers, "bandlimit transient", i);
			}
		}
		R_CreateEmissiveRadianceOverlayDescriptorSets ();
	}
	/* Band-limit transient sets are conditional and can go missing independently of
	 * buffer growth; recreate missing ones whenever the mode needs them so a
	 * required dispatch is never silently skipped. */
	if (emissive_bandlimit_active)
	{
		VkDescriptorBufferInfo source_buffers[6];
		R_TransientEmissiveSourceBuffers (source_buffers);
		for (int i = 0; i < lightmap_count; ++i)
		{
			struct lightmap_s *const lightmap = &lightmaps[i];
			if (!lightmap->emissive_transient_detail_texture || lightmap->emissive_bandlimit_transient_descriptor_set != VK_NULL_HANDLE)
				continue;
			lightmap->emissive_bandlimit_transient_descriptor_set = R_AllocateEmissiveComputeDescriptorSet (
				lightmap, lightmap->emissive_transient_detail_texture, lightmap->emissive_detail_texture, source_buffers, "bandlimit transient", i);
		}
	}
}

/*
==================
R_SetEmissiveLights
==================
*/
void R_SetEmissiveLights (const emissive_light_t *lights, const byte *styles, int count, const emissive_surface_light_t *surface_lights, int num_surface_lights)
{
	R_InvalidateEmissiveBrushReceiverSources ();
	num_emissive_light_diagnostics = q_min (count, MAX_DLIGHTS);
	if (num_emissive_light_diagnostics)
		memcpy (emissive_light_diagnostics, lights, num_emissive_light_diagnostics * sizeof (*lights));
	if (emissive_lights_buffer != VK_NULL_HANDLE || emissive_tiles_buffer != VK_NULL_HANDLE || emissive_occluder_tiles_buffer != VK_NULL_HANDLE ||
		emissive_tile_sources_buffer != VK_NULL_HANDLE ||
		emissive_modulations_buffer != VK_NULL_HANDLE || emissive_visibility_buffer != VK_NULL_HANDLE || emissive_radiance_tiles_buffer != VK_NULL_HANDLE ||
		emissive_brush_receiver_count)
		GL_WaitForDeviceIdle ();
	R_FreeEmissiveBrushReceiverDescriptorSets ();
	R_FreeEmissiveBandlimitDescriptorSets ();
	R_FreeEmissiveOccluderDescriptorSets ();
	R_FreeEmissiveRadianceOverlayDescriptorSets ();
	for (int i = 0; i < lightmap_count; ++i)
	{
		if (lightmaps[i].emissive_coarse_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmaps[i].emissive_coarse_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
			lightmaps[i].emissive_coarse_descriptor_set = VK_NULL_HANDLE;
		}
		if (lightmaps[i].emissive_detail_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmaps[i].emissive_detail_descriptor_set, &vulkan_globals.emissive_compute_set_layout);
			lightmaps[i].emissive_detail_descriptor_set = VK_NULL_HANDLE;
		}
		if (lightmaps[i].emissive_bounce_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmaps[i].emissive_bounce_descriptor_set, &vulkan_globals.emissive_bounce_set_layout);
			lightmaps[i].emissive_bounce_descriptor_set = VK_NULL_HANDLE;
		}
		if (lightmaps[i].emissive_bounce_detail_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmaps[i].emissive_bounce_detail_descriptor_set, &vulkan_globals.emissive_bounce_set_layout);
			lightmaps[i].emissive_bounce_detail_descriptor_set = VK_NULL_HANDLE;
		}
		VkDescriptorSet *const bounce_sets[] = {
			&lightmaps[i].emissive_bounce_output_descriptor_set,
			&lightmaps[i].emissive_bounce_output_detail_descriptor_set,
			&lightmaps[i].emissive_bounce_transient_descriptor_set,
			&lightmaps[i].emissive_bounce_transient_detail_descriptor_set,
			&lightmaps[i].emissive_bounce_transient_output_descriptor_set,
			&lightmaps[i].emissive_bounce_transient_output_detail_descriptor_set,
		};
		for (int set = 0; set < countof (bounce_sets); ++set)
			if (*bounce_sets[set] != VK_NULL_HANDLE)
			{
				R_FreeDescriptorSet (*bounce_sets[set], &vulkan_globals.emissive_bounce_set_layout);
				*bounce_sets[set] = VK_NULL_HANDLE;
			}
		if (lightmaps[i].emissive_bounce_debug_descriptor_set != VK_NULL_HANDLE)
		{
			R_FreeDescriptorSet (lightmaps[i].emissive_bounce_debug_descriptor_set, &vulkan_globals.emissive_bounce_set_layout);
			lightmaps[i].emissive_bounce_debug_descriptor_set = VK_NULL_HANDLE;
		}
	}
	R_FreeBuffer (emissive_lights_buffer, &emissive_lights_buffer_memory, &num_vulkan_bmodel_allocations);
	emissive_lights_buffer = VK_NULL_HANDLE;
	R_FreeBuffer (emissive_light_seeds_buffer, &emissive_light_seeds_buffer_memory, &num_vulkan_bmodel_allocations);
	emissive_light_seeds_buffer = VK_NULL_HANDLE;
	R_FreeBuffer (emissive_tiles_buffer, &emissive_tiles_buffer_memory, &num_vulkan_bmodel_allocations);
	emissive_tiles_buffer = VK_NULL_HANDLE;
	R_FreeBuffer (emissive_occluder_tiles_buffer, &emissive_occluder_tiles_buffer_memory, &num_vulkan_bmodel_allocations);
	emissive_occluder_tiles_buffer = VK_NULL_HANDLE;
	R_FreeBuffer (emissive_tile_sources_buffer, &emissive_tile_sources_buffer_memory, &num_vulkan_bmodel_allocations);
	emissive_tile_sources_buffer = VK_NULL_HANDLE;
	R_FreeBuffer (emissive_modulations_buffer, &emissive_modulations_buffer_memory, &num_vulkan_bmodel_allocations);
	emissive_modulations_buffer = VK_NULL_HANDLE;
	R_FreeBuffer (emissive_visibility_buffer, &emissive_visibility_buffer_memory, &num_vulkan_bmodel_allocations);
	emissive_visibility_buffer = VK_NULL_HANDLE;
	R_FreeBuffer (emissive_radiance_tiles_buffer, &emissive_radiance_tiles_buffer_memory, &num_vulkan_bmodel_allocations);
	emissive_radiance_tiles_buffer = VK_NULL_HANDLE;
	SAFE_FREE (emissive_light_styles);
	SAFE_FREE (emissive_cacheable_lights);
	SAFE_FREE (emissive_light_modulations);
	SAFE_FREE (emissive_radiance_tiles);
	num_emissive_lights = 0;
	num_emissive_radiance_tiles = 0;
	num_emissive_radiance_source_links = 0;
	num_emissive_radiance_tile_groups = 0;
	max_emissive_radiance_groups_per_tile = 0;
	num_emissive_modulation_groups = 0;
	emissive_radiance_coarse_pending = false;
	emissive_radiance_detail_pending = false;
	emissive_radiance_force_all_styled_tiles = false;
	emissive_modulations_pending = false;
	emissive_radiance_logged = false;
	emissive_visibility_available = false;
	emissive_coarse_pending = false;
	emissive_detail_pending = false;
	emissive_detail_building = false;
	emissive_detail_ready = false;
	emissive_bandlimit_active = false;
	if (++emissive_cacheable_direct_epoch == 0)
		++emissive_cacheable_direct_epoch;
	emissive_cacheable_bounce_epoch = emissive_transient_bounce_epoch = 0;
	emissive_bounce_cacheable_refresh_pending = emissive_bounce_ready;
	emissive_bounce_transient_refresh_pending = false;
	GL_ResetEmissiveCoarseTimestamp ();
	GL_ResetEmissiveDetailTimestamp ();
	GL_ResetEmissiveRadianceTimestamp ();
	if (lights && count > 0)
		emissive_bandlimit_active = R_EnableEmissiveBandlimit ();
	else
		R_ResetEmissiveBandlimitStats ();
	R_BuildEmissiveLogicalTileSources (lights, count, surface_lights, num_surface_lights);
	if (!lights || count <= 0)
		return;
	assert (styles);
	const uint64_t visibility_budget = (uint64_t)EMISSIVE_VISIBILITY_MEMORY_BUDGET_MB * 1024 * 1024;
	uint64_t visibility_size =
		(uint64_t)num_emissive_logical_tile_sources * 8 * R_EmissiveDetailScale () * 8 * R_EmissiveDetailScale () * sizeof (uint32_t);
	VkDeviceSize visibility_required_size =
		visibility_size ? R_EmissiveBufferMemorySize (visibility_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) : 0;
	const uint64_t detail_budget = (uint64_t)EMISSIVE_DETAIL_MEMORY_BUDGET_MB * 1024 * 1024;
	if (emissive_bandlimit_active &&
		(visibility_required_size > visibility_budget || emissive_bandlimit_peak_bytes + visibility_required_size > detail_budget))
	{
		const uint64_t rejected_required = emissive_bandlimit_peak_bytes + visibility_required_size;
		emissive_bandlimit_active = false;
		R_ResetEmissiveBandlimitStats ();
		emissive_bandlimit_budget_limited = true;
		emissive_bandlimit_peak_bytes = rejected_required;
		R_BuildEmissiveLogicalTileSources (lights, count, surface_lights, num_surface_lights);
		visibility_size =
			(uint64_t)num_emissive_logical_tile_sources * 8 * R_EmissiveDetailScale () * 8 * R_EmissiveDetailScale () * sizeof (uint32_t);
		visibility_required_size = visibility_size
			? R_EmissiveBufferMemorySize (visibility_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
			: 0;
		Con_DPrintf (
			"RT emissives: band-limited detail rejected (%" PRIu64
			" aggregate required bytes); classic detail remains active\n",
			rejected_required);
	}
	else if (emissive_bandlimit_active)
		emissive_bandlimit_peak_bytes += visibility_required_size;

	const size_t size = count * sizeof (*lights);
	emissive_cacheable_lights = Mem_Alloc (size);
	memcpy (emissive_cacheable_lights, lights, size);
	R_CreateBuffer (
		&emissive_lights_buffer, &emissive_lights_buffer_memory, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL, "Emissive source lights");
	R_StagingUploadBuffer (emissive_lights_buffer, size, (const byte *)lights);
	if (emissive_bandlimit_active)
	{
		uint32_t *const seeds = Mem_Alloc (count * sizeof (*seeds));
		for (int i = 0; i < count; ++i)
			seeds[i] = R_EmissiveLightSeed (lights[i].origin, lights[i].radius);
		const size_t seeds_size = count * sizeof (*seeds);
		R_CreateBuffer (
			&emissive_light_seeds_buffer, &emissive_light_seeds_buffer_memory, seeds_size,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL,
			"Emissive source sampling seeds");
		R_StagingUploadBuffer (emissive_light_seeds_buffer, seeds_size, (const byte *)seeds);
		Mem_Free (seeds);
	}
	emissive_light_styles = Mem_Alloc (count * sizeof (*emissive_light_styles));
	emissive_light_modulations = Mem_Alloc (count * sizeof (*emissive_light_modulations));
	qboolean used_styles[MAX_LIGHTSTYLES];
	memset (used_styles, 0, sizeof (used_styles));
	for (int i = 0; i < count; ++i)
	{
		emissive_light_styles[i] = styles[i];
		emissive_light_modulations[i] = styles[i] == 255 ? 1.0f : (float)d_lightstylevalue[styles[i]] / 256.0f;
		if (styles[i] != 255)
			used_styles[styles[i]] = true;
	}
	for (int style = 0; style < MAX_LIGHTSTYLES; ++style)
		if (used_styles[style])
			++num_emissive_modulation_groups;
	const size_t modulations_size = count * sizeof (*emissive_light_modulations);
	R_CreateBuffer (
		&emissive_modulations_buffer, &emissive_modulations_buffer_memory, modulations_size,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL,
		"Emissive source modulations");
	R_StagingUploadBuffer (emissive_modulations_buffer, modulations_size, (const byte *)emissive_light_modulations);
	const size_t tiles_size = num_emissive_logical_tiles * sizeof (*emissive_logical_tiles);
	if (tiles_size)
	{
		R_CreateBuffer (
			&emissive_tiles_buffer, &emissive_tiles_buffer_memory, tiles_size,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
			&num_vulkan_bmodel_allocations, NULL, "Emissive logical tiles");
		R_StagingUploadBuffer (emissive_tiles_buffer, tiles_size, (const byte *)emissive_logical_tiles);
		R_CreateBuffer (
			&emissive_occluder_tiles_buffer, &emissive_occluder_tiles_buffer_memory, tiles_size,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
			&num_vulkan_bmodel_allocations, NULL, "Emissive occluder dirty tiles");
		emissive_radiance_tiles = Mem_Alloc (tiles_size);
		R_CreateBuffer (
			&emissive_radiance_tiles_buffer, &emissive_radiance_tiles_buffer_memory, tiles_size,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL,
			"Emissive radiance logical tiles");
	}
	const size_t tile_sources_size = num_emissive_logical_tile_sources * sizeof (*emissive_logical_tile_sources);
	if (tile_sources_size)
	{
		R_CreateBuffer (
			&emissive_tile_sources_buffer, &emissive_tile_sources_buffer_memory, tile_sources_size,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
			&num_vulkan_bmodel_allocations, NULL, "Emissive tile source indices");
		R_StagingUploadBuffer (emissive_tile_sources_buffer, tile_sources_size, (const byte *)emissive_logical_tile_sources);
	}
	if (visibility_required_size && visibility_required_size <= visibility_budget)
	{
		R_CreateBuffer (
			&emissive_visibility_buffer, &emissive_visibility_buffer_memory, visibility_size,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL,
			"Emissive retained visibility");
		emissive_visibility_available = true;
		if (emissive_bandlimit_active)
		{
			emissive_bandlimit_logical_bytes = visibility_size;
			emissive_bandlimit_allocated_bytes = emissive_visibility_buffer_memory.size;
		}
	}
	else if (visibility_size)
		Con_DPrintf (
			"RT emissives: retained visibility rejected (%" PRIu64 " bytes, %" PRIu64 " byte budget); styled detail uses coarse fallback\n",
			visibility_required_size, visibility_budget);
	num_emissive_lights = count;
	const VkDescriptorBufferInfo source_buffers[6] = {
		{emissive_lights_buffer, 0, size},
		{emissive_tiles_buffer != VK_NULL_HANDLE ? emissive_tiles_buffer : emissive_lights_buffer, 0, tiles_size ? tiles_size : sizeof (uint32_t)},
		{emissive_tile_sources_buffer != VK_NULL_HANDLE ? emissive_tile_sources_buffer : emissive_lights_buffer, 0,
		 tile_sources_size ? tile_sources_size : sizeof (uint32_t)},
		{emissive_modulations_buffer, 0, modulations_size},
		{emissive_visibility_available ? emissive_visibility_buffer : emissive_modulations_buffer, 0,
		 emissive_visibility_available ? VK_WHOLE_SIZE : modulations_size},
		{emissive_light_seeds_buffer != VK_NULL_HANDLE ? emissive_light_seeds_buffer : emissive_lights_buffer, 0, VK_WHOLE_SIZE},
	};
	VkDescriptorBufferInfo occluder_source_buffers[6];
	memcpy (occluder_source_buffers, source_buffers, sizeof (occluder_source_buffers));
	occluder_source_buffers[1].buffer = emissive_occluder_tiles_buffer != VK_NULL_HANDLE ? emissive_occluder_tiles_buffer : emissive_lights_buffer;
	occluder_source_buffers[1].range = tiles_size ? tiles_size : sizeof (uint32_t);
	emissive_occluder_detail_descriptor_sets = Mem_Alloc (lightmap_count * sizeof (*emissive_occluder_detail_descriptor_sets));
	memset (emissive_occluder_detail_descriptor_sets, 0, lightmap_count * sizeof (*emissive_occluder_detail_descriptor_sets));
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		if (lightmap->emissive_texture)
			lightmap->emissive_coarse_descriptor_set = R_AllocateEmissiveComputeDescriptorSet (
				lightmap, lightmap->emissive_texture, NULL, source_buffers, "coarse", i);
		if (lightmap->emissive_detail_texture)
		{
			lightmap->emissive_detail_descriptor_set = R_AllocateEmissiveComputeDescriptorSet (
				lightmap, lightmap->emissive_detail_texture, NULL, source_buffers, "detail", i);
			emissive_occluder_detail_descriptor_sets[i] = R_AllocateEmissiveComputeDescriptorSet (
				lightmap, lightmap->emissive_detail_texture, NULL, occluder_source_buffers, "occluder detail", i);
			emissive_detail_pending = num_emissive_logical_tiles > 0 && R_EmissiveDetailAvailable ();
		}
	}
	R_CreateEmissiveBandlimitDescriptorSets (source_buffers);
	R_CreateEmissiveRadianceOverlayDescriptorSets ();
	R_BuildEmissiveBounceResources ();
	emissive_coarse_pending = true;
}

static void R_FreeEmissiveBandlimitDescriptorSets (void)
{
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		VkDescriptorSet *const sets[] = {
			&lightmap->emissive_bandlimit_detail_descriptor_set,
			&lightmap->emissive_bandlimit_radiance_descriptor_set,
			&lightmap->emissive_bandlimit_transient_descriptor_set,
		};
		for (int set = 0; set < countof (sets); ++set)
			if (*sets[set] != VK_NULL_HANDLE)
			{
				R_FreeDescriptorSet (*sets[set], &vulkan_globals.emissive_compute_set_layout);
				*sets[set] = VK_NULL_HANDLE;
			}
	}
}

static qboolean R_EnableEmissiveBandlimit (void)
{
	R_ResetEmissiveBandlimitStats ();
	if (!vulkan_globals.ray_query || CLAMP (0, (int)r_emissive_rt_bandlimit.value, 1) == 0)
		return false;
	for (int i = 0; i < lightmap_count; ++i)
		if (lightmaps[i].emissive_detail_texture)
			emissive_bandlimit_peak_bytes += GL_HeapGetAllocationSize (lightmaps[i].emissive_detail_texture->allocation);
	return true;
}

static void R_ResetEmissiveBandlimitStats (void)
{
	emissive_bandlimit_logical_bytes = 0;
	emissive_bandlimit_allocated_bytes = 0;
	emissive_bandlimit_peak_bytes = 0;
	emissive_bandlimit_budget_limited = false;
}

static void R_CreateEmissiveBandlimitDescriptorSets (const VkDescriptorBufferInfo source_buffers[6])
{
	R_FreeEmissiveBandlimitDescriptorSets ();
	if (!emissive_bandlimit_active)
	{
		return;
	}
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		if (!lightmap->emissive_detail_texture)
			continue;
		lightmap->emissive_bandlimit_detail_descriptor_set = R_AllocateEmissiveComputeDescriptorSet (
			lightmap, lightmap->emissive_detail_texture, NULL, source_buffers, "bandlimit detail", i);
		VkDescriptorBufferInfo radiance_buffers[6];
		memcpy (radiance_buffers, source_buffers, sizeof (radiance_buffers));
		radiance_buffers[1].buffer = emissive_radiance_tiles_buffer != VK_NULL_HANDLE ? emissive_radiance_tiles_buffer : emissive_lights_buffer;
		radiance_buffers[1].offset = 0;
		radiance_buffers[1].range = emissive_radiance_tiles_buffer != VK_NULL_HANDLE ? VK_WHOLE_SIZE : sizeof (uint32_t);
		lightmap->emissive_bandlimit_radiance_descriptor_set = R_AllocateEmissiveComputeDescriptorSet (
			lightmap, lightmap->emissive_detail_texture, NULL, radiance_buffers, "bandlimit radiance", i);
	}
	emissive_bandlimit_active = true;
}

static VkDescriptorSet R_AllocateEmissiveBounceDescriptorSet (struct lightmap_s *lightmap, gltexture_t *texture, int index)
{
	VkDescriptorSet set = R_AllocateDescriptorSet (&vulkan_globals.emissive_bounce_set_layout);
	GL_SetObjectName ((uint64_t)set, VK_OBJECT_TYPE_DESCRIPTOR_SET, va ("emissive bounce %07i desc set", index));
	VkDescriptorImageInfo images[2];
	memset (images, 0, sizeof (images));
	images[0].imageView = texture->target_image_view;
	images[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	images[1].imageView = lightmap->surface_indices_texture->image_view;
	images[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkDescriptorBufferInfo buffers[11] = {
		{surface_data_buffer, 0, num_surfaces * sizeof (lm_compute_surface_data_t)}, {bmodel_vertex_buffer, 0, VK_WHOLE_SIZE},
		{emissive_bounce_surfaces_buffer, 0, VK_WHOLE_SIZE}, {emissive_bounce_samples_buffer, 0, VK_WHOLE_SIZE},
		{emissive_world_primitive_surfaces_buffer, 0, emissive_world_as_triangles * sizeof (uint32_t)},
		{emissive_bounce_taps_buffer, 0, VK_WHOLE_SIZE}, {emissive_bounce_direct_buffer, 0, VK_WHOLE_SIZE},
		{emissive_bounce_reflectance_buffer, 0, VK_WHOLE_SIZE}, {emissive_bounce_values_buffer, 0, VK_WHOLE_SIZE},
		{emissive_bounce_filtered_buffer, 0, VK_WHOLE_SIZE}, {emissive_bounce_counters_buffer, 0, 2 * sizeof (uint32_t)}};
	VkWriteDescriptorSet writes[13];
	memset (writes, 0, sizeof (writes));
	for (int i = 0; i < countof (writes); ++i)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : i == 1 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		if (i < 2)
			writes[i].pImageInfo = &images[i];
		else
			writes[i].pBufferInfo = &buffers[i - 2];
	}
	vkUpdateDescriptorSets (vulkan_globals.device, countof (writes), writes, 0, NULL);
	return set;
}

static qboolean R_EnsureTransientEmissiveBounceOutputs (void)
{
	if (emissive_bounce_transient_budget_limited)
		return false;
	uint64_t required_bytes = emissive_bounce_memory.size + emissive_bounce_counters_memory.size;
	for (int i = 0; i < lightmap_count; ++i)
	{
		const struct lightmap_s *const lightmap = &lightmaps[i];
		gltexture_t *const resident[] = {
			lightmap->emissive_bounce_texture,
			lightmap->emissive_bounce_detail_texture,
			lightmap->emissive_bounce_debug_texture,
		};
		for (int texture = 0; texture < countof (resident); ++texture)
			if (resident[texture])
				required_bytes += GL_HeapGetAllocationSize (resident[texture]->allocation);
		gltexture_t *const inputs[] = {lightmap->emissive_transient_texture, lightmap->emissive_transient_detail_texture};
		gltexture_t *const outputs[] = {lightmap->emissive_transient_bounce_texture, lightmap->emissive_transient_bounce_detail_texture};
		for (int detail = 0; detail < 2; ++detail)
			if (inputs[detail])
				required_bytes += outputs[detail] ? GL_HeapGetAllocationSize (outputs[detail]->allocation)
										 : TexMgr_RGBA16FImageMemorySize (inputs[detail]->width, inputs[detail]->height);
	}
	const uint64_t budget_bytes = (uint64_t)EMISSIVE_BOUNCE_MEMORY_BUDGET_MB * 1024 * 1024;
	if (required_bytes > budget_bytes)
	{
		emissive_bounce_transient_budget_limited = true;
		Con_DPrintf (
			"RT emissive bounce: transient output rejected (%" PRIu64 " required bytes, %" PRIu64 " byte budget); using direct-only fallback\n",
			required_bytes, budget_bytes);
		return false;
	}
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		gltexture_t *const inputs[] = {lightmap->emissive_transient_texture, lightmap->emissive_transient_detail_texture};
		gltexture_t **const outputs[] = {&lightmap->emissive_transient_bounce_texture, &lightmap->emissive_transient_bounce_detail_texture};
		for (int detail = 0; detail < 2; ++detail)
			if (inputs[detail] && !*outputs[detail])
			{
				char name[56];
				q_snprintf (name, sizeof (name), detail ? "emissive_transient_bounce_detail_%07i" : "emissive_transient_bounce_%07i", i);
				*outputs[detail] = TexMgr_LoadImage (
					cl.worldmodel, name, inputs[detail]->width, inputs[detail]->height, SRC_RGBA16F, NULL, "", 0,
					TEXPREF_LINEAR | TEXPREF_NOPICMIP);
				emissive_bounce_transient_outputs_initialized = false;
			}
	}
	return true;
}

static void R_AllocateEmissiveBounceDebugLightmaps (void)
{
	if (r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f || emissive_bounce_surfaces_buffer == VK_NULL_HANDLE ||
		emissive_bounce_debug_budget_limited || !cl.worldmodel)
		return;

	uint64_t required_bytes = emissive_bounce_memory.size + emissive_bounce_counters_memory.size;
	for (int i = 0; i < lightmap_count; ++i)
	{
		const struct lightmap_s *const lightmap = &lightmaps[i];
		gltexture_t *const resident[] = {
			lightmap->emissive_bounce_texture,
			lightmap->emissive_bounce_detail_texture,
			lightmap->emissive_transient_bounce_texture,
			lightmap->emissive_transient_bounce_detail_texture,
		};
		for (int texture = 0; texture < countof (resident); ++texture)
			if (resident[texture])
				required_bytes += GL_HeapGetAllocationSize (resident[texture]->allocation);
		if (!lightmap->emissive_texture)
			continue;
		if (lightmap->emissive_bounce_debug_texture)
			required_bytes += GL_HeapGetAllocationSize (lightmap->emissive_bounce_debug_texture->allocation);
		else
			required_bytes += TexMgr_RGBA16FImageMemorySize (lightmap->emissive_texture->width, lightmap->emissive_texture->height);
	}
	const uint64_t budget_bytes = (uint64_t)EMISSIVE_BOUNCE_MEMORY_BUDGET_MB * 1024 * 1024;
	if (required_bytes > budget_bytes)
	{
		emissive_bounce_debug_budget_limited = true;
		Con_DPrintf (
			"RT emissive bounce: bounce-only debug view rejected (%" PRIu64 " required bytes, %" PRIu64 " byte budget)\n", required_bytes,
			budget_bytes);
		return;
	}

	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		if (!lightmap->emissive_texture)
			continue;
		if (!lightmap->emissive_bounce_debug_texture)
		{
			char name[40];
			q_snprintf (name, sizeof (name), "emissive_bounce_debug_%07i", i);
			lightmap->emissive_bounce_debug_texture = TexMgr_LoadImage (
				cl.worldmodel, name, lightmap->emissive_texture->width, lightmap->emissive_texture->height, SRC_RGBA16F, NULL, "", 0,
				TEXPREF_LINEAR | TEXPREF_NOPICMIP);
		}
		if (lightmap->emissive_bounce_debug_descriptor_set == VK_NULL_HANDLE)
			lightmap->emissive_bounce_debug_descriptor_set =
				R_AllocateEmissiveBounceDescriptorSet (lightmap, lightmap->emissive_bounce_debug_texture, i);
	}
	emissive_bounce_debug_pending = true;
	emissive_bounce_debug_ready = false;
}

void R_EmissiveBounceDebugChanged_f (cvar_t *var)
{
	if (CLAMP (0, (int)var->value, 5) == 5)
		R_AllocateEmissiveBounceDebugLightmaps ();
}

void R_EmissiveBounceChanged_f (cvar_t *var)
{
	(void)var;
	if (!cl.worldmodel || r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f || !num_emissive_lights)
		return;
	const uint32_t rays_per_sample = CLAMP (1, (int)r_emissive_rt_bounce_rays.value, EMISSIVE_BOUNCE_MAX_RAYS);
	const uint32_t sample_spacing = R_EmissiveBounceSampleSpacing ();
	const qboolean preserve_transfer = emissive_bounce_ready && emissive_bounce_surfaces_buffer != VK_NULL_HANDLE &&
		rays_per_sample == emissive_bounce_rays_per_sample && sample_spacing == emissive_bounce_sample_spacing;
	if (!preserve_transfer && (emissive_bounce_surfaces_buffer != VK_NULL_HANDLE || emissive_bounce_counters_buffer != VK_NULL_HANDLE))
		GL_WaitForDeviceIdle ();
	if (!preserve_transfer)
		R_DeleteEmissiveBounceResources ();
	if (r_emissive_rt_bounce.value <= 0.0f || r_emissive_rt_bounce_strength.value <= 0.0f)
	{
		R_SetEmissiveBounceInfluence (false);
		emissive_bounce_pending = emissive_bounce_recombine_pending = false;
		emissive_bounce_recorded = false;
		emissive_bounce_debug_pending = emissive_bounce_debug_ready = false;
	}
	else if (preserve_transfer)
	{
		R_SetEmissiveBounceInfluence (true);
		emissive_bounce_pending = false;
		emissive_bounce_recombine_pending = false;
		emissive_cacheable_bounce_epoch = emissive_transient_bounce_epoch = 0;
		emissive_bounce_cacheable_refresh_pending = true;
		emissive_bounce_cacheable_force_full_refresh = true;
		emissive_bounce_transient_refresh_pending = R_TransientEmissiveActive ();
		emissive_bounce_transient_force_full_refresh = emissive_bounce_transient_refresh_pending;
		if (CLAMP (0, (int)r_emissive_rt_debug.value, 11) == 5)
			emissive_bounce_debug_pending = true;
	}
	else
	{
		emissive_coarse_pending = true;
		emissive_detail_pending = num_emissive_logical_tiles > 0 && R_EmissiveDetailAvailable ();
		emissive_detail_building = emissive_detail_ready = false;
		GL_ResetEmissiveDetailTimestamp ();
		R_BuildEmissiveBounceResources ();
		R_SetEmissiveBounceInfluence (emissive_bounce_surfaces_buffer != VK_NULL_HANDLE);
	}
}

qboolean R_EmissiveBounceDebugReady (void)
{
	return emissive_bounce_debug_ready;
}

static void R_EmissiveComputeImageBarrier (cb_context_t *cbx, gltexture_t *texture, VkImageLayout old_layout, VkImageLayout new_layout)
{
	ZEROED_STRUCT (VkImageMemoryBarrier, barrier);
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = texture->image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier (cbx->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void R_PublishEmissiveBounceImage (cb_context_t *cbx, gltexture_t *texture)
{
	ZEROED_STRUCT (VkImageMemoryBarrier, barrier);
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = texture->image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier (
		cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void R_CopyEmissiveBounceRegion (
	cb_context_t *cbx, gltexture_t *input, gltexture_t *output, int x, int y, uint32_t width, uint32_t height)
{
	assert (input->width == output->width && input->height == output->height);
	assert (x >= 0 && y >= 0 && x + width <= input->width && y + height <= input->height);
	VkImageMemoryBarrier barriers[2];
	memset (barriers, 0, sizeof (barriers));
	for (int i = 0; i < 2; ++i)
	{
		barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		barriers[i].dstAccessMask = i ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
		barriers[i].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barriers[i].newLayout = i ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barriers[i].srcQueueFamilyIndex = barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[i].image = i ? output->image : input->image;
		barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barriers[i].subresourceRange.levelCount = barriers[i].subresourceRange.layerCount = 1;
	}
	vkCmdPipelineBarrier (cbx->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, barriers);
	ZEROED_STRUCT (VkImageCopy, copy);
	copy.srcSubresource.aspectMask = copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.srcSubresource.layerCount = copy.dstSubresource.layerCount = 1;
	copy.srcOffset.x = copy.dstOffset.x = x;
	copy.srcOffset.y = copy.dstOffset.y = y;
	copy.extent.width = width;
	copy.extent.height = height;
	copy.extent.depth = 1;
	vkCmdCopyImage (
		cbx->cb, input->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	vkCmdPipelineBarrier (
		cbx->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
		NULL, 2, barriers);
}

static void R_CopyEmissiveBounceImage (cb_context_t *cbx, gltexture_t *input, gltexture_t *output)
{
	R_CopyEmissiveBounceRegion (cbx, input, output, 0, 0, input->width, input->height);
}

static void R_DispatchEmissiveBounce (cb_context_t *cbx, qboolean detail)
{
	if (r_emissive_rt_bounce.value <= 0.0f || r_emissive_rt_bounce_strength.value <= 0.0f)
		return;
	if (emissive_bounce_recombine_pending)
	{
		const vulkan_pipeline_t *const pipeline = &vulkan_globals.emissive_bounce_pipeline;
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
		for (int i = 0; i < lightmap_count; ++i)
		{
			struct lightmap_s *const lightmap = &lightmaps[i];
			gltexture_t *const input = detail ? lightmap->emissive_detail_texture : lightmap->emissive_texture;
			gltexture_t *const output = detail ? lightmap->emissive_bounce_detail_texture : lightmap->emissive_bounce_texture;
			VkDescriptorSet *const descriptor_set =
				detail ? &lightmap->emissive_bounce_output_detail_descriptor_set : &lightmap->emissive_bounce_output_descriptor_set;
			if (!input || !output)
				continue;
			if (*descriptor_set == VK_NULL_HANDLE)
				*descriptor_set = R_AllocateEmissiveBounceDescriptorSet (lightmap, output, i);
			R_CopyEmissiveBounceImage (cbx, input, output);
			vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, descriptor_set, 0, NULL);
			emissive_bounce_push_constants_t constants = {4, 0, detail ? R_EmissiveDetailScale () : 1,
				emissive_bounce_rays_per_sample, CLAMP (0.0f, r_emissive_rt_bounce_strength.value, 4.0f), 1024.0f,
				R_EmissiveBounceReflectanceLift ()};
			constants.extent_x = output->width;
			constants.extent_y = output->height;
			R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
			vkCmdDispatch (cbx->cb, (output->width + 7) / 8, (output->height + 7) / 8, 1);
			R_PublishEmissiveBounceImage (cbx, output);
		}
		if (!detail)
			emissive_cacheable_bounce_epoch = emissive_cacheable_direct_epoch;
		if (detail || !emissive_detail_pending)
		{
			emissive_bounce_recombine_pending = false;
			emissive_bounce_recorded = false;
		}
		else
			emissive_bounce_recorded = true;
		return;
	}
	if ((!detail && (!emissive_bounce_pending || emissive_world_tlas == VK_NULL_HANDLE)) ||
		(detail && !emissive_bounce_recorded && !emissive_bounce_ready))
		return;
	const vulkan_pipeline_t *const pipeline = &vulkan_globals.emissive_bounce_pipeline;
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
	if (!detail)
	{
		GL_BeginEmissiveBounceTimestamp (cbx);
		vkCmdFillBuffer (cbx->cb, emissive_bounce_counters_buffer, 0, 2 * sizeof (uint32_t), 0);
		ZEROED_STRUCT (VkMemoryBarrier, counter_barrier);
		counter_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		counter_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		counter_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier (cbx->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &counter_barrier, 0, NULL, 0, NULL);
	}
	else
		GL_MarkEmissiveBounceTimestamp (cbx, EMISSIVE_BOUNCE_TIMESTAMP_DETAIL_START);
	VkDescriptorSet first_set = VK_NULL_HANDLE;
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		gltexture_t *const input = detail ? lightmap->emissive_detail_texture : lightmap->emissive_texture;
		if (!input)
			continue;
		if (detail)
		{
			gltexture_t *const output = lightmap->emissive_bounce_detail_texture;
			VkDescriptorSet *const descriptor_set = &lightmap->emissive_bounce_output_detail_descriptor_set;
			if (!output)
				continue;
			if (*descriptor_set == VK_NULL_HANDLE)
				*descriptor_set = R_AllocateEmissiveBounceDescriptorSet (lightmap, output, i);
			R_CopyEmissiveBounceImage (cbx, input, output);
			vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, descriptor_set, 0, NULL);
			emissive_bounce_push_constants_t constants = {4, 0, R_EmissiveDetailScale (), emissive_bounce_rays_per_sample,
				CLAMP (0.0f, r_emissive_rt_bounce_strength.value, 4.0f), 1024.0f, R_EmissiveBounceReflectanceLift ()};
			constants.extent_x = output->width;
			constants.extent_y = output->height;
			R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
			vkCmdDispatch (cbx->cb, (output->width + 7) / 8, (output->height + 7) / 8, 1);
			R_PublishEmissiveBounceImage (cbx, output);
			continue;
		}
		VkDescriptorSet *const descriptor_set = &lightmap->emissive_bounce_descriptor_set;
		if (*descriptor_set != VK_NULL_HANDLE)
			R_FreeDescriptorSet (*descriptor_set, &vulkan_globals.emissive_bounce_set_layout);
		*descriptor_set = R_AllocateEmissiveBounceDescriptorSet (lightmap, input, i);
		first_set = first_set == VK_NULL_HANDLE ? *descriptor_set : first_set;
		R_EmissiveComputeImageBarrier (cbx, input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, descriptor_set, 0, NULL);
		emissive_bounce_push_constants_t constants = {0, 0, 1,
			emissive_bounce_rays_per_sample, CLAMP (0.0f, r_emissive_rt_bounce_strength.value, 4.0f), 1024.0f,
			R_EmissiveBounceReflectanceLift ()};
		constants.extent_x = input->width;
		constants.extent_y = input->height;
		R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
		vkCmdDispatch (cbx->cb, (input->width + 7) / 8, (input->height + 7) / 8, 1);
		R_PublishEmissiveBounceImage (cbx, input);
	}
	if (detail)
	{
		GL_MarkEmissiveBounceTimestamp (cbx, EMISSIVE_BOUNCE_TIMESTAMP_DETAIL_END);
		return;
	}
	if (first_set == VK_NULL_HANDLE)
		return;
	ZEROED_STRUCT (VkMemoryBarrier, memory_barrier);
	memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier (cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
	vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, &first_set, 0, NULL);
	ZEROED_STRUCT (VkWriteDescriptorSetAccelerationStructureKHR, tlas_info);
	tlas_info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	tlas_info.accelerationStructureCount = 1;
	tlas_info.pAccelerationStructures = &emissive_world_tlas;
	ZEROED_STRUCT (VkWriteDescriptorSet, tlas_write);
	tlas_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	tlas_write.pNext = &tlas_info;
	tlas_write.dstBinding = 0;
	tlas_write.descriptorCount = 1;
	tlas_write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	vulkan_globals.vk_cmd_push_descriptor_set (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 1, 1, &tlas_write);
	emissive_bounce_push_constants_t constants = {
		1, num_emissive_bounce_samples, emissive_bounce_sample_spacing, emissive_bounce_rays_per_sample,
		CLAMP (0.0f, r_emissive_rt_bounce_strength.value, 4.0f), 1024.0f, R_EmissiveBounceReflectanceLift ()};
	R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
	vkCmdDispatch (cbx->cb, (num_emissive_bounce_samples + 7) / 8, 1, 1);
	GL_MarkEmissiveBounceTimestamp (cbx, EMISSIVE_BOUNCE_TIMESTAMP_TRANSFER);
	vkCmdPipelineBarrier (cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
	constants.mode = 2;
	R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
	vkCmdDispatch (cbx->cb, (num_emissive_bounce_samples + 7) / 8, 1, 1);
	GL_MarkEmissiveBounceTimestamp (cbx, EMISSIVE_BOUNCE_TIMESTAMP_RESOLVE);
	vkCmdPipelineBarrier (cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		if (!lightmap->emissive_texture || lightmap->emissive_bounce_descriptor_set == VK_NULL_HANDLE)
			continue;
		vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, &lightmap->emissive_bounce_descriptor_set, 0, NULL);
		constants.mode = 3;
		constants.offset_x = constants.offset_y = 0;
		constants.extent_x = lightmap->emissive_texture->width;
		constants.extent_y = lightmap->emissive_texture->height;
		R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
		vkCmdDispatch (cbx->cb, (lightmap->emissive_texture->width + 7) / 8, (lightmap->emissive_texture->height + 7) / 8, 1);
	}
	GL_MarkEmissiveBounceTimestamp (cbx, EMISSIVE_BOUNCE_TIMESTAMP_FILTER);
	vkCmdPipelineBarrier (cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		gltexture_t *const input = lightmap->emissive_texture;
		gltexture_t *const output = lightmap->emissive_bounce_texture;
		VkDescriptorSet *const descriptor_set = &lightmap->emissive_bounce_output_descriptor_set;
		if (!input || !output)
			continue;
		if (*descriptor_set == VK_NULL_HANDLE)
			*descriptor_set = R_AllocateEmissiveBounceDescriptorSet (lightmap, output, i);
		R_CopyEmissiveBounceImage (cbx, input, output);
		vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, descriptor_set, 0, NULL);
		constants.mode = 4;
		constants.coordinate_scale = 1;
		constants.offset_x = constants.offset_y = 0;
		constants.extent_x = output->width;
		constants.extent_y = output->height;
		R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
		vkCmdDispatch (cbx->cb, (output->width + 7) / 8, (output->height + 7) / 8, 1);
		R_PublishEmissiveBounceImage (cbx, output);
	}
	GL_MarkEmissiveBounceTimestamp (cbx, EMISSIVE_BOUNCE_TIMESTAMP_COARSE_COMBINE);
	emissive_cacheable_bounce_epoch = emissive_cacheable_direct_epoch;
	emissive_bounce_pending = false;
	emissive_bounce_building = emissive_bounce_recorded = true;
}

static void R_EmissiveBounceSurfaceBounds (const msurface_t *surface, vec3_t mins, vec3_t maxs)
{
	for (int axis = 0; axis < 3; ++axis)
	{
		mins[axis] = FLT_MAX;
		maxs[axis] = -FLT_MAX;
	}
	const float *vertex = surface->polys->verts[0];
	for (int i = 0; i < surface->polys->numverts; ++i, vertex += VERTEXSIZE)
		for (int axis = 0; axis < 3; ++axis)
		{
			mins[axis] = q_min (mins[axis], vertex[axis]);
			maxs[axis] = q_max (maxs[axis], vertex[axis]);
		}
}

static int R_EmissiveBounceDirtySurfaces (
	const emissive_logical_tile_t *dirty_tiles, int num_dirty_tiles, uint32_t **dirty_surfaces)
{
	*dirty_surfaces = NULL;
	if (!dirty_tiles || !num_dirty_tiles || !cl.worldmodel || !emissive_bounce_surface_metadata)
		return 0;
	const int num_world_surfaces = cl.worldmodel->nummodelsurfaces;
	qboolean *const changed = Mem_Alloc (num_world_surfaces * sizeof (*changed));
	qboolean *const affected = Mem_Alloc (num_world_surfaces * sizeof (*affected));
	memset (changed, 0, num_world_surfaces * sizeof (*changed));
	memset (affected, 0, num_world_surfaces * sizeof (*affected));
	msurface_t *const first = &cl.worldmodel->surfaces[cl.worldmodel->firstmodelsurface];
	for (int surface_index = 0; surface_index < num_world_surfaces; ++surface_index)
	{
		emissive_surface_tile_rect_t rect;
		if (!R_TransientEmissiveSurfaceTileRect (&first[surface_index], &rect))
			continue;
		for (int tile = 0; tile < num_dirty_tiles; ++tile)
			if (dirty_tiles[tile].lightmap == rect.lightmap && dirty_tiles[tile].x >= rect.first_x &&
				dirty_tiles[tile].x <= rect.last_x && dirty_tiles[tile].y >= rect.first_y && dirty_tiles[tile].y <= rect.last_y)
			{
				changed[surface_index] = true;
				break;
			}
	}
	vec3_t changed_mins = {FLT_MAX, FLT_MAX, FLT_MAX};
	vec3_t changed_maxs = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
	for (int changed_index = 0; changed_index < num_world_surfaces; ++changed_index)
	{
		if (!changed[changed_index])
			continue;
		const uint32_t changed_global = (uint32_t)(&first[changed_index] - cl.worldmodel->surfaces);
		for (int axis = 0; axis < 3; ++axis)
		{
			changed_mins[axis] = q_min (changed_mins[axis], emissive_bounce_surface_mins[changed_global][axis] - 1024.0f);
			changed_maxs[axis] = q_max (changed_maxs[axis], emissive_bounce_surface_maxs[changed_global][axis] + 1024.0f);
		}
	}
	for (int receiver = 0; receiver < num_world_surfaces; ++receiver)
	{
		const uint32_t global_index = (uint32_t)(&first[receiver] - cl.worldmodel->surfaces);
		if (emissive_bounce_surface_metadata[global_index].bounce_base == UINT32_MAX)
			continue;
		const vec3_t *const receiver_mins = &emissive_bounce_surface_mins[global_index];
		const vec3_t *const receiver_maxs = &emissive_bounce_surface_maxs[global_index];
		if ((*receiver_maxs)[0] < changed_mins[0] || (*receiver_mins)[0] > changed_maxs[0] ||
			(*receiver_maxs)[1] < changed_mins[1] || (*receiver_mins)[1] > changed_maxs[1] ||
			(*receiver_maxs)[2] < changed_mins[2] || (*receiver_mins)[2] > changed_maxs[2])
			continue;
		affected[receiver] = true;
	}
	int count = 0;
	for (int i = 0; i < num_world_surfaces; ++i)
		count += affected[i];
	if (count)
	{
		*dirty_surfaces = Mem_Alloc (count * sizeof (**dirty_surfaces));
		int output = 0;
		for (int i = 0; i < num_world_surfaces; ++i)
			if (affected[i])
				(*dirty_surfaces)[output++] = (uint32_t)(&first[i] - cl.worldmodel->surfaces);
	}
	Mem_Free (affected);
	Mem_Free (changed);
	return count;
}

static void R_RefreshEmissiveBounce (cb_context_t *cbx, qboolean transient)
{
	qboolean *const pending = transient ? &emissive_bounce_transient_refresh_pending : &emissive_bounce_cacheable_refresh_pending;
	if (!*pending || !emissive_bounce_ready || emissive_bounce_surfaces_buffer == VK_NULL_HANDLE ||
		r_emissive_rt_bounce.value <= 0.0f || r_emissive_rt_bounce_strength.value <= 0.0f)
		return;
	if (transient && !R_EnsureTransientEmissiveBounceOutputs ())
	{
		*pending = false;
		return;
	}
	if (transient && R_TransientEmissiveDetailAvailable () && !transient_emissive_detail_ready && !R_TransientEmissiveDetailPublished ())
		return;
	if (!transient && R_EmissiveDetailAvailable () && !emissive_detail_ready)
		return;

	const emissive_logical_tile_t *const dirty_tiles = transient ? transient_emissive_tiles : emissive_radiance_tiles;
	const int num_dirty_tiles = transient ? num_transient_emissive_tiles : num_emissive_radiance_tiles;
	// Thousands of tiny per-surface dispatches and copies cost more than one compact full-atlas resolve.
	// Keep region work for genuinely small changes and use the bounded no-ray fallback for broad lightstyle updates.
	const qboolean partial = num_dirty_tiles > 0 && num_dirty_tiles <= 64 &&
		(transient ? emissive_bounce_transient_outputs_initialized && !emissive_bounce_transient_force_full_refresh
				   : !emissive_bounce_cacheable_force_full_refresh);
	uint32_t *dirty_surfaces = NULL;
	const int num_dirty_surfaces = partial ? R_EmissiveBounceDirtySurfaces (dirty_tiles, num_dirty_tiles, &dirty_surfaces) : 0;
	emissive_bounce_dirty_receiver_surfaces = partial ? num_dirty_surfaces : num_dirty_tiles > 0 ? cl.worldmodel->nummodelsurfaces : 0;
	const vulkan_pipeline_t *const pipeline = &vulkan_globals.emissive_bounce_pipeline;
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
	VkDescriptorSet first_set = VK_NULL_HANDLE;
	qboolean *const input_transitioned = Mem_Alloc (lightmap_count * sizeof (*input_transitioned));
	memset (input_transitioned, 0, lightmap_count * sizeof (*input_transitioned));
	/* direct_values is shared scratch across owners: a partial capture only
	 * rewrites dirty tiles, so after an owner change the untouched slots still
	 * hold the other owner's input. Recapture fully on owner change; later
	 * stages keep their own partial/full choice. */
	const int capture_owner = transient ? 2 : 1;
	const qboolean capture_partial = partial && emissive_bounce_capture_owner == capture_owner;
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		gltexture_t *const input = transient ? lightmap->emissive_transient_texture : lightmap->emissive_texture;
		VkDescriptorSet *const descriptor_set = transient ? &lightmap->emissive_bounce_transient_descriptor_set
														  : &lightmap->emissive_bounce_descriptor_set;
		if (!input)
			continue;
		if (*descriptor_set == VK_NULL_HANDLE)
			*descriptor_set = R_AllocateEmissiveBounceDescriptorSet (lightmap, input, i);
		first_set = first_set == VK_NULL_HANDLE ? *descriptor_set : first_set;
		R_EmissiveComputeImageBarrier (cbx, input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		input_transitioned[i] = true;
		vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, descriptor_set, 0, NULL);
		emissive_bounce_push_constants_t constants = {
			0, 0, 1, emissive_bounce_rays_per_sample, CLAMP (0.0f, r_emissive_rt_bounce_strength.value, 4.0f), 1024.0f,
			R_EmissiveBounceReflectanceLift ()};
		if (capture_partial)
		{
			for (int tile = 0; tile < num_dirty_tiles; ++tile)
				if (dirty_tiles[tile].lightmap == i)
				{
					constants.offset_x = dirty_tiles[tile].x * 8;
					constants.offset_y = dirty_tiles[tile].y * 8;
					constants.extent_x = q_min (8, input->width - constants.offset_x);
					constants.extent_y = q_min (8, input->height - constants.offset_y);
					R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
					vkCmdDispatch (cbx->cb, 1, 1, 1);
				}
		}
		else
		{
			constants.offset_x = constants.offset_y = 0;
			constants.extent_x = input->width;
			constants.extent_y = input->height;
			R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
			vkCmdDispatch (cbx->cb, (input->width + 7) / 8, (input->height + 7) / 8, 1);
		}
	}
	if (first_set == VK_NULL_HANDLE)
	{
		Mem_Free (input_transitioned);
		Mem_Free (dirty_surfaces);
		*pending = false;
		return;
	}
	emissive_bounce_capture_owner = capture_owner;
	ZEROED_STRUCT (VkMemoryBarrier, memory_barrier);
	memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier (
		cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
	vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, &first_set, 0, NULL);
	emissive_bounce_push_constants_t constants = {
		2, num_emissive_bounce_samples, emissive_bounce_sample_spacing, emissive_bounce_rays_per_sample,
		CLAMP (0.0f, r_emissive_rt_bounce_strength.value, 4.0f), 1024.0f, R_EmissiveBounceReflectanceLift ()};
	if (partial)
	{
		for (int dirty = 0; dirty < num_dirty_surfaces; ++dirty)
		{
			const emissive_bounce_surface_t *const meta = &emissive_bounce_surface_metadata[dirty_surfaces[dirty]];
			const uint32_t width = meta->packed_bounce_size & 0xFFFF, height = meta->packed_bounce_size >> 16;
			constants.first = meta->bounce_base;
			constants.count = width * height;
			R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
			vkCmdDispatch (cbx->cb, (constants.count + 7) / 8, 1, 1);
		}
	}
	else
	{
		R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
		vkCmdDispatch (cbx->cb, (num_emissive_bounce_samples + 7) / 8, 1, 1);
	}
	vkCmdPipelineBarrier (
		cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
	if (partial)
	{
		for (int dirty = 0; dirty < num_dirty_surfaces; ++dirty)
		{
			const uint32_t surface_index = dirty_surfaces[dirty];
			const msurface_t *const surface = &cl.worldmodel->surfaces[surface_index];
			struct lightmap_s *const lightmap = &lightmaps[surface->lightmaptexturenum];
			VkDescriptorSet *const descriptor_set = transient ? &lightmap->emissive_bounce_transient_descriptor_set
															  : &lightmap->emissive_bounce_descriptor_set;
			const emissive_bounce_surface_t *const meta = &emissive_bounce_surface_metadata[surface_index];
			const uint32_t width = meta->packed_direct_size & 0xFFFF, height = meta->packed_direct_size >> 16;
			constants.mode = 3;
			constants.coordinate_scale = emissive_bounce_sample_spacing;
			constants.first = 0;
			constants.offset_x = surface->light_s;
			constants.offset_y = surface->light_t;
			constants.extent_x = width;
			constants.extent_y = height;
			vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, descriptor_set, 0, NULL);
			R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
			vkCmdDispatch (cbx->cb, (width + 7) / 8, (height + 7) / 8, 1);
		}
	}
	else for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		gltexture_t *const input = transient ? lightmap->emissive_transient_texture : lightmap->emissive_texture;
		VkDescriptorSet *const descriptor_set = transient ? &lightmap->emissive_bounce_transient_descriptor_set
														  : &lightmap->emissive_bounce_descriptor_set;
		if (!input || *descriptor_set == VK_NULL_HANDLE)
			continue;
		vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, descriptor_set, 0, NULL);
		constants.mode = 3;
		constants.first = 0;
		constants.offset_x = constants.offset_y = 0;
		constants.extent_x = input->width;
		constants.extent_y = input->height;
		R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
		vkCmdDispatch (cbx->cb, (input->width + 7) / 8, (input->height + 7) / 8, 1);
	}
	for (int i = 0; i < lightmap_count; ++i)
		if (input_transitioned[i])
		{
			gltexture_t *const input = transient ? lightmaps[i].emissive_transient_texture : lightmaps[i].emissive_texture;
			R_PublishEmissiveBounceImage (cbx, input);
		}
	Mem_Free (input_transitioned);
	vkCmdPipelineBarrier (
		cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
	for (int detail = 0; detail < 2; ++detail)
	{
		if (partial)
			for (int tile = 0; tile < num_dirty_tiles; ++tile)
			{
				const int lightmap_index = dirty_tiles[tile].lightmap;
				struct lightmap_s *const lightmap = &lightmaps[lightmap_index];
				gltexture_t *const input = transient
					? (detail ? lightmap->emissive_transient_detail_texture : lightmap->emissive_transient_texture)
					: (detail ? lightmap->emissive_detail_texture : lightmap->emissive_texture);
				gltexture_t *const output = transient
					? (detail ? lightmap->emissive_transient_bounce_detail_texture : lightmap->emissive_transient_bounce_texture)
					: (detail ? lightmap->emissive_bounce_detail_texture : lightmap->emissive_bounce_texture);
				if (!input || !output)
					continue;
				const int scale = detail ? R_EmissiveDetailScale () : 1;
				const int x = dirty_tiles[tile].x * 8 * scale, y = dirty_tiles[tile].y * 8 * scale;
				const uint32_t width = q_min (8 * scale, input->width - x), height = q_min (8 * scale, input->height - y);
				R_CopyEmissiveBounceRegion (cbx, input, output, x, y, width, height);
				R_PublishEmissiveBounceImage (cbx, output);
			}
		if (partial)
			for (int dirty = 0; dirty < num_dirty_surfaces; ++dirty)
			{
				const uint32_t surface_index = dirty_surfaces[dirty];
				const msurface_t *const surface = &cl.worldmodel->surfaces[surface_index];
				struct lightmap_s *const lightmap = &lightmaps[surface->lightmaptexturenum];
				gltexture_t *const input = transient
					? (detail ? lightmap->emissive_transient_detail_texture : lightmap->emissive_transient_texture)
					: (detail ? lightmap->emissive_detail_texture : lightmap->emissive_texture);
				gltexture_t *const output = transient
					? (detail ? lightmap->emissive_transient_bounce_detail_texture : lightmap->emissive_transient_bounce_texture)
					: (detail ? lightmap->emissive_bounce_detail_texture : lightmap->emissive_bounce_texture);
				VkDescriptorSet *const descriptor_set = transient
					? (detail ? &lightmap->emissive_bounce_transient_output_detail_descriptor_set
							  : &lightmap->emissive_bounce_transient_output_descriptor_set)
					: (detail ? &lightmap->emissive_bounce_output_detail_descriptor_set : &lightmap->emissive_bounce_output_descriptor_set);
				if (!input || !output)
					continue;
				if (*descriptor_set == VK_NULL_HANDLE)
					*descriptor_set = R_AllocateEmissiveBounceDescriptorSet (lightmap, output, surface->lightmaptexturenum);
				const emissive_bounce_surface_t *const meta = &emissive_bounce_surface_metadata[surface_index];
				const int scale = detail ? R_EmissiveDetailScale () : 1;
				const uint32_t width = (meta->packed_direct_size & 0xFFFF) * scale, height = (meta->packed_direct_size >> 16) * scale;
				const int x = surface->light_s * scale, y = surface->light_t * scale;
				R_CopyEmissiveBounceRegion (cbx, input, output, x, y, width, height);
				vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, descriptor_set, 0, NULL);
				constants.mode = 4;
				constants.coordinate_scale = scale;
				constants.offset_x = x;
				constants.offset_y = y;
				constants.extent_x = width;
				constants.extent_y = height;
				R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
				vkCmdDispatch (cbx->cb, (width + 7) / 8, (height + 7) / 8, 1);
				R_PublishEmissiveBounceImage (cbx, output);
			}
		else
		for (int i = 0; i < lightmap_count; ++i)
		{
			struct lightmap_s *const lightmap = &lightmaps[i];
			gltexture_t *const input = transient
				? (detail ? lightmap->emissive_transient_detail_texture : lightmap->emissive_transient_texture)
				: (detail ? lightmap->emissive_detail_texture : lightmap->emissive_texture);
			gltexture_t *const output = transient
				? (detail ? lightmap->emissive_transient_bounce_detail_texture : lightmap->emissive_transient_bounce_texture)
				: (detail ? lightmap->emissive_bounce_detail_texture : lightmap->emissive_bounce_texture);
			VkDescriptorSet *const descriptor_set = transient
				? (detail ? &lightmap->emissive_bounce_transient_output_detail_descriptor_set
						  : &lightmap->emissive_bounce_transient_output_descriptor_set)
				: (detail ? &lightmap->emissive_bounce_output_detail_descriptor_set : &lightmap->emissive_bounce_output_descriptor_set);
			if (!input || !output)
				continue;
			if (*descriptor_set == VK_NULL_HANDLE)
				*descriptor_set = R_AllocateEmissiveBounceDescriptorSet (lightmap, output, i);
			R_CopyEmissiveBounceImage (cbx, input, output);
			vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, descriptor_set, 0, NULL);
			constants.mode = 4;
			constants.coordinate_scale = detail ? R_EmissiveDetailScale () : 1;
			constants.offset_x = constants.offset_y = 0;
			constants.extent_x = output->width;
			constants.extent_y = output->height;
			R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
			vkCmdDispatch (cbx->cb, (output->width + 7) / 8, (output->height + 7) / 8, 1);
			R_PublishEmissiveBounceImage (cbx, output);
		}
	}
	Mem_Free (dirty_surfaces);
	if (transient)
	{
		emissive_transient_bounce_epoch = emissive_transient_direct_epoch;
		emissive_bounce_transient_outputs_initialized = true;
		emissive_bounce_transient_force_full_refresh = false;
	}
	else
	{
		emissive_cacheable_bounce_epoch = emissive_cacheable_direct_epoch;
		emissive_bounce_cacheable_force_full_refresh = false;
	}
	*pending = false;
	++emissive_bounce_no_ray_refreshes;
	if (CLAMP (0, (int)r_emissive_rt_debug.value, 11) == 5)
		emissive_bounce_debug_pending = true;
}

static void R_RefreshEmissiveBounceLayers (cb_context_t *cbx)
{
	if (!emissive_bounce_cacheable_refresh_pending && !emissive_bounce_transient_refresh_pending)
		return;
	const double start = Sys_DoubleTime ();
	GL_BeginEmissiveBounceRefreshTimestamp (cbx);
	R_RefreshEmissiveBounce (cbx, false);
	R_RefreshEmissiveBounce (cbx, true);
	GL_EndEmissiveBounceRefreshTimestamp (cbx);
	emissive_bounce_refresh_cpu_time_us = (uint32_t)((Sys_DoubleTime () - start) * 1000000.0);
}

static void R_DispatchEmissiveBounceDebug (cb_context_t *cbx)
{
	if (!emissive_bounce_debug_pending || (!emissive_bounce_recorded && !emissive_bounce_ready))
		return;
	const vulkan_pipeline_t *const pipeline = &vulkan_globals.emissive_bounce_pipeline;
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		gltexture_t *const texture = lightmap->emissive_bounce_debug_texture;
		if (!texture || lightmap->emissive_bounce_debug_descriptor_set == VK_NULL_HANDLE)
			continue;
		R_EmissiveComputeImageBarrier (cbx, texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		vkCmdBindDescriptorSets (
			cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, &lightmap->emissive_bounce_debug_descriptor_set, 0, NULL);
		emissive_bounce_push_constants_t constants = {
			5, 0, 1, emissive_bounce_rays_per_sample, CLAMP (0.0f, r_emissive_rt_bounce_strength.value, 4.0f), 1024.0f,
			R_EmissiveBounceReflectanceLift ()};
		constants.offset_x = constants.offset_y = 0;
		constants.extent_x = texture->width;
		constants.extent_y = texture->height;
		R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (constants), &constants);
		vkCmdDispatch (cbx->cb, (texture->width + 7) / 8, (texture->height + 7) / 8, 1);
		R_PublishEmissiveBounceImage (cbx, texture);
	}
	emissive_bounce_debug_pending = false;
	emissive_bounce_debug_ready = true;
}

/*
==================
R_EmissiveLightStats
==================
*/
void R_EmissiveLightStats (int *count, uint64_t *allocated_bytes, qboolean *pending)
{
	*count = num_emissive_lights;
	*allocated_bytes = emissive_lights_buffer_memory.size + emissive_light_seeds_buffer_memory.size;
	*pending = emissive_coarse_pending;
}

void R_EmissiveRadianceStats (
	int *groups, int *dirty_tiles, int *source_links, int *tile_groups, int *max_groups_per_tile, uint64_t *cpu_bytes, uint64_t *gpu_bytes,
	uint32_t *cpu_time_us, qboolean *visibility_available, qboolean *pending)
{
	*groups = num_emissive_modulation_groups;
	*dirty_tiles = num_emissive_radiance_tiles;
	*source_links = num_emissive_radiance_source_links;
	*tile_groups = num_emissive_radiance_tile_groups;
	*max_groups_per_tile = max_emissive_radiance_groups_per_tile;
	*cpu_bytes = (uint64_t)num_emissive_lights * (sizeof (*emissive_light_styles) + sizeof (*emissive_light_modulations)) +
				 (uint64_t)num_emissive_logical_tiles * sizeof (*emissive_radiance_tiles);
	*gpu_bytes = emissive_modulations_buffer_memory.size + emissive_visibility_buffer_memory.size + emissive_radiance_tiles_buffer_memory.size;
	*cpu_time_us = emissive_radiance_cpu_time_us;
	*visibility_available = emissive_visibility_available;
	*pending = emissive_radiance_coarse_pending || emissive_radiance_detail_pending || emissive_modulations_pending;
}

void R_UpdateEmissiveLightstyles (void)
{
	if (r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f || !num_emissive_lights || !emissive_light_styles)
		return;
	const double start_time = Sys_DoubleTime ();
	qboolean	 changed_styles[MAX_LIGHTSTYLES];
	memset (changed_styles, 0, sizeof (changed_styles));
	qboolean changed = false;
	for (int i = 0; i < num_emissive_lights; ++i)
	{
		const byte style = emissive_light_styles[i];
		if (style == 255)
			continue;
		const float modulation = (float)d_lightstylevalue[style] / 256.0f;
		if (modulation == emissive_light_modulations[i])
			continue;
		emissive_light_modulations[i] = modulation;
		changed_styles[style] = true;
		changed = true;
	}
	if (!changed && !emissive_radiance_force_all_styled_tiles)
	{
		emissive_radiance_cpu_time_us = 0;
		return;
	}
	if (changed)
		R_InvalidateEmissiveBrushReceiverRadiance (changed_styles);
	if (!changed && !emissive_detail_ready)
	{
		emissive_radiance_cpu_time_us = 0;
		return;
	}

	num_emissive_radiance_tiles = 0;
	num_emissive_radiance_source_links = 0;
	num_emissive_radiance_tile_groups = 0;
	max_emissive_radiance_groups_per_tile = 0;
	for (int tile_index = 0; tile_index < num_emissive_logical_tiles; ++tile_index)
	{
		const emissive_logical_tile_t *const tile = &emissive_logical_tiles[tile_index];
		qboolean							 dirty = false;
		for (uint32_t source = 0; source < tile->num_sources; ++source)
		{
			const uint32_t light_index = emissive_logical_tile_sources[tile->first_source + source];
			const byte	   style = emissive_light_styles[light_index];
			if (style != 255 && (emissive_radiance_force_all_styled_tiles || changed_styles[style]))
			{
				dirty = true;
				break;
			}
		}
		if (!dirty)
			continue;
		qboolean tile_styles[MAX_LIGHTSTYLES];
		memset (tile_styles, 0, sizeof (tile_styles));
		int tile_groups = 0;
		for (uint32_t source = 0; source < tile->num_sources; ++source)
		{
			const uint32_t light_index = emissive_logical_tile_sources[tile->first_source + source];
			const byte	   style = emissive_light_styles[light_index];
			if (style != 255 && !tile_styles[style])
			{
				tile_styles[style] = true;
				++tile_groups;
			}
		}
		emissive_radiance_tiles[num_emissive_radiance_tiles++] = *tile;
		num_emissive_radiance_source_links += tile->num_sources;
		num_emissive_radiance_tile_groups += tile_groups;
		max_emissive_radiance_groups_per_tile = q_max (max_emissive_radiance_groups_per_tile, tile_groups);
	}

	emissive_modulations_pending |= changed;
	emissive_radiance_coarse_pending = changed && num_emissive_radiance_tiles > 0;
	if (num_emissive_radiance_tiles > 0 && emissive_visibility_available && emissive_detail_ready)
	{
		emissive_radiance_detail_pending = true;
		emissive_radiance_force_all_styled_tiles = false;
	}
	else if (num_emissive_radiance_tiles > 0 && emissive_visibility_available)
		emissive_radiance_force_all_styled_tiles = true;
	else if (changed && num_emissive_radiance_tiles > 0)
	{
		/* Styled detail is not admitted without retained visibility; the coarse atlas remains authoritative. */
		emissive_radiance_detail_pending = false;
		emissive_radiance_force_all_styled_tiles = false;
		emissive_detail_pending = emissive_detail_building = emissive_detail_ready = false;
		transient_emissive_detail_ready = false;
		transient_emissive_detail_published_generation = 0;
	}
	emissive_radiance_cpu_time_us = (uint32_t)((Sys_DoubleTime () - start_time) * 1000000.0);
}

void R_EmissiveBandlimitStats (
	qboolean *active, qboolean *budget_limited, uint64_t *logical_bytes, uint64_t *allocated_bytes, uint64_t *peak_bytes)
{
	*active = emissive_bandlimit_active;
	*budget_limited = emissive_bandlimit_budget_limited;
	*logical_bytes = emissive_bandlimit_logical_bytes;
	*allocated_bytes = emissive_bandlimit_allocated_bytes;
	*peak_bytes = emissive_bandlimit_peak_bytes;
}

qboolean R_EmissiveBandlimitActive (void)
{
	return emissive_bandlimit_active;
}

void R_EmissiveOccludersChanged_f (cvar_t *var)
{
	(void)var;
	emissive_occluder_state_valid = false;
	emissive_occluder_next_update_time = 0.0;
	emissive_live_as_dirty = true;
	if (!cl.worldmodel)
		return;
	R_InvalidateEmissiveBrushReceiverSources ();
	emissive_detail_pending = num_emissive_logical_tiles > 0 && R_EmissiveDetailAvailable ();
	emissive_detail_building = emissive_detail_ready = false;
	transient_emissive_detail_pending = num_transient_emissive_tiles > 0 && R_TransientEmissiveDetailAvailable ();
	R_InvalidateTransientEmissiveDetail ();
	if (CLAMP (0, (int)r_emissive_rt_occluders.value, 2) > 0 && r_emissive_rt.value > 0.0f)
		GL_RequestAccelerationStructure (RT_AS_CONSUMER_TRANSIENT_EMISSIVES);
}

static void R_UpdateEmissiveLightmaps (cb_context_t *cbx, qboolean detail)
{
	VkAccelerationStructureKHR direct_tlas = R_EmissiveDirectAccelerationStructure ();
	qboolean *const pending = detail ? &emissive_detail_pending : &emissive_coarse_pending;
	if (!detail && emissive_bounce_debug_pending && r_emissive_rt.value > 0.0f && gl_fullbrights.value > 0.0f)
		R_DispatchEmissiveBounceDebug (cbx);
	if (!*pending || r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f)
		return;
	if (detail && direct_tlas == VK_NULL_HANDLE)
		return;

	const qboolean partial_detail = detail && emissive_occluder_partial_detail_refresh && emissive_detail_ready &&
		num_emissive_occluder_dirty_tiles > 0;
	const qboolean bandlimited = detail && emissive_bandlimit_active;
	const vulkan_pipeline_t *const pipeline = detail
		? (bandlimited ? &vulkan_globals.emissive_bandlimit_detail_pipeline : &vulkan_globals.emissive_detail_pipeline)
		: &vulkan_globals.emissive_coarse_pipeline;
	R_BeginDebugUtilsLabel (cbx, detail ? "Update Detail Emissive Lightmaps" : "Update Coarse Emissive Lightmaps");
	if (detail)
		GL_BeginEmissiveDetailTimestamp (cbx);
	else
		GL_BeginEmissiveCoarseTimestamp (cbx);
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
	if (detail)
	{
		ZEROED_STRUCT (VkWriteDescriptorSetAccelerationStructureKHR, tlas_info);
		tlas_info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
		tlas_info.accelerationStructureCount = 1;
		tlas_info.pAccelerationStructures = &direct_tlas;

		ZEROED_STRUCT (VkWriteDescriptorSet, tlas_write);
		tlas_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		tlas_write.pNext = &tlas_info;
		tlas_write.dstBinding = 0;
		tlas_write.descriptorCount = 1;
		tlas_write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

		vulkan_globals.vk_cmd_push_descriptor_set (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 1, 1, &tlas_write);
		if (emissive_visibility_available && !partial_detail)
		{
			vkCmdFillBuffer (cbx->cb, emissive_visibility_buffer, 0, VK_WHOLE_SIZE, 0);
			ZEROED_STRUCT (VkMemoryBarrier, visibility_barrier);
			visibility_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			visibility_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			visibility_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			vkCmdPipelineBarrier (cbx->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &visibility_barrier, 0, NULL, 0, NULL);
		}
		else if (emissive_visibility_available)
		{
			ZEROED_STRUCT (VkMemoryBarrier, visibility_barrier);
			visibility_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			visibility_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			visibility_barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			vkCmdPipelineBarrier (
				cbx->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &visibility_barrier, 0, NULL, 0, NULL);
		}
		if (partial_detail)
		{
			R_UpdateTransientEmissiveBuffer (cbx->cb, emissive_occluder_tiles_buffer, emissive_occluder_tiles,
				num_emissive_occluder_dirty_tiles * sizeof (*emissive_occluder_tiles));
			ZEROED_STRUCT (VkMemoryBarrier, tile_barrier);
			tile_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			tile_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			tile_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			vkCmdPipelineBarrier (
				cbx->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &tile_barrier, 0, NULL, 0, NULL);
		}
	}
	emissive_compute_push_constants_t push_constants = {
		num_emissive_lights, 0, detail && !emissive_visibility_available ? EMISSIVE_PUBLICATION_NO_VISIBILITY : EMISSIVE_PUBLICATION_UPDATE,
		detail ? R_EmissiveDetailScale () : 1, R_EmissiveOccluderMask ()};
	if (!detail)
		R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (push_constants), &push_constants);

	int logical_tile = 0;
	int dirty_tile = 0;
	emissive_detail_recorded_tiles = partial_detail ? num_emissive_occluder_dirty_tiles : num_emissive_logical_tiles;
	emissive_detail_recorded_dispatches = 0;
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		int first_dirty_tile = dirty_tile;
		int num_dirty_tiles = 0;
		if (partial_detail)
		{
			while (first_dirty_tile < num_emissive_occluder_dirty_tiles && emissive_occluder_tiles[first_dirty_tile].lightmap < i)
				++first_dirty_tile;
			dirty_tile = first_dirty_tile;
			while (dirty_tile < num_emissive_occluder_dirty_tiles && emissive_occluder_tiles[dirty_tile].lightmap == i)
				++dirty_tile;
			num_dirty_tiles = dirty_tile - first_dirty_tile;
			if (!num_dirty_tiles)
				continue;
		}
		const VkDescriptorSet descriptor_set = detail
			? (partial_detail ? emissive_occluder_detail_descriptor_sets[i]
							  : (bandlimited ? lightmap->emissive_bandlimit_detail_descriptor_set : lightmap->emissive_detail_descriptor_set))
			: lightmap->emissive_coarse_descriptor_set;
		const gltexture_t *const texture = detail
			? lightmap->emissive_detail_texture
			: lightmap->emissive_texture;
		if (descriptor_set == VK_NULL_HANDLE)
			continue;

		VkImageMemoryBarrier barrier;
		memset (&barrier, 0, sizeof (barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = detail && !partial_detail ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = texture->image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier (
			cbx->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, detail && !partial_detail ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
			NULL, 1, &barrier);
		if (detail && !partial_detail)
		{
			ZEROED_STRUCT (VkClearColorValue, clear_color);
			vkCmdClearColorImage (cbx->cb, texture->image, VK_IMAGE_LAYOUT_GENERAL, &clear_color, 1, &barrier.subresourceRange);
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			vkCmdPipelineBarrier (
				cbx->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
		}

		vkCmdBindDescriptorSets (
			cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, &descriptor_set, 0, NULL);
		if (detail)
		{
			if (partial_detail)
			{
				push_constants.first_tile = first_dirty_tile;
				R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (push_constants), &push_constants);
				vkCmdDispatch (cbx->cb, R_EmissiveDetailScale (), R_EmissiveDetailScale (), num_dirty_tiles);
				++emissive_detail_recorded_dispatches;
			}
			else
			{
				while (logical_tile < num_emissive_logical_tiles && emissive_logical_tiles[logical_tile].lightmap < i)
					++logical_tile;
				const int first_tile = logical_tile;
				while (logical_tile < num_emissive_logical_tiles && emissive_logical_tiles[logical_tile].lightmap == i)
					++logical_tile;
				const int num_tiles = logical_tile - first_tile;
				if (num_tiles)
				{
					push_constants.first_tile = first_tile;
					R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (push_constants), &push_constants);
					vkCmdDispatch (cbx->cb, R_EmissiveDetailScale (), R_EmissiveDetailScale (), num_tiles);
					++emissive_detail_recorded_dispatches;
				}
			}
		}
		else
			vkCmdDispatch (cbx->cb, (texture->width + 7) / 8, (texture->height + 7) / 8, 1);

		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | (detail ? VK_ACCESS_TRANSFER_WRITE_BIT : 0);
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		const VkPipelineStageFlags destination_stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		vkCmdPipelineBarrier (
			cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | (detail ? VK_PIPELINE_STAGE_TRANSFER_BIT : 0),
			destination_stages, 0, 0, NULL, 0, NULL, 1, &barrier);
	}
	R_DispatchEmissiveBounce (cbx, detail);
	if (!detail)
		R_DispatchEmissiveBounceDebug (cbx);

	*pending = false;
	if (detail)
	{
		emissive_detail_building = true;
		emissive_occluder_partial_detail_refresh = false;
		emissive_occluder_full_detail_refresh = false;
		GL_EndEmissiveDetailTimestamp (cbx);
	}
	else
		GL_EndEmissiveCoarseTimestamp (cbx);
	R_EndDebugUtilsLabel (cbx);
}

static void R_UpdateTransientEmissiveBuffer (VkCommandBuffer cb, VkBuffer buffer, const void *data, size_t size)
{
	const byte *bytes = data;
	VkDeviceSize offset = 0;
	while (size)
	{
		const size_t chunk = q_min (size, 65536u);
		vkCmdUpdateBuffer (cb, buffer, offset, chunk, bytes);
		bytes += chunk;
		offset += chunk;
		size -= chunk;
	}
}

static void R_DispatchEmissiveRadianceTiles (cb_context_t *cbx, qboolean detail, qboolean overlay)
{
	const qboolean bandlimited = detail && emissive_bandlimit_active;
	const vulkan_pipeline_t *const pipeline =
		detail ? (bandlimited ? (overlay ? &vulkan_globals.emissive_bandlimit_radiance_overlay_pipeline
										 : &vulkan_globals.emissive_bandlimit_radiance_pipeline)
							 : (overlay ? &vulkan_globals.emissive_radiance_overlay_detail_pipeline : &vulkan_globals.emissive_radiance_detail_pipeline))
			   : (overlay ? &vulkan_globals.emissive_radiance_overlay_pipeline : &vulkan_globals.emissive_radiance_pipeline);
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
	emissive_compute_push_constants_t push_constants = {
		num_emissive_lights, 0, EMISSIVE_PUBLICATION_UPDATE, detail ? R_EmissiveDetailScale () : 1, R_EmissiveOccluderMask ()};
	int								  logical_tile = 0;
	for (int i = 0; i < lightmap_count; ++i)
	{
		while (logical_tile < num_emissive_radiance_tiles && emissive_radiance_tiles[logical_tile].lightmap < i)
			++logical_tile;
		const int first_tile = logical_tile;
		while (logical_tile < num_emissive_radiance_tiles && emissive_radiance_tiles[logical_tile].lightmap == i)
			++logical_tile;
		const int num_tiles = logical_tile - first_tile;
		if (!num_tiles)
			continue;
		struct lightmap_s *const lightmap = &lightmaps[i];
		const VkDescriptorSet	 descriptor_set =
			detail ? (bandlimited ? (overlay ? lightmap->emissive_radiance_overlay_detail_descriptor_set
											 : lightmap->emissive_bandlimit_radiance_descriptor_set)
								 : (overlay ? lightmap->emissive_radiance_overlay_detail_descriptor_set : lightmap->emissive_radiance_detail_descriptor_set))
				   : (overlay ? lightmap->emissive_radiance_overlay_descriptor_set : lightmap->emissive_radiance_descriptor_set);
		gltexture_t *const texture = detail ? (overlay ? lightmap->emissive_transient_detail_texture : lightmap->emissive_detail_texture)
											: (overlay ? lightmap->emissive_transient_texture : lightmap->emissive_texture);
		if (descriptor_set == VK_NULL_HANDLE || !texture)
			continue;

		ZEROED_STRUCT (VkImageMemoryBarrier, barrier);
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = texture->image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier (cbx->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
		vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, &descriptor_set, 0, NULL);
		push_constants.first_tile = first_tile;
		R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (push_constants), &push_constants);
		vkCmdDispatch (cbx->cb, detail ? R_EmissiveDetailScale () : 1, detail ? R_EmissiveDetailScale () : 1, num_tiles);
		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier (cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
	}
}

static void R_UpdateEmissiveRadiance (cb_context_t *cbx)
{
	if ((!emissive_modulations_pending && !emissive_radiance_coarse_pending && !emissive_radiance_detail_pending) || r_emissive_rt.value <= 0.0f ||
		gl_fullbrights.value <= 0.0f)
		return;
	R_BeginDebugUtilsLabel (cbx, "Update Emissive Radiance");
	if (emissive_modulations_pending)
		R_UpdateTransientEmissiveBuffer (
			cbx->cb, emissive_modulations_buffer, emissive_light_modulations, num_emissive_lights * sizeof (*emissive_light_modulations));
	if (num_emissive_radiance_tiles)
		R_UpdateTransientEmissiveBuffer (
			cbx->cb, emissive_radiance_tiles_buffer, emissive_radiance_tiles, num_emissive_radiance_tiles * sizeof (*emissive_radiance_tiles));
	ZEROED_STRUCT (VkMemoryBarrier, memory_barrier);
	memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier (cbx->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
	emissive_modulations_pending = false;

	if (emissive_radiance_coarse_pending || emissive_radiance_detail_pending)
	{
		if (++emissive_cacheable_direct_epoch == 0)
			++emissive_cacheable_direct_epoch;
		emissive_cacheable_bounce_epoch = 0;
		emissive_bounce_cacheable_refresh_pending = emissive_bounce_ready;
		if (transient_emissive_initialized)
		{
			if (++emissive_transient_direct_epoch == 0)
				++emissive_transient_direct_epoch;
			emissive_transient_bounce_epoch = 0;
			emissive_bounce_transient_refresh_pending = emissive_bounce_ready;
			emissive_bounce_transient_force_full_refresh = true;
		}
		GL_BeginEmissiveRadianceTimestamp (cbx);
		if (emissive_radiance_coarse_pending)
		{
			if (transient_emissive_initialized)
				R_DispatchEmissiveRadianceTiles (cbx, false, true);
			R_DispatchEmissiveRadianceTiles (cbx, false, false);
			emissive_radiance_coarse_pending = false;
		}
		if (emissive_radiance_detail_pending && emissive_detail_ready)
		{
			if (transient_emissive_initialized && transient_emissive_detail_cache_copied)
				R_DispatchEmissiveRadianceTiles (cbx, true, true);
			R_DispatchEmissiveRadianceTiles (cbx, true, false);
			emissive_radiance_detail_pending = false;
		}
		GL_EndEmissiveRadianceTimestamp (cbx);
		if (!emissive_radiance_logged)
		{
			Con_DPrintf (
				"RT emissives: scheduled no-ray radiance resolve for %d tile%s and %d source link%s\n", num_emissive_radiance_tiles,
				num_emissive_radiance_tiles == 1 ? "" : "s", num_emissive_radiance_source_links, num_emissive_radiance_source_links == 1 ? "" : "s");
			emissive_radiance_logged = true;
		}
	}
	R_EndDebugUtilsLabel (cbx);
}

static void R_InitializeTransientEmissiveImages (cb_context_t *cbx, qboolean detail_only)
{
	for (int i = 0; i < lightmap_count; ++i)
	{
		struct lightmap_s *const lightmap = &lightmaps[i];
		gltexture_t *outputs[2] = {lightmap->emissive_transient_texture, lightmap->emissive_transient_detail_texture};
		gltexture_t *inputs[2] = {lightmap->emissive_texture, lightmap->emissive_detail_texture};
		for (int detail = detail_only ? 1 : 0; detail < 2; ++detail)
		{
			gltexture_t *const output = outputs[detail];
			gltexture_t *const input = inputs[detail];
			if (!output || !input)
				continue;
			VkImageMemoryBarrier barriers[2];
			memset (barriers, 0, sizeof (barriers));
			for (int barrier = 0; barrier < 2; ++barrier)
			{
				barriers[barrier].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				barriers[barrier].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
				barriers[barrier].dstAccessMask = barrier ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
				barriers[barrier].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				barriers[barrier].newLayout = barrier ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				barriers[barrier].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barriers[barrier].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barriers[barrier].image = barrier ? output->image : input->image;
				barriers[barrier].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				barriers[barrier].subresourceRange.levelCount = 1;
				barriers[barrier].subresourceRange.layerCount = 1;
			}
			vkCmdPipelineBarrier (
				cbx->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, barriers);
			if ((!detail && num_emissive_lights) || (detail && R_EmissiveDetailReady ()))
			{
				VkImageCopy copy;
				memset (&copy, 0, sizeof (copy));
				copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				copy.srcSubresource.layerCount = 1;
				copy.dstSubresource = copy.srcSubresource;
				copy.extent.width = output->width;
				copy.extent.height = output->height;
				copy.extent.depth = 1;
				vkCmdCopyImage (
					cbx->cb, input->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			}
			else
			{
				ZEROED_STRUCT (VkClearColorValue, clear_color);
				vkCmdClearColorImage (
					cbx->cb, output->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1, &barriers[1].subresourceRange);
			}
			for (int barrier = 0; barrier < 2; ++barrier)
			{
				barriers[barrier].srcAccessMask = barrier ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
				barriers[barrier].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				barriers[barrier].oldLayout = barrier ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				barriers[barrier].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			}
			vkCmdPipelineBarrier (
				cbx->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
				NULL, 2, barriers);
		}
	}
	transient_emissive_initialized = true;
	transient_emissive_detail_cache_copied = !num_emissive_lights || R_EmissiveDetailReady ();
}

/* Returns false when any required lightmap was skipped (missing descriptor set or
 * texture); publication must only bless fully recorded generations. */
static qboolean R_DispatchTransientEmissiveTiles (cb_context_t *cbx, qboolean detail, qboolean invalidate)
{
	VkAccelerationStructureKHR direct_tlas = R_EmissiveDirectAccelerationStructure ();
	const qboolean bandlimited = detail && emissive_bandlimit_active;
	const vulkan_pipeline_t *const pipeline = detail
		? (bandlimited ? &vulkan_globals.emissive_bandlimit_transient_pipeline : &vulkan_globals.emissive_transient_detail_pipeline)
		: &vulkan_globals.emissive_transient_pipeline;
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
	if (detail)
	{
		if (direct_tlas == VK_NULL_HANDLE)
			return false;
		ZEROED_STRUCT (VkWriteDescriptorSetAccelerationStructureKHR, tlas_info);
		tlas_info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
		tlas_info.accelerationStructureCount = 1;
		tlas_info.pAccelerationStructures = &direct_tlas;
		ZEROED_STRUCT (VkWriteDescriptorSet, tlas_write);
		tlas_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		tlas_write.pNext = &tlas_info;
		tlas_write.dstBinding = 0;
		tlas_write.descriptorCount = 1;
		tlas_write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		vulkan_globals.vk_cmd_push_descriptor_set (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 1, 1, &tlas_write);
	}

	emissive_compute_push_constants_t push_constants = {
		num_transient_emissive_lights, 0, invalidate ? EMISSIVE_PUBLICATION_INVALIDATE : EMISSIVE_PUBLICATION_UPDATE,
		detail ? R_EmissiveDetailScale () : 1, R_EmissiveOccluderMask ()};
	qboolean recorded = true;
	int logical_tile = 0;
	for (int i = 0; i < lightmap_count; ++i)
	{
		while (logical_tile < num_transient_emissive_tiles && transient_emissive_tiles[logical_tile].lightmap < i)
			++logical_tile;
		const int first_tile = logical_tile;
		while (logical_tile < num_transient_emissive_tiles && transient_emissive_tiles[logical_tile].lightmap == i)
			++logical_tile;
		const int num_tiles = logical_tile - first_tile;
		if (!num_tiles)
			continue;
		struct lightmap_s *const lightmap = &lightmaps[i];
		const VkDescriptorSet descriptor_set = detail
			? (bandlimited ? lightmap->emissive_bandlimit_transient_descriptor_set : lightmap->emissive_transient_detail_descriptor_set)
			: lightmap->emissive_transient_descriptor_set;
		gltexture_t *const texture = detail
			? lightmap->emissive_transient_detail_texture
			: lightmap->emissive_transient_texture;
		if (descriptor_set == VK_NULL_HANDLE || !texture)
		{
			recorded = false;
			continue;
		}
		VkImageMemoryBarrier barrier;
		memset (&barrier, 0, sizeof (barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = texture->image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier (
			cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
		vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, &descriptor_set, 0, NULL);
		push_constants.first_tile = first_tile;
		R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (push_constants), &push_constants);
		vkCmdDispatch (cbx->cb, detail ? R_EmissiveDetailScale () : 1, detail ? R_EmissiveDetailScale () : 1, num_tiles);
		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier (
			cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
	}
	return recorded;
}

static void R_UpdateTransientEmissiveLightmaps (cb_context_t *cbx)
{
	if ((!transient_emissive_pending && !transient_emissive_detail_pending) || r_emissive_rt.value <= 0.0f || gl_fullbrights.value <= 0.0f)
		return;
	R_BeginDebugUtilsLabel (cbx, "Update Transient Emissive Lightmaps");
	GL_BeginEmissiveTransientTimestamp (cbx);
	if (!transient_emissive_initialized)
		R_InitializeTransientEmissiveImages (cbx, false);
	else if (!transient_emissive_detail_cache_copied && R_EmissiveDetailReady ())
		R_InitializeTransientEmissiveImages (cbx, true);
	if (transient_emissive_occluder_tiles_pending)
	{
		transient_emissive_occluder_tiles_pending = false;
		R_RebuildTransientEmissiveOccluderTiles (cbx);
	}
	const qboolean coarse_publication = transient_emissive_pending;
	qboolean detail_recorded = false;
	if (coarse_publication)
	{
		if (num_transient_emissive_lights)
		{
			R_UpdateTransientEmissiveBuffer (
				cbx->cb, transient_emissive_lights_buffer, transient_emissive_lights,
				num_transient_emissive_lights * sizeof (*transient_emissive_lights));
			if (emissive_bandlimit_active)
				R_UpdateTransientEmissiveBuffer (
					cbx->cb, transient_emissive_light_seeds_buffer, transient_emissive_light_seeds,
					num_transient_emissive_lights * sizeof (*transient_emissive_light_seeds));
		}
		R_UpdateTransientEmissiveBuffer (
			cbx->cb, transient_emissive_tiles_buffer, transient_emissive_tiles,
			num_transient_emissive_tiles * sizeof (*transient_emissive_tiles));
		if (num_transient_emissive_tile_sources)
			R_UpdateTransientEmissiveBuffer (
				cbx->cb, transient_emissive_tile_sources_buffer, transient_emissive_tile_sources,
				num_transient_emissive_tile_sources * sizeof (*transient_emissive_tile_sources));
		ZEROED_STRUCT (VkMemoryBarrier, memory_barrier);
		memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier (
			cbx->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
		const qboolean coarse_ok = R_DispatchTransientEmissiveTiles (cbx, false, false);
		if (vulkan_globals.ray_query && R_TransientEmissiveDetailAvailable ())
		{
			/* The TLAS build precedes lightmap updates, so the current AS is already
			 * fresh: build this generation's detail inline instead of invalidating it
			 * and requiring a motion-free frame to make progress. Fall back to
			 * invalidate-and-defer only when detail cannot build yet. Publish only
			 * fully recorded work; skipped tiles stay coarse until a later change
			 * rebuilds them, instead of passing off partial output. */
			if (R_EmissiveDirectAccelerationStructure () != VK_NULL_HANDLE &&
				(!num_emissive_lights || transient_emissive_detail_cache_copied))
			{
				if (coarse_ok && R_DispatchTransientEmissiveTiles (cbx, true, false))
				{
					transient_emissive_detail_published_generation = transient_emissive_generation;
					detail_recorded = true;
				}
			}
			else
			{
				R_DispatchTransientEmissiveTiles (cbx, true, true);
				transient_emissive_detail_pending = true;
			}
		}
		transient_emissive_pending = false;
	}
	else if (transient_emissive_detail_pending && R_EmissiveDirectAccelerationStructure () != VK_NULL_HANDLE &&
		(!num_emissive_lights || transient_emissive_detail_cache_copied))
	{
		if (R_DispatchTransientEmissiveTiles (cbx, true, false))
		{
			transient_emissive_detail_published_generation = transient_emissive_generation;
			detail_recorded = true;
		}
		/* Pending clears even on partial recording: skipped tiles stay coarse (the
		 * safe fallback) until a later source change rebuilds them. */
		transient_emissive_detail_pending = false;
	}
	GL_EndEmissiveTransientTimestamp (cbx, detail_recorded ? transient_emissive_generation : 0);
	R_EndDebugUtilsLabel (cbx);
}

/*
=============================================================

	VBO support

=============================================================
*/

/*
==================
GL_DeleteBModelVertexBuffer
==================
*/
void GL_DeleteBModelVertexBuffer (void)
{
	GL_WaitForDeviceIdle ();
	R_FreeBuffer (bmodel_vertex_buffer, &bmodel_memory, &num_vulkan_bmodel_allocations);
	R_FreeBuffer (vertex_submodels_buffer, &vertex_submodels_buffer_memory, &num_vulkan_bmodel_allocations);
	R_SetEmissiveLights (NULL, NULL, 0, NULL, 0);
	R_DeleteEmissiveBounceResources ();
}

/*
==================
GL_DeleteBModelAccelerationStructures
==================
*/
void GL_DeleteBModelAccelerationStructures (void)
{
	if (bmodel_tlas == VK_NULL_HANDLE)
	{
		GL_ResetLiveASTimestamp ();
		rs_live_as_cputime_us = 0;
		live_as_instance_count = 0;
		return;
	}

	GL_WaitForDeviceIdle ();
	GL_ResetLiveASTimestamp ();
	rs_live_as_cputime_us = 0;
	live_as_instance_count = 0;
	TEMP_ALLOC (VkBuffer, buffers, 1 + MAX_MODELS);
	int num_buffers = 0;
	buffers[num_buffers++] = bmodel_indices_buffer;
	for (int i = 0; i < MAX_MODELS; ++i)
	{
		qmodel_t *m = cl.model_precache[i];
		if (!m)
			continue;
		if (m->blas != VK_NULL_HANDLE)
		{
			vulkan_globals.vk_destroy_acceleration_structure (vulkan_globals.device, cl.model_precache[i]->blas, NULL);
			buffers[num_buffers++] = m->buffer;
			m->blas = VK_NULL_HANDLE;
			m->buffer = VK_NULL_HANDLE;
			m->address = 0;
		}
		assert (m->buffer == VK_NULL_HANDLE);
		assert (m->address == 0);
	}
	R_FreeBuffers (num_buffers, buffers, &bmodel_as_device_memory, &num_vulkan_bmodel_allocations);

	vulkan_globals.vk_destroy_acceleration_structure (vulkan_globals.device, bmodel_tlas, NULL);
	vkDestroyBuffer (vulkan_globals.device, bmodel_tlas_buffer, NULL);
	R_FreeVulkanMemory (&bmodel_tlas_device_memory, &num_vulkan_bmodel_allocations);

	bmodel_tlas = VK_NULL_HANDLE;
	bmodel_tlas_buffer = VK_NULL_HANDLE;
	bmodel_tlas_size = 0;
	bmodel_indices_buffer = VK_NULL_HANDLE;
	bmodel_indices_device_address = 0;
	TEMP_FREE (buffers);
}

/*
==================
GL_DeleteEmissiveWorldAccelerationStructure
==================
*/
void GL_DeleteEmissiveWorldAccelerationStructure (void)
{
	if (emissive_world_tlas == VK_NULL_HANDLE)
		return;

	GL_WaitForDeviceIdle ();
	vulkan_globals.vk_destroy_acceleration_structure (vulkan_globals.device, emissive_world_tlas, NULL);
	vulkan_globals.vk_destroy_acceleration_structure (vulkan_globals.device, emissive_world_blas, NULL);
	VkBuffer buffers[] = {emissive_world_indices_buffer, emissive_world_primitive_surfaces_buffer, emissive_world_instances_buffer,
		emissive_world_blas_buffer, emissive_world_tlas_buffer};
	R_FreeBuffers (countof (buffers), buffers, &emissive_world_as_memory, &num_vulkan_bmodel_allocations);

	emissive_world_blas = VK_NULL_HANDLE;
	emissive_world_tlas = VK_NULL_HANDLE;
	emissive_world_indices_buffer = VK_NULL_HANDLE;
	emissive_world_primitive_surfaces_buffer = VK_NULL_HANDLE;
	emissive_world_instances_buffer = VK_NULL_HANDLE;
	emissive_world_blas_buffer = VK_NULL_HANDLE;
	emissive_world_tlas_buffer = VK_NULL_HANDLE;
	emissive_world_indices_address = 0;
	emissive_world_instances_address = 0;
	emissive_world_blas_buffer_address = 0;
	emissive_world_blas_address = 0;
	emissive_world_as_bytes = 0;
	emissive_world_as_triangles = 0;
	emissive_world_as_build_time_us = 0;
	emissive_world_as_build_time_valid = false;
}

/*
==================
GL_EmissiveWorldAccelerationStructureStats
==================
*/
void GL_EmissiveWorldAccelerationStructureStats (
	uint64_t *bytes, uint32_t *triangle_count, uint32_t *build_time_us, qboolean *build_time_valid, qboolean *ready)
{
	*bytes = emissive_world_as_bytes;
	*triangle_count = emissive_world_as_triangles;
	*build_time_us = emissive_world_as_build_time_us;
	*build_time_valid = emissive_world_as_build_time_valid;
	*ready = emissive_world_tlas != VK_NULL_HANDLE;
}

static qboolean R_SurfaceInEmissiveWorldAccelerationStructure (const msurface_t *surface)
{
	return (surface->flags & ~(SURF_PLANEBACK | SURF_DRAWFENCE)) == 0;
}

/*
==================
GL_BuildEmissiveWorldAccelerationStructure

Builds the immutable worldspawn-only AS used while generating cacheable
emissive detail. It is independent from the live entity TLAS owned by
r_rtshadows.
==================
*/
static void GL_BuildEmissiveWorldAccelerationStructure (void)
{
	if (!vulkan_globals.ray_query || !cl.worldmodel || emissive_world_tlas != VK_NULL_HANDLE)
		return;

	qmodel_t *const worldmodel = cl.worldmodel;
	uint32_t		num_triangles = 0;
	for (int i = worldmodel->firstmodelsurface; i < worldmodel->firstmodelsurface + worldmodel->nummodelsurfaces; ++i)
	{
		const msurface_t *const surface = &worldmodel->surfaces[i];
		if (R_SurfaceInEmissiveWorldAccelerationStructure (surface))
			num_triangles += surface->numedges - 2;
	}
	if (!num_triangles)
		return;

	ZEROED_STRUCT (VkAccelerationStructureGeometryKHR, blas_geometry);
	blas_geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	blas_geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	blas_geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	blas_geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	blas_geometry.geometry.triangles.vertexStride = VERTEXSIZE * sizeof (float);
	blas_geometry.geometry.triangles.maxVertex = bmodel_numverts;
	blas_geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;

	ZEROED_STRUCT (VkAccelerationStructureBuildGeometryInfoKHR, blas_geometry_info);
	blas_geometry_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	blas_geometry_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	blas_geometry_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	blas_geometry_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	blas_geometry_info.geometryCount = 1;
	blas_geometry_info.pGeometries = &blas_geometry;

	ZEROED_STRUCT (VkAccelerationStructureBuildSizesInfoKHR, blas_sizes);
	blas_sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vulkan_globals.vk_get_acceleration_structure_build_sizes (
		vulkan_globals.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &blas_geometry_info, &num_triangles, &blas_sizes);

	ZEROED_STRUCT (VkAccelerationStructureGeometryKHR, tlas_geometry);
	tlas_geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	tlas_geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;

	ZEROED_STRUCT (VkAccelerationStructureBuildGeometryInfoKHR, tlas_geometry_info);
	tlas_geometry_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	tlas_geometry_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	tlas_geometry_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	tlas_geometry_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	tlas_geometry_info.geometryCount = 1;
	tlas_geometry_info.pGeometries = &tlas_geometry;

	const uint32_t tlas_num_instances = 1;
	ZEROED_STRUCT (VkAccelerationStructureBuildSizesInfoKHR, tlas_sizes);
	tlas_sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vulkan_globals.vk_get_acceleration_structure_build_sizes (
		vulkan_globals.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlas_geometry_info, &tlas_num_instances, &tlas_sizes);

	const size_t		 indices_size = (size_t)num_triangles * 3 * sizeof (uint32_t);
	const size_t		 primitive_surfaces_size = (size_t)num_triangles * sizeof (uint32_t);
	buffer_create_info_t buffer_create_infos[] = {
		{&emissive_world_indices_buffer, indices_size, 0,
		 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL, &emissive_world_indices_address,
		 "Emissive world indices"},
		{&emissive_world_primitive_surfaces_buffer, primitive_surfaces_size, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		 NULL, NULL, "Emissive world primitive surfaces"},
		{&emissive_world_instances_buffer, sizeof (VkAccelerationStructureInstanceKHR), 0,
		 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL, &emissive_world_instances_address,
		 "Emissive world instance"},
		{&emissive_world_blas_buffer, blas_sizes.accelerationStructureSize, 0, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, NULL,
		 &emissive_world_blas_buffer_address, "Emissive world BLAS"},
		{&emissive_world_tlas_buffer, tlas_sizes.accelerationStructureSize, 0, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, NULL, NULL,
		 "Emissive world TLAS"},
	};
	emissive_world_as_bytes = R_CreateBuffers (
		countof (buffer_create_infos), buffer_create_infos, &emissive_world_as_memory, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations,
		"Emissive world AS");

	VkResult err;
	ZEROED_STRUCT (VkAccelerationStructureCreateInfoKHR, acceleration_structure_create_info);
	acceleration_structure_create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	acceleration_structure_create_info.buffer = emissive_world_blas_buffer;
	acceleration_structure_create_info.size = blas_sizes.accelerationStructureSize;
	acceleration_structure_create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	err = vulkan_globals.vk_create_acceleration_structure (vulkan_globals.device, &acceleration_structure_create_info, NULL, &emissive_world_blas);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateAccelerationStructure failed with code %i", (int)err);
	ZEROED_STRUCT (VkAccelerationStructureDeviceAddressInfoKHR, address_info);
	address_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	address_info.accelerationStructure = emissive_world_blas;
	emissive_world_blas_address = vulkan_globals.vk_get_acceleration_structure_device_address (vulkan_globals.device, &address_info);

	acceleration_structure_create_info.buffer = emissive_world_tlas_buffer;
	acceleration_structure_create_info.size = tlas_sizes.accelerationStructureSize;
	acceleration_structure_create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	err = vulkan_globals.vk_create_acceleration_structure (vulkan_globals.device, &acceleration_structure_create_info, NULL, &emissive_world_tlas);
	if (err != VK_SUCCESS)
		Sys_Error ("vkCreateAccelerationStructure failed with code %i", (int)err);

	R_EnsureASScratchBufferSize (q_max (blas_sizes.buildScratchSize, tlas_sizes.buildScratchSize));

	const size_t	staging_primitive_offset = indices_size;
	const size_t	staging_instance_offset = q_align (staging_primitive_offset + primitive_surfaces_size, 16);
	const size_t	staging_size = staging_instance_offset + sizeof (VkAccelerationStructureInstanceKHR);
	VkQueryPool		build_timestamp_query_pool = VK_NULL_HANDLE;
	if (vulkan_globals.device_properties.limits.timestampComputeAndGraphics && (vulkan_globals.device_properties.limits.timestampPeriod > 0.0f))
	{
		ZEROED_STRUCT (VkQueryPoolCreateInfo, query_pool_create_info);
		query_pool_create_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		query_pool_create_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
		query_pool_create_info.queryCount = 2;
		err = vkCreateQueryPool (vulkan_globals.device, &query_pool_create_info, NULL, &build_timestamp_query_pool);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateQueryPool failed with code %i", (int)err);
	}
	VkCommandBuffer command_buffer;
	VkBuffer		staging_buffer;
	int				staging_offset;
	byte *const		staging_memory = R_StagingAllocate ((int)staging_size, 16, &command_buffer, &staging_buffer, &staging_offset);
	if (build_timestamp_query_pool != VK_NULL_HANDLE)
		vkCmdResetQueryPool (command_buffer, build_timestamp_query_pool, 0, 2);

	VkBufferCopy copy_regions[3];
	copy_regions[0].srcOffset = staging_offset;
	copy_regions[0].dstOffset = 0;
	copy_regions[0].size = indices_size;
	copy_regions[1].srcOffset = staging_offset + staging_primitive_offset;
	copy_regions[1].dstOffset = 0;
	copy_regions[1].size = primitive_surfaces_size;
	copy_regions[2].srcOffset = staging_offset + staging_instance_offset;
	copy_regions[2].dstOffset = 0;
	copy_regions[2].size = sizeof (VkAccelerationStructureInstanceKHR);
	vkCmdCopyBuffer (command_buffer, staging_buffer, emissive_world_indices_buffer, 1, &copy_regions[0]);
	vkCmdCopyBuffer (command_buffer, staging_buffer, emissive_world_primitive_surfaces_buffer, 1, &copy_regions[1]);
	vkCmdCopyBuffer (command_buffer, staging_buffer, emissive_world_instances_buffer, 1, &copy_regions[2]);

	ZEROED_STRUCT (VkMemoryBarrier, memory_barrier);
	memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	memory_barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vulkan_globals.vk_cmd_pipeline_barrier (
		command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

	blas_geometry.geometry.triangles.vertexData.deviceAddress = bmodel_vertex_buffer_device_address;
	blas_geometry.geometry.triangles.indexData.deviceAddress = emissive_world_indices_address;
	blas_geometry_info.dstAccelerationStructure = emissive_world_blas;
	blas_geometry_info.scratchData.deviceAddress = as_scratch_buffer.device_address;
	ZEROED_STRUCT (VkAccelerationStructureBuildRangeInfoKHR, blas_range);
	blas_range.primitiveCount = num_triangles;
	const VkAccelerationStructureBuildRangeInfoKHR *blas_range_ptr = &blas_range;
	if (build_timestamp_query_pool != VK_NULL_HANDLE)
		vkCmdWriteTimestamp (command_buffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, build_timestamp_query_pool, 0);
	vulkan_globals.vk_cmd_build_acceleration_structures (command_buffer, 1, &blas_geometry_info, &blas_range_ptr);

	memory_barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	memory_barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vulkan_globals.vk_cmd_pipeline_barrier (
		command_buffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &memory_barrier,
		0, NULL, 0, NULL);

	tlas_geometry.geometry.instances.data.deviceAddress = emissive_world_instances_address;
	tlas_geometry_info.dstAccelerationStructure = emissive_world_tlas;
	tlas_geometry_info.scratchData.deviceAddress = as_scratch_buffer.device_address;
	ZEROED_STRUCT (VkAccelerationStructureBuildRangeInfoKHR, tlas_range);
	tlas_range.primitiveCount = 1;
	const VkAccelerationStructureBuildRangeInfoKHR *tlas_range_ptr = &tlas_range;
	vulkan_globals.vk_cmd_build_acceleration_structures (command_buffer, 1, &tlas_geometry_info, &tlas_range_ptr);
	if (build_timestamp_query_pool != VK_NULL_HANDLE)
		vkCmdWriteTimestamp (command_buffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, build_timestamp_query_pool, 1);

	memory_barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	memory_barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vulkan_globals.vk_cmd_pipeline_barrier (
		command_buffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

	R_StagingBeginCopy ();
	uint32_t *indices = (uint32_t *)staging_memory;
	uint32_t *primitive_surfaces = (uint32_t *)(staging_memory + staging_primitive_offset);
	uint32_t  current_index = 0;
	uint32_t  current_primitive = 0;
	for (int i = worldmodel->firstmodelsurface; i < worldmodel->firstmodelsurface + worldmodel->nummodelsurfaces; ++i)
	{
		const msurface_t *const surface = &worldmodel->surfaces[i];
		if (!R_SurfaceInEmissiveWorldAccelerationStructure (surface))
			continue;
		for (int k = 2; k < surface->numedges; ++k)
		{
			primitive_surfaces[current_primitive++] = i;
			indices[current_index++] = surface->vbo_firstvert;
			indices[current_index++] = surface->vbo_firstvert + k - 1;
			indices[current_index++] = surface->vbo_firstvert + k;
		}
	}
	assert (current_index == num_triangles * 3);
	assert (current_primitive == num_triangles);

	VkAccelerationStructureInstanceKHR *const instance = (VkAccelerationStructureInstanceKHR *)(staging_memory + staging_instance_offset);
	memset (instance, 0, sizeof (*instance));
	instance->transform.matrix[0][0] = 1.0f;
	instance->transform.matrix[1][1] = 1.0f;
	instance->transform.matrix[2][2] = 1.0f;
	instance->mask = 0x01;
	instance->flags = VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR | VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
	instance->accelerationStructureReference = emissive_world_blas_address;
	R_StagingEndCopy ();

	R_SubmitStagingBuffers ();
	GL_WaitForDeviceIdle ();
	if (build_timestamp_query_pool != VK_NULL_HANDLE)
	{
		uint64_t timestamps[2];
		if (vkGetQueryPoolResults (
				vulkan_globals.device, build_timestamp_query_pool, 0, 2, sizeof (timestamps), timestamps, sizeof (uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
		{
			emissive_world_as_build_time_us =
				(uint32_t)((double)(timestamps[1] - timestamps[0]) * (double)vulkan_globals.device_properties.limits.timestampPeriod / 1000.0);
			emissive_world_as_build_time_valid = true;
		}
		vkDestroyQueryPool (vulkan_globals.device, build_timestamp_query_pool, NULL);
	}
	if (!GL_LiveAccelerationStructureRequired ())
		R_FreeASScratchBuffer ();
	emissive_world_as_triangles = num_triangles;
	if (emissive_world_as_build_time_valid)
		Con_DPrintf (
			"RT emissives: built immutable world AS (%u triangles, %" PRIu64 " bytes, %.3f ms GPU)\n", emissive_world_as_triangles,
			emissive_world_as_bytes, (double)emissive_world_as_build_time_us / 1000.0);
	else
		Con_DPrintf (
			"RT emissives: built immutable world AS (%u triangles, %" PRIu64 " bytes, GPU timing unavailable)\n", emissive_world_as_triangles,
			emissive_world_as_bytes);
}

/*
==================
GL_BuildBModelVertexBuffer

Deletes gl_bmodel_vbo if it already exists, then rebuilds it with all
surfaces from world + all brush models
==================
*/
void GL_BuildBModelVertexBuffer (void)
{
	unsigned int varray_bytes;
	int			 i, j;
	qmodel_t	*m;

	// count all verts in all models
	bmodel_numverts = 0;
	for (j = 1; j < MAX_MODELS; j++)
	{
		m = cl.model_precache[j];
		if (!m || m->name[0] == '*' || m->type != mod_brush)
			continue;

		for (i = 0; i < m->numsurfaces; i++)
		{
			bmodel_numverts += m->surfaces[i].numedges;
		}
	}

	// build vertex array
	varray_bytes = VERTEXSIZE * sizeof (float) * bmodel_numverts;
	TEMP_ALLOC_ZEROED (float, varray, varray_bytes / sizeof (float));
	TEMP_ALLOC_ZEROED (uint32_t, vertex_submodels, bmodel_numverts);

	int current_submodel = 0;
	for (j = 1; j < MAX_MODELS; j++)
	{
		m = cl.model_precache[j];
		if (!m || m->name[0] == '*' || m->type != mod_brush)
			continue;

		for (i = 0; i < m->numsurfaces; i++)
		{
			msurface_t *s = &m->surfaces[i];
			memcpy (&varray[VERTEXSIZE * s->vbo_firstvert], s->polys->verts, VERTEXSIZE * sizeof (float) * s->numedges);

			uint32_t submodel = 0;
			if (j == 1) // the worldmodel surface array also contains all movable submodel surfaces
			{
				while (((current_submodel + 1) < m->numsubmodels) && (i >= m->submodels[current_submodel + 1].firstface))
					++current_submodel;
				if (current_submodel < num_worldmodel_submodels)
					submodel = current_submodel;
			}
			for (int v = 0; v < s->numedges; v++)
				vertex_submodels[s->vbo_firstvert + v] = submodel;
		}
	}

	VkImageUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	if (vulkan_globals.ray_query)
		usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

	// Allocate & upload to GPU
	R_CreateBuffer (
		&bmodel_vertex_buffer, &bmodel_memory, varray_bytes, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations,
		&bmodel_vertex_buffer_device_address, "BModel vertices");
	R_StagingUploadBuffer (bmodel_vertex_buffer, varray_bytes, (byte *)varray);

	R_CreateBuffer (
		&vertex_submodels_buffer, &vertex_submodels_buffer_memory, bmodel_numverts * sizeof (uint32_t),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, NULL,
		"BModel vertex submodels");
	R_StagingUploadBuffer (vertex_submodels_buffer, bmodel_numverts * sizeof (uint32_t), (byte *)vertex_submodels);
	TEMP_FREE (vertex_submodels);
	TEMP_FREE (varray);

	if (vulkan_globals.bmodel_instances_desc_set != VK_NULL_HANDLE)
		R_FreeDescriptorSet (vulkan_globals.bmodel_instances_desc_set, &vulkan_globals.bmodel_instances_set_layout);
	vulkan_globals.bmodel_instances_desc_set = R_AllocateDescriptorSet (&vulkan_globals.bmodel_instances_set_layout);

	ZEROED_STRUCT (VkDescriptorBufferInfo, vertex_submodels_buffer_info);
	vertex_submodels_buffer_info.buffer = vertex_submodels_buffer;
	vertex_submodels_buffer_info.offset = 0;
	vertex_submodels_buffer_info.range = VK_WHOLE_SIZE;

	ZEROED_STRUCT (VkDescriptorBufferInfo, bmodel_instances_buffer_info);
	bmodel_instances_buffer_info.buffer = bmodel_instances_buffer;
	bmodel_instances_buffer_info.offset = 0;
	bmodel_instances_buffer_info.range = VK_WHOLE_SIZE;

	ZEROED_STRUCT_ARRAY (VkWriteDescriptorSet, instance_writes, 2);
	instance_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	instance_writes[0].dstBinding = 0;
	instance_writes[0].dstArrayElement = 0;
	instance_writes[0].descriptorCount = 1;
	instance_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	instance_writes[0].dstSet = vulkan_globals.bmodel_instances_desc_set;
	instance_writes[0].pBufferInfo = &vertex_submodels_buffer_info;
	instance_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	instance_writes[1].dstBinding = 1;
	instance_writes[1].dstArrayElement = 0;
	instance_writes[1].descriptorCount = 1;
	instance_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	instance_writes[1].dstSet = vulkan_globals.bmodel_instances_desc_set;
	instance_writes[1].pBufferInfo = &bmodel_instances_buffer_info;
	vkUpdateDescriptorSets (vulkan_globals.device, countof (instance_writes), instance_writes, 0, NULL);
}

/*
==================
GL_BuildBModelAccelerationStructures
==================
*/
static void GL_BuildBModelAccelerationStructures (void)
{
	VkResult err;

	if (!vulkan_globals.ray_query || !GL_LiveAccelerationStructureRequired () || (bmodel_tlas != VK_NULL_HANDLE))
		return;

	// count all tris in all models
	uint32_t total_num_triangles = 0;
	TEMP_ALLOC_ZEROED (uint32_t, blas_num_tris, MAX_MODELS);
	TEMP_ALLOC_ZEROED (VkAccelerationStructureGeometryKHR, blas_geometries, MAX_MODELS);
	TEMP_ALLOC_ZEROED (VkAccelerationStructureBuildGeometryInfoKHR, blas_geometry_infos, MAX_MODELS);
	TEMP_ALLOC_ZEROED (VkAccelerationStructureBuildSizesInfoKHR, blas_sizes_infos, MAX_MODELS);
	TEMP_ALLOC_ZEROED (qmodel_t *, blas_models, MAX_MODELS);
	TEMP_ALLOC_ZEROED (buffer_create_info_t, buffer_create_infos, 1 + MAX_MODELS);

	size_t scratch_buffer_size = 0;
	int	   num_blas = 0;
	for (int j = 1; j < MAX_MODELS; j++)
	{
		qmodel_t *m = cl.model_precache[j];
		if (!m || m->type != mod_brush)
			continue;
		if (m->flags & MF_HOLEY)
			continue;

		for (int i = m->firstmodelsurface; i < m->firstmodelsurface + m->nummodelsurfaces; i++)
		{
			msurface_t *s = &m->surfaces[i];
			if ((s->flags & ~SURF_PLANEBACK) != 0)
				continue;
			total_num_triangles += m->surfaces[i].numedges - 2;
			blas_num_tris[num_blas] += m->surfaces[i].numedges - 2;
		}
		if (blas_num_tris[num_blas] == 0)
			continue;

		blas_models[num_blas] = m;

		VkAccelerationStructureGeometryKHR *blas_geometry = &blas_geometries[num_blas];
		blas_geometry->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
		blas_geometry->geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
		blas_geometry->geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
		blas_geometry->geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		blas_geometry->geometry.triangles.vertexStride = 28;
		blas_geometry->geometry.triangles.maxVertex = bmodel_numverts;
		blas_geometry->geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;

		VkAccelerationStructureBuildGeometryInfoKHR *blas_geometry_info = &blas_geometry_infos[num_blas];
		blas_geometry_info->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
		blas_geometry_info->type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		blas_geometry_info->flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		blas_geometry_info->mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		blas_geometry_info->geometryCount = 1;
		blas_geometry_info->pGeometries = blas_geometry;

		VkAccelerationStructureBuildSizesInfoKHR *blas_build_sizes_info = &blas_sizes_infos[num_blas];
		blas_build_sizes_info->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
		vulkan_globals.vk_get_acceleration_structure_build_sizes (
			vulkan_globals.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, blas_geometry_info, &blas_num_tris[num_blas], blas_build_sizes_info);

		scratch_buffer_size = q_max (scratch_buffer_size, blas_build_sizes_info->buildScratchSize);
		++num_blas;
	}

	if (num_blas == 0)
	{
		TEMP_FREE (blas_num_tris);
		TEMP_FREE (blas_geometries);
		TEMP_FREE (blas_geometry_infos);
		TEMP_FREE (blas_sizes_infos);
		TEMP_FREE (blas_models);
		TEMP_FREE (buffer_create_infos);
		return;
	}

	// Query TLAS sizes for initial instance count
	{
		ZEROED_STRUCT (VkAccelerationStructureGeometryKHR, tlas_geometry);
		tlas_geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
		tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
		tlas_geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;

		ZEROED_STRUCT (VkAccelerationStructureBuildGeometryInfoKHR, tlas_geometry_info);
		tlas_geometry_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
		tlas_geometry_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
		tlas_geometry_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		tlas_geometry_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		tlas_geometry_info.geometryCount = 1;
		tlas_geometry_info.pGeometries = &tlas_geometry;

		ZEROED_STRUCT (VkAccelerationStructureBuildSizesInfoKHR, tlas_build_sizes_info);
		tlas_build_sizes_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
		vulkan_globals.vk_get_acceleration_structure_build_sizes (
			vulkan_globals.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlas_geometry_info, &bmodel_tlas_max_instances, &tlas_build_sizes_info);

		scratch_buffer_size = q_max (scratch_buffer_size, tlas_build_sizes_info.buildScratchSize);
		bmodel_tlas_size = tlas_build_sizes_info.accelerationStructureSize;
	}

	const size_t indices_size = total_num_triangles * 3 * sizeof (uint32_t);

	buffer_create_infos[0].buffer = &bmodel_indices_buffer;
	buffer_create_infos[0].size = indices_size;
	buffer_create_infos[0].usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer_create_infos[0].address = &bmodel_indices_device_address;
	buffer_create_infos[0].name = "BModel indices";

	for (int i = 0; i < num_blas; ++i)
	{
		buffer_create_info_t *create_info = &buffer_create_infos[1 + i];
		create_info->buffer = &blas_models[i]->buffer;
		create_info->size = blas_sizes_infos[i].accelerationStructureSize;
		create_info->usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
		create_info->address = &blas_models[i]->address;
		create_info->name = "BModel BLAS";
	}

	const size_t total_as_device_size = R_CreateBuffers (
		1 + num_blas, buffer_create_infos, &bmodel_as_device_memory, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &num_vulkan_bmodel_allocations, "BModel AS");

	Sys_Printf ("Allocating acceleration structure data (%u KB)\n", (int)(total_as_device_size / 1024ull));

	R_AllocateTLAS ();

	R_EnsureASScratchBufferSize (scratch_buffer_size);

	VkBuffer		staging_buffer;
	VkCommandBuffer command_buffer;
	int				staging_offset;
	unsigned char  *staging_memory = R_StagingAllocate (indices_size, 1, &command_buffer, &staging_buffer, &staging_offset);

	{
		ZEROED_STRUCT (VkBufferCopy, region);
		region.srcOffset = staging_offset;
		region.dstOffset = 0;
		region.size = indices_size;
		vkCmdCopyBuffer (command_buffer, staging_buffer, bmodel_indices_buffer, 1, &region);

		ZEROED_STRUCT (VkMemoryBarrier, memory_barrier);
		memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vulkan_globals.vk_cmd_pipeline_barrier (
			command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
	}

	size_t scratch_offset = 0;
	size_t indices_offsets = 0;
	for (int i = 0; i < num_blas; ++i)
	{
		scratch_offset =
			q_align (scratch_offset, vulkan_globals.physical_device_acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment);

		if ((scratch_offset + blas_sizes_infos[i].buildScratchSize) > scratch_buffer_size)
		{
			ZEROED_STRUCT (VkMemoryBarrier, memory_barrier);
			memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			memory_barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
			memory_barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
			vulkan_globals.vk_cmd_pipeline_barrier (
				command_buffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1,
				&memory_barrier, 0, NULL, 0, NULL);
			scratch_offset = 0;
		}

		ZEROED_STRUCT (VkAccelerationStructureCreateInfoKHR, acceleration_structure_create_info);
		acceleration_structure_create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
		acceleration_structure_create_info.buffer = blas_models[i]->buffer;
		acceleration_structure_create_info.size = blas_sizes_infos[i].accelerationStructureSize;
		acceleration_structure_create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		err = vulkan_globals.vk_create_acceleration_structure (vulkan_globals.device, &acceleration_structure_create_info, NULL, &blas_models[i]->blas);
		if (err != VK_SUCCESS)
			Sys_Error ("vkCreateAccelerationStructure failed with code %i", (int)err);

		ZEROED_STRUCT (VkAccelerationStructureBuildRangeInfoKHR, build_range_info);
		build_range_info.primitiveCount = blas_num_tris[i];
		VkAccelerationStructureBuildGeometryInfoKHR *blas_geometry_info = &blas_geometry_infos[i];
		blas_geometry_info->dstAccelerationStructure = blas_models[i]->blas;
		blas_geometry_info->scratchData.deviceAddress = as_scratch_buffer.device_address + scratch_offset;
		VkAccelerationStructureGeometryKHR *blas_geometry = &blas_geometries[i];
		blas_geometry->geometry.triangles.vertexData.deviceAddress = bmodel_vertex_buffer_device_address;
		blas_geometry->geometry.triangles.indexData.deviceAddress = bmodel_indices_device_address + indices_offsets;
		const VkAccelerationStructureBuildRangeInfoKHR *build_range_info_ptr = &build_range_info;
		vulkan_globals.vk_cmd_build_acceleration_structures (command_buffer, 1, blas_geometry_info, &build_range_info_ptr);

		scratch_offset += blas_sizes_infos[i].buildScratchSize;
		indices_offsets += blas_num_tris[i] * 3 * sizeof (uint32_t);
	}

	{
		ZEROED_STRUCT (VkMemoryBarrier, memory_barrier);
		memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memory_barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		memory_barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
		vulkan_globals.vk_cmd_pipeline_barrier (
			command_buffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1,
			&memory_barrier, 0, NULL, 0, NULL);
	}

	TEMP_FREE (blas_num_tris);
	TEMP_FREE (blas_geometries);
	TEMP_FREE (blas_geometry_infos);
	TEMP_FREE (blas_sizes_infos);
	TEMP_FREE (blas_models);
	TEMP_FREE (buffer_create_infos);

	uint32_t *indices = (uint32_t *)staging_memory;
	uint32_t  current_index = 0;
	R_StagingBeginCopy ();

	for (int j = 1; j < MAX_MODELS; j++)
	{
		qmodel_t *m = cl.model_precache[j];
		if (!m || m->type != mod_brush)
			continue;
		if (m->flags & MF_HOLEY)
			continue;

		for (int i = m->firstmodelsurface; i < m->firstmodelsurface + m->nummodelsurfaces; i++)
		{
			msurface_t *s = &m->surfaces[i];
			if ((s->flags & ~SURF_PLANEBACK) != 0)
				continue;

			for (int k = 2; k < s->numedges; ++k)
			{
				indices[current_index++] = s->vbo_firstvert;
				indices[current_index++] = s->vbo_firstvert + k - 1;
				indices[current_index++] = s->vbo_firstvert + k;
			}
		}
	}
	R_StagingEndCopy ();
}

/*
==================
GL_RequestAccelerationStructure

Routes each active ray-query consumer to the minimum scene representation it
currently needs. Cacheable emissives use the immutable worldspawn AS; RT
shadows retain the live world, brush, and alias-model AS path.
==================
*/
void GL_RequestAccelerationStructure (rt_as_consumer_t consumer)
{
	if (!vulkan_globals.ray_query)
		return;

	switch (consumer)
	{
	case RT_AS_CONSUMER_CACHEABLE_EMISSIVES:
	case RT_AS_CONSUMER_TRANSIENT_EMISSIVES:
		GL_BuildEmissiveWorldAccelerationStructure ();
		if (CLAMP (0, (int)r_emissive_rt_occluders.value, 2) > 0)
			GL_BuildBModelAccelerationStructures ();
		break;
	case RT_AS_CONSUMER_RT_SHADOWS:
		GL_BuildBModelAccelerationStructures ();
		break;
	default:
		Sys_Error ("GL_RequestAccelerationStructure: invalid consumer %d", (int)consumer);
	}
}

qboolean GL_LiveAccelerationStructureRequired (void)
{
	return (r_rtshadows.value > 0.0f && r_gpulightmapupdate.value > 0.0f) ||
		(r_emissive_rt.value > 0.0f && CLAMP (0, (int)r_emissive_rt_occluders.value, 2) > 0);
}

qboolean GL_AnimatedAccelerationStructureRequired (void)
{
	return (r_rtshadows.value > 0.0f && r_gpulightmapupdate.value > 0.0f) ||
		(r_emissive_rt.value > 0.0f && CLAMP (0, (int)r_emissive_rt_occluders.value, 2) > 1);
}

static VkAccelerationStructureKHR R_EmissiveDirectAccelerationStructure (void)
{
	if (r_emissive_rt.value > 0.0f && CLAMP (0, (int)r_emissive_rt_occluders.value, 2) > 0 && live_as_instance_count > 0)
		return bmodel_tlas;
	return emissive_world_tlas;
}

static uint32_t R_EmissiveOccluderMask (void)
{
	return CLAMP (0, (int)r_emissive_rt_occluders.value, 2) > 1 ? 0x03u : 0x01u;
}

void GL_LiveAccelerationStructureStats (qboolean *ready, uint32_t *instance_count)
{
	*ready = bmodel_tlas != VK_NULL_HANDLE;
	*instance_count = live_as_instance_count;
}

/*
=============
R_BuildTopLevelAccelerationStructure
=============
*/
void R_BuildTopLevelAccelerationStructure (void *unused)
{
	const int occluder_tier = CLAMP (0, (int)r_emissive_rt_occluders.value, 2);
	if (r_emissive_rt.value > 0.0f && occluder_tier > 0 && R_UpdateEmissiveOccluderState ())
	{
		if (!emissive_detail_building && num_emissive_logical_tiles > 0 && R_EmissiveDetailAvailable () &&
			(emissive_occluder_full_detail_refresh || num_emissive_occluder_dirty_tiles > 0))
			emissive_detail_pending = true;
		if (num_transient_emissive_tiles > 0 && R_TransientEmissiveDetailAvailable ())
		{
			/* Participating occluder movement invalidates transient transport exactly
			 * like an emitter-list change; the pending flag below schedules the
			 * replacement. Ordered before update-draw consumers by the task graph. */
			R_InvalidateTransientEmissiveDetail ();
			transient_emissive_detail_pending = true;
			transient_emissive_occluder_tiles_pending = true;
			/* The combined atlas embeds direct detail: deselect it until the no-ray
			 * refresh scheduled below rebuilds it from the new direct field. */
			emissive_transient_bounce_epoch = 0;
			emissive_bounce_transient_refresh_pending = emissive_bounce_ready;
		}
		emissive_occluder_receiver_refresh_pending = true;
	}
	if (bmodel_tlas == VK_NULL_HANDLE)
		return;
	const qboolean rt_shadows_require_update = r_rtshadows.value > 0.0f && r_gpulightmapupdate.value > 0.0f;
	if (!rt_shadows_require_update && r_emissive_rt.value > 0.0f && CLAMP (0, (int)r_emissive_rt_occluders.value, 2) > 0 &&
		!emissive_live_as_dirty)
		return;

	const double  start_time = Sys_DoubleTime ();
	cb_context_t *cbx = &vulkan_globals.primary_cb_contexts[PCBX_BUILD_ACCELERATION_STRUCTURES];
	GL_BeginLiveASTimestamp (cbx);

	// Update animated entity BLASes first
	R_UpdateAnimatedBLASes (cbx);

	R_BeginDebugUtilsLabel (cbx, "Build TLAS");

	int num_instances = 0;
	for (int i = 0; i < cl.num_entities + cl.num_statics; ++i)
	{
		entity_t *e = (i < cl.num_entities) ? &cl.entities[i] : cl.static_entities[i - cl.num_entities];
		if (!e->model || e->model->needload)
			continue;
		if ((e->alpha != ENTALPHA_DEFAULT) && (ENTALPHA_DECODE (e->alpha) < 1.0f))
			continue;

		// Brush models use model BLAS, alias models use entity BLAS
		if (e->model->type == mod_brush && e->model->blas != VK_NULL_HANDLE)
			++num_instances;
		else if (
			e->model->type == mod_alias && e->blas_data && e->blas_data->blas != VK_NULL_HANDLE && !e->blas_data->needs_initial_build &&
			e->blas_data->model == e->model)
			++num_instances;
	}

	VkDeviceAddress						instances_device_address;
	VkAccelerationStructureInstanceKHR *instances = (VkAccelerationStructureInstanceKHR *)R_StorageAllocate (
		num_instances * sizeof (VkAccelerationStructureInstanceKHR), NULL, NULL, &instances_device_address);

	num_instances = 0;
	for (int i = 0; i < cl.num_entities + cl.num_statics; ++i)
	{
		entity_t *e = (i < cl.num_entities) ? &cl.entities[i] : cl.static_entities[i - cl.num_entities];
		if (!e->model || e->model->needload)
			continue;
		if ((e->alpha != ENTALPHA_DEFAULT) && (ENTALPHA_DECODE (e->alpha) < 1.0f))
			continue;

		VkDeviceAddress address = 0;
		qboolean		is_alias = false;

		if (e->model->type == mod_brush && e->model->blas != VK_NULL_HANDLE)
		{
			address = e->model->address;
		}
		else if (
			e->model->type == mod_alias && e->blas_data && e->blas_data->blas != VK_NULL_HANDLE && !e->blas_data->needs_initial_build &&
			e->blas_data->model == e->model)
		{
			address = e->blas_data->address;
			is_alias = true;
		}
		else
		{
			continue;
		}

		vec3_t lerped_origin, lerped_angles;
		if (is_alias)
			R_GetEntityLerpedTransform (e, lerped_origin, lerped_angles);
		else
		{
			VectorCopy (e->origin, lerped_origin);
			VectorCopy (e->angles, lerped_angles);
		}
		lerped_angles[0] = -lerped_angles[0]; // quake bug

		float model_matrix[16];
		IdentityMatrix (model_matrix);
		if (e->model != cl.worldmodel)
			R_RotateForEntity (model_matrix, lerped_origin, lerped_angles, e->netstate.scale);

		// For alias models, apply scale_origin translation and scale
		if (is_alias)
		{
			aliashdr_t *hdr = (aliashdr_t *)Mod_Extradata (e->model);
			if (hdr)
			{
				float translation_matrix[16];
				TranslationMatrix (translation_matrix, hdr->scale_origin[0], hdr->scale_origin[1], hdr->scale_origin[2]);
				MatrixMultiply (model_matrix, translation_matrix);

				float scale_matrix[16];
				ScaleMatrix (scale_matrix, hdr->scale[0], hdr->scale[1], hdr->scale[2]);
				MatrixMultiply (model_matrix, scale_matrix);
			}
		}

		VkAccelerationStructureInstanceKHR *instance = &instances[num_instances];
		instance->transform.matrix[0][0] = model_matrix[0];
		instance->transform.matrix[0][1] = model_matrix[4];
		instance->transform.matrix[0][2] = model_matrix[8];
		instance->transform.matrix[0][3] = model_matrix[12];
		instance->transform.matrix[1][0] = model_matrix[1];
		instance->transform.matrix[1][1] = model_matrix[5];
		instance->transform.matrix[1][2] = model_matrix[9];
		instance->transform.matrix[1][3] = model_matrix[13];
		instance->transform.matrix[2][0] = model_matrix[2];
		instance->transform.matrix[2][1] = model_matrix[6];
		instance->transform.matrix[2][2] = model_matrix[10];
		instance->transform.matrix[2][3] = model_matrix[14];
		instance->instanceCustomIndex = 0;
		instance->mask = is_alias ? (R_EmissiveAliasEntityIsSource (e) ? 0x04 : 0x02) : 0x01;
		instance->instanceShaderBindingTableRecordOffset = 0;
		instance->flags = VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR | VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		instance->accelerationStructureReference = address;

		++num_instances;
	}
	ZEROED_STRUCT (VkAccelerationStructureGeometryKHR, tlas_geometry);
	tlas_geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	tlas_geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	tlas_geometry.geometry.instances.data.deviceAddress = instances_device_address;

	ZEROED_STRUCT (VkAccelerationStructureBuildGeometryInfoKHR, tlas_geometry_info);
	tlas_geometry_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	tlas_geometry_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	tlas_geometry_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	tlas_geometry_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	tlas_geometry_info.geometryCount = 1;
	tlas_geometry_info.pGeometries = &tlas_geometry;

	// Resize TLAS if instance count exceeds current capacity
	if ((uint32_t)num_instances > bmodel_tlas_max_instances)
	{
		tlas_garbage[tlas_garbage_index] = bmodel_tlas;
		dynbuffer_t tlas_dynbuf;
		memset (&tlas_dynbuf, 0, sizeof (tlas_dynbuf));
		tlas_dynbuf.buffer = bmodel_tlas_buffer;
		R_AddDynamicBufferGarbage (bmodel_tlas_device_memory, &tlas_dynbuf, 1, NULL);

		bmodel_tlas_max_instances = ((num_instances / TLAS_SIZE_MULTIPLE) + 1) * TLAS_SIZE_MULTIPLE;

		ZEROED_STRUCT (VkAccelerationStructureBuildSizesInfoKHR, new_tlas_sizes);
		new_tlas_sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
		vulkan_globals.vk_get_acceleration_structure_build_sizes (
			vulkan_globals.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlas_geometry_info, &bmodel_tlas_max_instances, &new_tlas_sizes);
		bmodel_tlas_size = new_tlas_sizes.accelerationStructureSize;

		Sys_Printf ("Reallocating TLAS for %u instances (%u KB)\n", bmodel_tlas_max_instances, (uint32_t)(bmodel_tlas_size / 1024));
		R_AllocateTLAS ();
	}

	const uint32_t							 tlas_num_instances = num_instances;
	VkAccelerationStructureBuildSizesInfoKHR tlas_sizes;
	tlas_sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	tlas_sizes.pNext = NULL;
	vulkan_globals.vk_get_acceleration_structure_build_sizes (
		vulkan_globals.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlas_geometry_info, &tlas_num_instances, &tlas_sizes);

	R_EnsureASScratchBufferSize (tlas_sizes.buildScratchSize);

	tlas_geometry_info.dstAccelerationStructure = bmodel_tlas;
	tlas_geometry_info.scratchData.deviceAddress = as_scratch_buffer.device_address;

	ZEROED_STRUCT (VkAccelerationStructureBuildRangeInfoKHR, build_range_info);
	build_range_info.primitiveCount = num_instances;
	const VkAccelerationStructureBuildRangeInfoKHR *build_range_info_ptr = &build_range_info;
	vulkan_globals.vk_cmd_build_acceleration_structures (cbx->cb, 1, &tlas_geometry_info, &build_range_info_ptr);

	ZEROED_STRUCT (VkMemoryBarrier, memory_barrier);
	memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory_barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	memory_barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vulkan_globals.vk_cmd_pipeline_barrier (
		cbx->cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

	R_EndDebugUtilsLabel (cbx);
	GL_EndLiveASTimestamp (cbx);
	live_as_instance_count = num_instances;
	rs_live_as_cputime_us = (uint32_t)((Sys_DoubleTime () - start_time) * 1000000.0);
	emissive_live_as_dirty = false;
}

/*
=================
SoA_FillBoxLane
=================
*/
void SoA_FillBoxLane (soa_aabb_t *boxes, int index, vec3_t mins, vec3_t maxs)
{
	float *dst = boxes[index >> 3];
	index &= 7;
	dst[index + 0] = mins[0];
	dst[index + 8] = maxs[0];
	dst[index + 16] = mins[1];
	dst[index + 24] = maxs[1];
	dst[index + 32] = mins[2];
	dst[index + 40] = maxs[2];
}

/*
=================
SoA_FillPlaneLane
=================
*/
void SoA_FillPlaneLane (soa_plane_t *planes, int index, mplane_t *src, qboolean flip)
{
	float  side = flip ? -1.0f : 1.0f;
	float *dst = planes[index >> 3];
	index &= 7;
	dst[index + 0] = side * src->normal[0];
	dst[index + 8] = side * src->normal[1];
	dst[index + 16] = side * src->normal[2];
	dst[index + 24] = side * src->dist;
}

/*
===============
GL_PrepareSIMDData
===============
*/
void GL_PrepareSIMDAndParallelData (void)
{
	cl.worldmodel->surfvis = Mem_Alloc (((cl.worldmodel->numsurfaces + 31) / 8));
#ifdef USE_SIMD
	int i;

	cl.worldmodel->soa_leafbounds = Mem_Alloc (6 * sizeof (float) * ((cl.worldmodel->numleafs + 31) & ~7));
	cl.worldmodel->soa_surfplanes = Mem_Alloc (4 * sizeof (float) * ((cl.worldmodel->numsurfaces + 31) & ~7));

	for (i = 0; i < cl.worldmodel->numleafs; ++i)
	{
		mleaf_t *leaf = &cl.worldmodel->leafs[i + 1];
		SoA_FillBoxLane (cl.worldmodel->soa_leafbounds, i, leaf->minmaxs, leaf->minmaxs + 3);
	}

	for (i = 0; i < cl.worldmodel->numsurfaces; ++i)
	{
		msurface_t *surf = &cl.worldmodel->surfaces[i];
		SoA_FillPlaneLane (cl.worldmodel->soa_surfplanes, i, surf->plane, surf->flags & SURF_PLANEBACK);
	}
#endif // def USE_SIMD
}

/*
===============
R_AddDynamicLights
===============
*/
void R_AddDynamicLights (msurface_t *surf)
{
	int			lnum;
	int			sd, td;
	float		dist, rad, minlight;
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	mtexinfo_t *tex;
	// johnfitz -- lit support via lordhavoc
	float		cred, cgreen, cblue, brightness;
	unsigned   *bl;
	// johnfitz

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	tex = surf->texinfo;

	for (lnum = 0; lnum < MAX_DLIGHTS; lnum++)
	{
		if (!(surf->dlightbits[lnum >> 5] & (1U << (lnum & 31))))
			continue; // not lit by this light

		// lightmap_dlight_origins holds the light position in the space of the model this surface belongs to
		rad = cl_dlights[lnum].radius;
		dist = DotProduct (lightmap_dlight_origins[lnum], surf->plane->normal) - surf->plane->dist;
		rad -= fabs (dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i = 0; i < 3; i++)
		{
			impact[i] = lightmap_dlight_origins[lnum][i] - surf->plane->normal[i] * dist;
		}

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];

		// johnfitz -- lit support via lordhavoc
		bl = blocklights;
		cred = cl_dlights[lnum].color[0] * 256.0f;
		cgreen = cl_dlights[lnum].color[1] * 256.0f;
		cblue = cl_dlights[lnum].color[2] * 256.0f;
		// johnfitz
		for (t = 0; t < tmax; t++)
		{
			td = local[1] - t * 16;
			if (td < 0)
				td = -td;
			for (s = 0; s < smax; s++)
			{
				sd = local[0] - s * 16;
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td >> 1);
				else
					dist = td + (sd >> 1);
				if (dist < minlight)
				// johnfitz -- lit support via lordhavoc
				{
					brightness = rad - dist;
					bl[0] += (int)(brightness * cred);
					bl[1] += (int)(brightness * cgreen);
					bl[2] += (int)(brightness * cblue);
				}
				bl += 3;
				// johnfitz
			}
		}
	}
}

/*
===============
R_AccumulateLightmap

Scales 'lightmap' contents (RGB8) by 'scale' and accumulates
the result in the 'blocklights' array (RGB32)
===============
*/
void R_AccumulateLightmap (byte *lightmap, unsigned scale, int texels)
{
	unsigned *bl = blocklights;
	int		  size = texels * 3;

#if defined(USE_SIMD)
	if (use_simd && size >= 8)
	{
#if defined(USE_SSE2)
		__m128i vscale = _mm_set1_epi16 (scale);
		__m128i vlo, vhi, vdst, vsrc, v;

		while (size >= 8)
		{
			vsrc = _mm_loadl_epi64 ((const __m128i *)lightmap);

			v = _mm_unpacklo_epi8 (vsrc, _mm_setzero_si128 ());
			vlo = _mm_mullo_epi16 (v, vscale);
			vhi = _mm_mulhi_epu16 (v, vscale);

			vdst = _mm_loadu_si128 ((const __m128i *)bl);
			vdst = _mm_add_epi32 (vdst, _mm_unpacklo_epi16 (vlo, vhi));
			_mm_storeu_si128 ((__m128i *)bl, vdst);
			bl += 4;

			vdst = _mm_loadu_si128 ((const __m128i *)bl);
			vdst = _mm_add_epi32 (vdst, _mm_unpackhi_epi16 (vlo, vhi));
			_mm_storeu_si128 ((__m128i *)bl, vdst);
			bl += 4;

			lightmap += 8;
			size -= 8;
		}
#elif defined(USE_NEON)
		while (size >= 8)
		{
			uint8x8_t  lm_uint_8x8 = vld1_u8 (lightmap);
			uint16x8_t lm_uint_16x8 = vmovl_u8 (lm_uint_8x8);

			uint32x4_t lm_old_low_4x32bit = vld1q_u32 (bl);
			uint16x4_t lm_uint_low_16x4 = vget_low_u16 (lm_uint_16x8);
			uint32x4_t lm_scaled_accum_low_4x32bit = vmlal_n_u16 (lm_old_low_4x32bit, lm_uint_low_16x4, scale);
			vst1q_u32 (bl, lm_scaled_accum_low_4x32bit);
			bl += 4;

			uint32x4_t lm_old_high_4x32bit = vld1q_u32 (bl);
			uint16x4_t lm_uint_high_16x4 = vget_high_u16 (lm_uint_16x8);
			uint32x4_t lm_scaled_accum_high_4x32bit = vmlal_n_u16 (lm_old_high_4x32bit, lm_uint_high_16x4, scale);
			vst1q_u32 (bl, lm_scaled_accum_high_4x32bit);
			bl += 4;

			lightmap += 8;
			size -= 8;
		}
#endif
	}
#endif

	while (size-- > 0)
		*bl++ += *lightmap++ * scale;
}

/*
===============
R_StoreLightmap

Converts contiguous lightmap info accumulated in 'blocklights'
from RGB32 (with 8 fractional bits) to RGBA8, saturates and
stores the result in 'dest'
===============
*/
void R_StoreLightmap (byte *dest, int width, int height, int stride)
{
	unsigned *src = blocklights;

#if defined(USE_SIMD)
	if (use_simd)
	{
#if defined(USE_SSE2)
		__m128i vzero = _mm_setzero_si128 ();

		while (height-- > 0)
		{
			int i;
			for (i = 0; i < width; i++)
			{
				__m128i v = _mm_srli_epi32 (_mm_loadu_si128 ((const __m128i *)src), 8);
				v = _mm_packs_epi32 (v, vzero);
				v = _mm_packus_epi16 (v, vzero);
				((uint32_t *)dest)[i] = _mm_cvtsi128_si32 (v) | 0xff000000;
				src += 3;
			}
			dest += stride;
		}
#elif defined(USE_NEON)
		while (height-- > 0)
		{
			int i;
			for (i = 0; i < width; i++)
			{
				uint32x4_t lm_32x4 = vld1q_u32 (src);
				uint16x4_t lm_shifted_16x4 = vshrn_n_u32 (lm_32x4, 8);
				uint16x4_t lm_shifted_16x4_masked = vset_lane_u16 (0xFF, lm_shifted_16x4, 3);
				uint16x8_t lm_shifted_16x8 = vcombine_u16 (lm_shifted_16x4_masked, vcreate_u16 (0));
				uint8x8_t  lm_shifted_saturated_8x8 = vqmovn_u16 (lm_shifted_16x8);
				uint32x2_t lm_shifted_saturated_32x2 = vreinterpret_u32_u8 (lm_shifted_saturated_8x8);
				((uint32_t *)dest)[i] = vget_lane_u32 (lm_shifted_saturated_32x2, 0);
				src += 3;
			}
			dest += stride;
		}
#endif
	}
	else
#endif
	{
		stride -= width * 4;
		while (height-- > 0)
		{
			int i;
			for (i = 0; i < width; i++)
			{
				unsigned c;
				c = *src++ >> 8;
				*dest++ = q_min (c, 255);
				c = *src++ >> 8;
				*dest++ = q_min (c, 255);
				c = *src++ >> 8;
				*dest++ = q_min (c, 255);
				*dest++ = 255;
			}
			dest += stride;
		}
	}
}

/*
===============
R_BuildLightMap -- johnfitz -- revised for lit support via lordhavoc

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
void R_BuildLightMap (msurface_t *surf, byte *dest, int stride)
{
	int		 smax, tmax;
	int		 size;
	byte	*lightmap;
	unsigned scale;
	int		 maps;

	surf->cached_dlight = (surf->dlightframe == r_framecount);

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	size = smax * tmax;
	lightmap = surf->samples;

	if (cl.worldmodel->lightdata)
	{
		// clear to no light
		memset (&blocklights[0], 0, size * 3 * sizeof (unsigned int)); // johnfitz -- lit support via lordhavoc

		// add all the lightmaps
		if (lightmap)
		{
			for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
			{
				scale = d_lightstylevalue[surf->styles[maps]];
				surf->cached_light[maps] = scale; // 8.8 fraction
				// johnfitz -- lit support via lordhavoc
				R_AccumulateLightmap (lightmap, scale, size);
				lightmap += size * 3;
				// johnfitz
			}
		}

		// add all the dynamic lights
		if (surf->dlightframe == r_framecount)
			R_AddDynamicLights (surf);
	}
	else
	{
		// set to full bright if no light data
		memset (&blocklights[0], 255, size * 3 * sizeof (unsigned int)); // johnfitz -- lit support via lordhavoc
	}

	R_StoreLightmap (dest, smax, tmax, stride);
}

/*
===============
R_UploadLightmap -- johnfitz -- uploads the modified lightmap to opengl if necessary

assumes lightmap texture is already bound
===============
*/
static void R_UploadLightmap (int lmap, gltexture_t *lightmap_tex)
{
	struct lightmap_s *lm = &lightmaps[lmap];
	qboolean		   modified = false;
	for (int i = 0; i < TASKS_MAX_WORKERS; ++i)
	{
		if (lm->modified[i])
			modified = true;
		lm->modified[i] = 0;
	}
	if (!modified)
		return;

	const int staging_size = LMBLOCK_WIDTH * lm->rectchange.h * 4;

	if (staging_size == 0) // Empty copies are not valid. This can happen for a single frame when toggling r_gpulightmapupdate from 1 to 0
		return;

	VkBuffer		staging_buffer;
	VkCommandBuffer command_buffer;
	int				staging_offset;
	unsigned char  *staging_memory = R_StagingAllocate (staging_size, 4, &command_buffer, &staging_buffer, &staging_offset);

	ZEROED_STRUCT (VkBufferImageCopy, region);
	region.bufferOffset = staging_offset;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.mipLevel = 0;
	region.imageExtent.width = LMBLOCK_WIDTH;
	region.imageExtent.height = lm->rectchange.h;
	region.imageExtent.depth = 1;
	region.imageOffset.y = lm->rectchange.t;

	ZEROED_STRUCT (VkImageMemoryBarrier, image_memory_barrier);
	image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	image_memory_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	image_memory_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	image_memory_barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	image_memory_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	image_memory_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.image = lightmap_tex->image;
	image_memory_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	image_memory_barrier.subresourceRange.baseMipLevel = 0;
	image_memory_barrier.subresourceRange.levelCount = 1;
	image_memory_barrier.subresourceRange.baseArrayLayer = 0;
	image_memory_barrier.subresourceRange.layerCount = 1;

	vulkan_globals.vk_cmd_pipeline_barrier (
		command_buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &image_memory_barrier);

	vulkan_globals.vk_cmd_copy_buffer_to_image (command_buffer, staging_buffer, lightmap_tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	image_memory_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	image_memory_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	image_memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	image_memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vulkan_globals.vk_cmd_pipeline_barrier (
		command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &image_memory_barrier);

	R_StagingBeginCopy ();
	byte *data = lm->data + lm->rectchange.t * LMBLOCK_WIDTH * LIGHTMAP_BYTES;
	if (vulkan_globals.color_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
		for (byte *p = data; p < data + staging_size; p += 4, staging_memory += 4)
			*(unsigned *)staging_memory = p[0] | p[1] << 10 | p[2] << 20;
	else
		memcpy (staging_memory, data, staging_size);
	R_StagingEndCopy ();

	lm->rectchange.l = LMBLOCK_WIDTH;
	lm->rectchange.t = LMBLOCK_HEIGHT;
	lm->rectchange.h = 0;
	lm->rectchange.w = 0;
}

/*
=============
R_FlushUpdateLightmaps
=============
*/
#define UPDATE_LIGHTMAP_BATCH_SIZE 64
void R_FlushUpdateLightmaps (
	cb_context_t *cbx, int num_batch_lightmaps, VkImageMemoryBarrier *pre_barriers, VkImageMemoryBarrier *post_barriers, int *lightmap_indexes,
	byte lightmap_regions[UPDATE_LIGHTMAP_BATCH_SIZE][LMBLOCK_HEIGHT / LM_CULL_BLOCK_H][LMBLOCK_WIDTH / LM_CULL_BLOCK_W], int current_dlights,
	int cached_dlights)
{
	vulkan_pipeline_t *pipeline =
		(r_rtshadows.value && (bmodel_tlas != VK_NULL_HANDLE)) ? &vulkan_globals.update_lightmap_rt_pipeline : &vulkan_globals.update_lightmap_pipeline;

	vkCmdPipelineBarrier (
		cbx->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, num_batch_lightmaps, pre_barriers);
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
	if (pipeline == &vulkan_globals.update_lightmap_rt_pipeline)
	{
		ZEROED_STRUCT (VkWriteDescriptorSetAccelerationStructureKHR, tlas_info);
		tlas_info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
		tlas_info.accelerationStructureCount = 1;
		tlas_info.pAccelerationStructures = &bmodel_tlas;

		ZEROED_STRUCT (VkWriteDescriptorSet, tlas_write);
		tlas_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		tlas_write.pNext = &tlas_info;
		tlas_write.dstBinding = 0;
		tlas_write.descriptorCount = 1;
		tlas_write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

		vulkan_globals.vk_cmd_push_descriptor_set (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 1, 1, &tlas_write);
	}
	uint32_t offsets[2] = {
		current_compute_buffer_index * MAX_LIGHTSTYLES * sizeof (float), current_compute_buffer_index * MAX_DLIGHTS * 2 * sizeof (lm_compute_light_t)};
	for (int j = 0; j < num_batch_lightmaps; ++j)
	{
		VkDescriptorSet sets[1] = {lightmaps[lightmap_indexes[j]].descriptor_set};
		vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout.handle, 0, 1, sets, 2, offsets);
		for (int y = 0; y < LMBLOCK_HEIGHT / LM_CULL_BLOCK_H; y++)
			for (int x = 0; x < LMBLOCK_WIDTH / LM_CULL_BLOCK_W; x++)
				if (lightmap_regions[j][y][x])
				{
					int w = 1;
					int h = 1;
					int type = lightmap_regions[j][y][x];
					while (x + w < LMBLOCK_WIDTH / LM_CULL_BLOCK_W && lightmap_regions[j][y][x + w] == type)
					{
						lightmap_regions[j][y][x + w] = false;
						w += 1;
					}
					while (y + h < LMBLOCK_HEIGHT / LM_CULL_BLOCK_H && lightmap_regions[j][y + h][x] == type)
					{
						qboolean ok = true;
						for (int i = x + 1; i < x + w; i++)
							if (lightmap_regions[j][y + h][i] != type)
							{
								ok = false;
								break;
							}
						if (!ok)
							break;
						if (x > 0 && lightmap_regions[j][y + h][x - 1] == type && x + w < LMBLOCK_WIDTH / LM_CULL_BLOCK_W &&
							lightmap_regions[j][y + h][x + w] == type)
							break; // don't split if it continues both sides, (locally) turns 2 rectangles into 3
						for (int i = x; i < x + w; i++)
							lightmap_regions[j][y + h][i] = false;
						h += 1;
					}
					uint32_t push_constants[12] = {
						current_dlights,
						LMBLOCK_WIDTH,
						x * LM_CULL_BLOCK_W / 8,
						y * LM_CULL_BLOCK_H / 8,
						type == 1,
						cached_dlights,
						current_compute_buffer_index * MAX_MODELS,
						(1 - current_compute_buffer_index) * MAX_MODELS};
					memcpy (&push_constants[8], r_refdef.vieworg, 3 * sizeof (float));
					int push_size = 11 * sizeof (uint32_t);
					if (pipeline == &vulkan_globals.update_lightmap_rt_pipeline)
					{
						uint32_t shadow_samples = 1 << ((int)r_rtshadows.value + 1);
						push_constants[11] = shadow_samples;
						push_size = 12 * sizeof (uint32_t);
					}
					R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push_constants);
					w = q_min (lightmaps[lightmap_indexes[j]].lightstyle_rectused[0].w / 8 - x * LM_CULL_BLOCK_W / 8, w * LM_CULL_BLOCK_W / 8);
					h = q_min (lightmaps[lightmap_indexes[j]].lightstyle_rectused[0].h / 8 - y * LM_CULL_BLOCK_H / 8, h * LM_CULL_BLOCK_H / 8);
					vkCmdDispatch (cbx->cb, w, h, 1);
				}
	}

	vkCmdPipelineBarrier (
		cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, num_batch_lightmaps, post_barriers);
}

/*
=============
R_IndirectComputeDispatch
=============
*/
static void R_IndirectComputeDispatch (cb_context_t *cbx)
{
	if (!indirect)
		return;

	R_BeginDebugUtilsLabel (cbx, "Indirect Compute");

	R_UploadVisibility (cl.worldmodel->surfvis, (cl.worldmodel->numsurfaces + 31) / 8);

	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, vulkan_globals.indirect_clear_pipeline);
	VkDescriptorSet sets[1] = {vulkan_globals.indirect_compute_desc_set};
	vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_COMPUTE, vulkan_globals.indirect_clear_pipeline.layout.handle, 0, 1, sets, 0, NULL);
	R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof (uint32_t), &used_indirect_draws);

	vkCmdDispatch (cbx->cb, (used_indirect_draws + 63) / 64, 1, 1);

	VkMemoryBarrier memory_barrier;
	memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory_barrier.pNext = NULL;
	memory_barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	memory_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier (cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);

	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_COMPUTE, vulkan_globals.indirect_draw_pipeline);
	char push_constants[7 * 4];
	memcpy (push_constants, &cl.model_precache[1]->numsurfaces, sizeof (int));
	memset (push_constants + 4, 0, sizeof (uint32_t));
	uint32_t offset = current_compute_buffer_index * dyn_visibility_offset / 4;
	memcpy (push_constants + 8, &offset, sizeof (uint32_t));
	memcpy (push_constants + 12, r_refdef.vieworg, sizeof (vec3_t));
	const uint32_t instance_base = (uint32_t)bmodel_instances_index * MAX_MODELS;
	memcpy (push_constants + 24, &instance_base, sizeof (uint32_t));
	R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 0, 7 * 4, push_constants);
	const uint32_t num_workgroups = (cl.worldmodel->numsurfaces + 63) / 64;
	const uint32_t max_dispatch = vulkan_globals.device_properties.limits.maxComputeWorkGroupCount[0];
	uint32_t	   start_workgroup = 0;
	while (true)
	{
		vkCmdDispatch (cbx->cb, q_min (max_dispatch, num_workgroups - start_workgroup), 1, 1);
		start_workgroup += max_dispatch;
		if (start_workgroup >= num_workgroups)
			break;
		const uint32_t start_offset = start_workgroup * 64;
		R_PushConstants (cbx, VK_SHADER_STAGE_COMPUTE_BIT, 4, sizeof (uint32_t), &start_offset);
	}

	memory_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory_barrier.pNext = NULL;
	memory_barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	memory_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	vkCmdPipelineBarrier (
		cbx->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &memory_barrier, 0, NULL,
		0, NULL);

	R_EndDebugUtilsLabel (cbx);
}

/*
=============
R_UpdateEmissiveLightmapsOnly
=============
*/
void R_UpdateEmissiveLightmapsOnly (void)
{
	cb_context_t *cbx = &vulkan_globals.primary_cb_contexts[PCBX_UPDATE_LIGHTMAPS];
	R_BeginDebugUtilsLabel (cbx, "Update Emissive Lightmaps");
	R_UpdateEmissiveLightmaps (cbx, false);
	R_UpdateEmissiveLightmaps (cbx, true);
	R_UpdateEmissiveRadiance (cbx);
	R_UpdateTransientEmissiveLightmaps (cbx);
	R_UpdateEmissiveBrushReceiverLightmaps (cbx);
	R_RefreshEmissiveBounceLayers (cbx);
	R_EndDebugUtilsLabel (cbx);
}

/*
=============
R_UpdateLightmapsAndIndirect
=============
*/
void R_UpdateLightmapsAndIndirect (void *unused)
{
	cb_context_t *cbx = &vulkan_globals.primary_cb_contexts[PCBX_UPDATE_LIGHTMAPS];
	R_BeginDebugUtilsLabel (cbx, "Update Lightmaps");
	R_UpdateEmissiveLightmaps (cbx, false);
	R_UpdateEmissiveLightmaps (cbx, true);
	R_UpdateEmissiveRadiance (cbx);
	R_UpdateTransientEmissiveLightmaps (cbx);
	R_UpdateEmissiveBrushReceiverLightmaps (cbx);
	R_RefreshEmissiveBounceLayers (cbx);

	for (int i = 0; i < MAX_LIGHTSTYLES; ++i)
	{
		float *style = lightstyles_scales_buffer_mapped + i + (current_compute_buffer_index * MAX_LIGHTSTYLES);
		*style = (float)d_lightstylevalue[i] / 256.0f;
	}

	static lm_compute_light_t cached_dlights[MAX_DLIGHTS];
	static int				  num_cached_dlights;

	memcpy (
		lights_buffer_mapped + (current_compute_buffer_index * MAX_DLIGHTS * 2) + MAX_DLIGHTS, cached_dlights,
		sizeof (lm_compute_light_t) * num_cached_dlights);

	int	  num_used_dlights = 0;
	int	  used_dlights[MAX_DLIGHTS];
	float squared_radius[MAX_DLIGHTS];
	for (int i = 0; i < MAX_DLIGHTS; ++i)
	{
		lm_compute_light_t *light = &cached_dlights[num_used_dlights];
		if (!r_dynamic.value || cl_dlights[i].die < cl.time || cl_dlights[i].radius == 0.0f || (cl_dlights[i].radius < cl_dlights[i].minlight))
			continue;
		VectorCopy (cl_dlights[i].origin, light->origin);
		light->radius = cl_dlights[i].radius;
		VectorCopy (cl_dlights[i].color, light->color);
		// rerelease dynamiclights don't use minlight, so its sign packs the KEX intensity
		light->minlight = (cl_dlights[i].kex_intensity > 0.0f) ? -cl_dlights[i].kex_intensity : cl_dlights[i].minlight;
		VectorCopy (cl_dlights[i].cone_dir, light->cone_dir);
		light->cone_cos = cl_dlights[i].cone_cos;
		squared_radius[num_used_dlights] = cl_dlights[i].radius * cl_dlights[i].radius;
		used_dlights[num_used_dlights++] = i;
	}
	memcpy (lights_buffer_mapped + (current_compute_buffer_index * MAX_DLIGHTS * 2), cached_dlights, sizeof (lm_compute_light_t) * num_used_dlights);

	// Movable brush submodels are lit in entity space: upload the current model to world transform for each submodel.
	// The GPU culls dlights against the transformed workgroup bounds, the CPU only schedules updates for the cull
	// blocks containing submodel surfaces while dlights are active. The transforms are only read while dlight
	// updates are dispatched, skip all of it when no dlights are active
	const qboolean any_dlight_updates = (num_used_dlights > 0) || (num_cached_dlights > 0);
	if (any_dlight_updates)
	{
		float *transforms = submodel_transforms_buffer_mapped + ((size_t)current_compute_buffer_index * MAX_MODELS * 12);
		for (int i = 0; i < num_worldmodel_submodels; ++i)
		{
			float *transform_rows = transforms + (i * 12);
			memset (transform_rows, 0, 12 * sizeof (float));
			transform_rows[0] = transform_rows[5] = transform_rows[10] = 1.0f;
		}
		for (int i = 0; i < cl.num_entities + cl.num_statics; ++i)
		{
			entity_t *e = (i < cl.num_entities) ? &cl.entities[i] : cl.static_entities[i - cl.num_entities];
			if (!e->model || e->model->needload || (e->model->name[0] != '*') || (e->model->surfaces != cl.worldmodel->surfaces))
				continue;
			const int submodel = atoi (e->model->name + 1);
			if ((submodel <= 0) || (submodel >= num_worldmodel_submodels))
				continue;

			vec3_t angles;
			VectorCopy (e->angles, angles);
			angles[0] = -angles[0]; // stupid quake bug
			float model_matrix[16];
			IdentityMatrix (model_matrix);
			R_RotateForEntity (model_matrix, e->origin, angles, e->netstate.scale);

			float *transform_rows = transforms + (submodel * 12);
			for (int row = 0; row < 3; ++row)
				for (int col = 0; col < 4; ++col)
					transform_rows[(row * 4) + col] = model_matrix[(col * 4) + row];
		}
	}

	int					 num_lightmaps = 0;
	int					 num_batch_lightmaps = 0;
	VkImageMemoryBarrier pre_lm_image_barriers[UPDATE_LIGHTMAP_BATCH_SIZE];
	VkImageMemoryBarrier post_lm_image_barriers[UPDATE_LIGHTMAP_BATCH_SIZE];
	int					 lightmap_indexes[UPDATE_LIGHTMAP_BATCH_SIZE];
	byte				 lightmap_regions[UPDATE_LIGHTMAP_BATCH_SIZE][LMBLOCK_HEIGHT / LM_CULL_BLOCK_H][LMBLOCK_WIDTH / LM_CULL_BLOCK_W];

	for (int lightmap_index = 0; lightmap_index < lightmap_count; ++lightmap_index)
	{
		struct lightmap_s *lm = &lightmaps[lightmap_index];
		uint32_t		   modified = 0;
		byte			   regions[LMBLOCK_HEIGHT / LM_CULL_BLOCK_H][LMBLOCK_WIDTH / LM_CULL_BLOCK_W]; // 1: dlights update only; 2: unconditional update
		memset (regions, 0, sizeof (regions));
		for (int i = 0; i < TASKS_MAX_WORKERS; ++i)
		{
			modified |= lm->modified[i];
			lm->modified[i] = 0;
		}
		if (modified == 0)
			continue;

		qboolean any_needs_dlight_update = false;
		uint32_t used_lightstyles = 0;
		int		 num_blocks = 0;
		for (int y = 0; y < LMBLOCK_HEIGHT / LM_CULL_BLOCK_H; y++)
			for (int x = 0; x < LMBLOCK_WIDTH / LM_CULL_BLOCK_W; x++)
			{
				qboolean needs_update = false;
				for (int i = 0; i < num_used_dlights; i++)
				{
					float sq_dist = 0.0f;
					for (int j = 0; j < 3; j++)
					{
						float v = cl_dlights[used_dlights[i]].origin[j];
						float mins = lm->global_bounds[y][x].mins[j];
						float maxs = lm->global_bounds[y][x].maxs[j];

						if (v < mins)
							sq_dist += (mins - v) * (mins - v);
						if (v > maxs)
							sq_dist += (v - maxs) * (v - maxs);

						if (sq_dist > squared_radius[i])
							break;
					}

					if (sq_dist <= squared_radius[i])
					{
						lm->active_dlights[y][x] = true;
						needs_update = true;
					}
				}
				if (any_dlight_updates && lm->block_has_submodels[y][x])
					needs_update = true;
				if (!needs_update && lm->active_dlights[y][x])
				{
					lm->active_dlights[y][x] = false;
					needs_update = true;
				}
				if (needs_update)
				{
					any_needs_dlight_update = true;
					if (lm->cached_framecount == r_framecount - 1)
						regions[y][x] = 1;
					else
						regions[y][x] = 2;
					num_blocks += 1;
				}
				if (regions[y][x] != 2)
					for (int i = 0; i < lm->num_used_lightstyles[y][x]; i++)
					{
						int l = lm->used_lightstyles[y][x][i];
						if (lm->cached_light[l] != d_lightstylevalue[l])
						{
							if (regions[y][x] == 0)
								num_blocks += 1;
							regions[y][x] = 2;
							if (!any_needs_dlight_update)
								used_lightstyles |= 1 << (l < 16 ? l : l % 16 + 16);
							else
								break;
						}
					}
			}
		if (!any_needs_dlight_update && !(used_lightstyles & modified))
			continue;
		else
		{
			for (int i = 0; i < MAX_LIGHTSTYLES; i++)
				lm->cached_light[i] = d_lightstylevalue[i];
			lm->cached_framecount = r_framecount;
			num_lightmaps += num_blocks;
		}

		int batch_index = num_batch_lightmaps++;
		lightmap_indexes[batch_index] = lightmap_index;
		memcpy (lightmap_regions[batch_index], regions, sizeof (regions));

		VkImageMemoryBarrier *pre_barrier = &pre_lm_image_barriers[batch_index];
		pre_barrier->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		pre_barrier->pNext = NULL;
		pre_barrier->srcAccessMask = 0;
		pre_barrier->dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		pre_barrier->oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		pre_barrier->newLayout = VK_IMAGE_LAYOUT_GENERAL;
		pre_barrier->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre_barrier->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre_barrier->image = lm->texture->image;
		pre_barrier->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		pre_barrier->subresourceRange.baseMipLevel = 0;
		pre_barrier->subresourceRange.levelCount = 1;
		pre_barrier->subresourceRange.baseArrayLayer = 0;
		pre_barrier->subresourceRange.layerCount = 1;

		VkImageMemoryBarrier *post_barrier = &post_lm_image_barriers[batch_index];
		post_barrier->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		post_barrier->pNext = NULL;
		post_barrier->srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		post_barrier->dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		post_barrier->oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		post_barrier->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		post_barrier->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		post_barrier->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		post_barrier->image = lm->texture->image;
		post_barrier->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		post_barrier->subresourceRange.baseMipLevel = 0;
		post_barrier->subresourceRange.levelCount = 1;
		post_barrier->subresourceRange.baseArrayLayer = 0;
		post_barrier->subresourceRange.layerCount = 1;

		if (num_batch_lightmaps == UPDATE_LIGHTMAP_BATCH_SIZE)
		{
			R_FlushUpdateLightmaps (
				cbx, num_batch_lightmaps, pre_lm_image_barriers, post_lm_image_barriers, lightmap_indexes, lightmap_regions, num_used_dlights,
				num_cached_dlights);
			num_batch_lightmaps = 0;
		}
	}

	if (num_batch_lightmaps > 0)
		R_FlushUpdateLightmaps (
			cbx, num_batch_lightmaps, pre_lm_image_barriers, post_lm_image_barriers, lightmap_indexes, lightmap_regions, num_used_dlights, num_cached_dlights);

	num_cached_dlights = num_used_dlights;

	Atomic_AddUInt32 (&rs_dynamiclightmaps, num_lightmaps);

	R_EndDebugUtilsLabel (cbx);

	R_IndirectComputeDispatch (cbx);

	current_compute_buffer_index = (current_compute_buffer_index + 1) % 2;
}

void R_UploadLightmaps (void)
{
	int lmap;
	int num_uploads = 0;

	for (lmap = 0; lmap < lightmap_count; lmap++)
	{
		qboolean modified = false;
		for (int i = 0; i < TASKS_MAX_WORKERS; ++i)
			modified = modified || lightmaps[lmap].modified[i];
		if (!modified)
			continue;

		++num_uploads;
		R_UploadLightmap (lmap, lightmaps[lmap].texture);
	}

	Atomic_AddUInt32 (&rs_dynamiclightmaps, num_uploads);
}
