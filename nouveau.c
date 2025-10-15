/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifdef DRV_NOUVEAU

#include <assert.h>
#include <dlfcn.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <nouveau_drm.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

#include "external/nouveau_drm.h"
#include "drv_helpers.h"
#include "drv_priv.h"
#include "util.h"

/* From drm_fourcc.h
 *
 * Generalized Block Linear layout, used by desktop GPUs starting with NV50/G80,
 * and Tegra GPUs starting with Tegra K1.
 *
 * Pixels are arranged in Groups of Bytes (GOBs).  GOB size and layout varies
 * based on the architecture generation.  GOBs themselves are then arranged in
 * 3D blocks, with the block dimensions (in terms of GOBs) always being a power
 * of two, and hence expressible as their log2 equivalent (E.g., "2" represents
 * a block depth or height of "4").
 *
 * Chapter 20 "Pixel Memory Formats" of the Tegra X1 TRM describes this format
 * in full detail.
 *
 *       Macro
 * Bits  Param Description
 * ----  ----- -----------------------------------------------------------------
 *
 *  3:0  h     log2(height) of each block, in GOBs.  Placed here for
 *             compatibility with the existing
 *             DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK()-based modifiers.
 *
 *  4:4  -     Must be 1, to indicate block-linear layout.  Necessary for
 *             compatibility with the existing
 *             DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK()-based modifiers.
 *
 *  8:5  -     Reserved (To support 3D-surfaces with variable log2(depth) block
 *             size).  Must be zero.
 *
 *             Note there is no log2(width) parameter.  Some portions of the
 *             hardware support a block width of two gobs, but it is impractical
 *             to use due to lack of support elsewhere, and has no known
 *             benefits.
 *
 * 11:9  -     Reserved (To support 2D-array textures with variable array stride
 *             in blocks, specified via log2(tile width in blocks)).  Must be
 *             zero.
 *
 * 19:12 k     Page Kind.  This value directly maps to a field in the page
 *             tables of all GPUs >= NV50.  It affects the exact layout of bits
 *             in memory and can be derived from the tuple
 *
 *               (format, GPU model, compression type, samples per pixel)
 *
 *             Where compression type is defined below.  If GPU model were
 *             implied by the format modifier, format, or memory buffer, page
 *             kind would not need to be included in the modifier itself, but
 *             since the modifier should define the layout of the associated
 *             memory buffer independent from any device or other context, it
 *             must be included here.
 *
 * 21:20 g     GOB Height and Page Kind Generation.  The height of a GOB changed
 *             starting with Fermi GPUs.  Additionally, the mapping between page
 *             kind and bit layout has changed at various points.
 *
 *               0 = Gob Height 8, Fermi - Volta, Tegra K1+ Page Kind mapping
 *               1 = Gob Height 4, G80 - GT2XX Page Kind mapping
 *               2 = Gob Height 8, Turing+ Page Kind mapping
 *               3 = Reserved for future use.
 *
 * 22:22 s     Sector layout.  There is a further bit remapping step that occurs
 * 26:27       at an even lower level than the page kind and block linear
 *             swizzles.  This causes the bit arrangement of surfaces in memory
 *             to differ subtly, and prevents direct sharing of surfaces between
 *             GPUs with different layouts.
 *
 *               0 = Tegra K1 - Tegra Parker/TX2 Layout
 *               1 = Pre-GB20x, GB20x 32+ bpp, GB10, Tegra Xavier-Orin Layout
 *               2 = GB20x(Blackwell 2)+ 8 bpp surface layout
 *               3 = GB20x(Blackwell 2)+ 16 bpp surface layout
 *               4 = Reserved for future use.
 *               5 = Reserved for future use.
 *               6 = Reserved for future use.
 *               7 = Reserved for future use.
 *
 * 25:23 c     Lossless Framebuffer Compression type.
 *
 *               0 = none
 *               1 = ROP/3D, layout 1, exact compression format implied by Page
 *                   Kind field
 *               2 = ROP/3D, layout 2, exact compression format implied by Page
 *                   Kind field
 *               3 = CDE horizontal
 *               4 = CDE vertical
 *               5 = Reserved for future use
 *               6 = Reserved for future use
 *               7 = Reserved for future use
 *
 * 55:28 -     Reserved for future use.  Must be zero.
 */
#define NOUVEAU_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(c, s, g, k, h) \
	fourcc_mod_code(NVIDIA, (0x10 | \
				 ((h) & 0xf) | \
				 (((k) & 0xff) << 12) | \
				 (((g) & 0x3) << 20) | \
				 (((s) & 0x1) << 22) | \
				 (((s) & 0x6) << 25) | \
				 (((c) & 0x7) << 23)))

#define NOUVEAU_GOB_HEIGHT_KIND_FERMI 0
#define NOUVEAU_GOB_HEIGHT_KIND_G80 1
#define NOUVEAU_GOB_HEIGHT_KIND_TURING 2

#define NOUVEAU_GOB_SECTOR_LAYOUT_TEGRA 0
#define NOUVEAU_GOB_SECTOR_LAYOUT_FERMI 1
#define NOUVEAU_GOB_SECTOR_LAYOUT_BLACKWELL_8 2
#define NOUVEAU_GOB_SECTOR_LAYOUT_BLACKWELL_16 3

enum nouveau_driver {
	NOUVEAU_DRIVER_UNKNOWN,
	NOUVEAU_DRIVER_NOUVEAU,
	NOUVEAU_DRIVER_TEGRA,
};

enum nouveau_bus_type {
	NOUVEAU_BUS_TYPE_AGP = 0,
	NOUVEAU_BUS_TYPE_PCI = 1,
	NOUVEAU_BUS_TYPE_PCIE = 2,
	NOUVEAU_BUS_TYPE_SOC = 3,
};

struct nouveau_device_info {
	enum nouveau_bus_type bus_type;
	uint16_t chipset;
};

struct nouveau_device {
	enum nouveau_driver driver;
	struct nouveau_device_info info;
	int nouveau_fd;
};

static bool
nouveau_wants_tegra_display(const struct nouveau_device_info *devinfo)
{
	return devinfo->bus_type == NOUVEAU_BUS_TYPE_SOC &&
	       devinfo->chipset < 0x14b /* Turing A */;
}

/* We only care about 2D images so we can just use +1 for the tiling */
#define NOUVEAU_TILING(y_log2) ((y_log2) + 1)

#define NOUVEAU_GOB_WIDTH_B 64
#define NOUVEAU_GOB_HEIGHT 8

static uint32_t
nouveau_tiling_y_log2(uint32_t tiling)
{
	assert(tiling > 0);
	return tiling - 1;
}

static uint32_t
nouveau_tiling_block_width_B(uint32_t tiling)
{
	if (tiling == 0)
		return 1;

	return NOUVEAU_GOB_WIDTH_B;
}

static uint32_t
nouveau_tiling_block_height(uint32_t tiling)
{
	if (tiling == 0)
		return 1;

	return NOUVEAU_GOB_HEIGHT << nouveau_tiling_y_log2(tiling);
}

static bool
nouveau_tiling_valid(uint32_t tiling, uint32_t height)
{
	if (tiling == 0)
		return true;

	/* We can never go smaller than 1 GOB height */
	if (tiling == 1)
		return true;

	return nouveau_tiling_block_height(tiling) < height * 2;
}

static uint16_t
nouveau_tiling_tile_mode(uint32_t tiling)
{
	if (tiling == 0)
		return 0;

	return nouveau_tiling_y_log2(tiling) << 4;
}

static uint8_t
nouveau_choose_pte_kind(const struct nouveau_device_info *devinfo)
{
	if (devinfo->chipset >= 0x14b /* Turing A */) {
		return 0x6; /* NV_MMU_PTE_KIND_GENERIC_MEMORY */
	} else {
		return 0xfe; /* NV_MMU_PTE_KIND_GENERIC_16BX2 */
	}
}

static uint64_t
nouveau_get_modifier(struct nouveau_device *nvdev,
		     uint32_t cpp, uint32_t tiling,
		     uint32_t pte_kind)
{
	if (tiling == 0)
		return DRM_FORMAT_MOD_LINEAR;

	const uint32_t y_log2 = nouveau_tiling_y_log2(tiling);

	/* The Tegra driver only supports the old modifiers */
	if (nvdev->driver == NOUVEAU_DRIVER_TEGRA) {
		assert(pte_kind == 0xfe /* NV_MMU_PTE_KIND_GENERIC_16BX2 */);
		return DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(y_log2);
	}

	uint32_t gob_height_kind_gen, sector_layout;
	if (nvdev->info.chipset >= 0x1a0 /* Blackwell A */) {
		gob_height_kind_gen = NOUVEAU_GOB_HEIGHT_KIND_TURING;
		if (cpp == 1)
			sector_layout = NOUVEAU_GOB_SECTOR_LAYOUT_BLACKWELL_8;
		else if (cpp == 2)
			sector_layout = NOUVEAU_GOB_SECTOR_LAYOUT_BLACKWELL_16;
		else
			sector_layout = NOUVEAU_GOB_SECTOR_LAYOUT_FERMI;
	} else if (nvdev->info.chipset >= 0x14b /* Turing A */) {
		gob_height_kind_gen = NOUVEAU_GOB_HEIGHT_KIND_TURING;
		sector_layout = NOUVEAU_GOB_SECTOR_LAYOUT_FERMI;
	} else if (nvdev->info.bus_type == NOUVEAU_BUS_TYPE_SOC) {
		gob_height_kind_gen = NOUVEAU_GOB_HEIGHT_KIND_FERMI;
		sector_layout = NOUVEAU_GOB_SECTOR_LAYOUT_TEGRA;
	} else {
		gob_height_kind_gen = NOUVEAU_GOB_HEIGHT_KIND_FERMI;
		sector_layout = NOUVEAU_GOB_SECTOR_LAYOUT_FERMI;
	}

	return NOUVEAU_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0 /* Uncompressed */,
							 sector_layout,
							 gob_height_kind_gen,
							 pte_kind, y_log2);
}

static uint32_t
nouveau_modifier_to_tiling(uint64_t modifier)
{
	if (modifier == DRM_FORMAT_MOD_LINEAR)
		return 0;

	const uint32_t y_log2 = modifier & 0xf;
	return NOUVEAU_TILING(y_log2);
}

/** Returns true if a is better than b */
static bool
nouveau_is_modifier_better(uint64_t a_mod, uint64_t b_mod)
{
	if (a_mod == DRM_FORMAT_MOD_INVALID)
		return false;

	if (b_mod == DRM_FORMAT_MOD_INVALID)
		return true;

	const uint32_t a_tiling = nouveau_modifier_to_tiling(a_mod);
	const uint32_t b_tiling = nouveau_modifier_to_tiling(b_mod);

	return a_tiling > b_tiling;
}

static uint64_t
nouveau_choose_modifier(struct driver *drv, uint32_t format, uint32_t height,
			const uint64_t *modifiers, uint32_t modifier_count)
{
	struct nouveau_device *nvdev = drv->priv;
	const uint8_t pte_kind = nouveau_choose_pte_kind(&nvdev->info);
	const uint32_t cpp = drv_bytes_per_pixel_from_format(format, 0);

	uint64_t best_modifier = DRM_FORMAT_MOD_INVALID;
	uint32_t smallest_tiling = UINT32_MAX;
	for (uint32_t i = 0; i < modifier_count; i++) {
		const uint32_t tiling = nouveau_modifier_to_tiling(modifiers[i]);

		/* Re-create the modifier to ensure it's the right one for our device. */
		const uint64_t modifier =
			nouveau_get_modifier(nvdev, cpp, tiling, pte_kind);
		if (modifiers[i] != modifier)
			continue;

		smallest_tiling = MIN(smallest_tiling, tiling);

		/* Reject modifiers with too big a tile size for the first pass */
		if (!nouveau_tiling_valid(tiling, height))
			continue;

		if (nouveau_is_modifier_better(modifier, best_modifier))
			best_modifier = modifier;
	}

	if (best_modifier != DRM_FORMAT_MOD_INVALID)
		return best_modifier;

	if (smallest_tiling == UINT32_MAX) {
		drv_loge("No supported modifier found\n");
		return DRM_FORMAT_MOD_INVALID;
	} else {
		const uint64_t modifier =
			nouveau_get_modifier(nvdev, cpp, smallest_tiling, pte_kind);
		drv_logd("No modifier found with suitible tile height. Falling "
			 "back to %"PRIx64" (block_height = %d)\n",
			 modifier, nouveau_tiling_block_height(smallest_tiling));

		return modifier;
	}
}

static void
nouveau_calculate_layout(struct bo *bo)
{
	const uint32_t width = bo->meta.width;
	const uint32_t height = bo->meta.height;
	const uint32_t stride = drv_stride_from_format(bo->meta.format, width, 0);

	uint32_t stride_align, height_align;
	if (bo->meta.tiling == 0) {
		/* Linear requires 128B alignment for render */
		stride_align = 128;
		height_align = 1;
	} else {
		stride_align = nouveau_tiling_block_width_B(bo->meta.tiling);
		height_align = nouveau_tiling_block_height(bo->meta.tiling);
	};

	drv_bo_from_format(bo, stride, stride_align,
			   ALIGN(height, height_align),
			   bo->meta.format);
}

static enum nouveau_driver
nouveau_fd_get_driver(int fd)
{
	enum nouveau_driver driver = NOUVEAU_DRIVER_UNKNOWN;
	drmVersionPtr version;

	version = drmGetVersion(fd);
	if (!version)
		return driver;

	if (strncmp("nouveau", version->name, version->name_len) == 0)
		driver = NOUVEAU_DRIVER_NOUVEAU;
	else if (strncmp("tegra", version->name, version->name_len) == 0)
		driver = NOUVEAU_DRIVER_TEGRA;
	free(version);

	return driver;
}

static int
nouveau_device_open(struct driver *drv, struct nouveau_device *nvdev)
{
	drmDevicePtr devs[64];
	int dev_count;

	dev_count = drmGetDevices2(0, devs, ARRAY_SIZE(devs));
	for (int i = 0; i < dev_count; i++) {
		drmDevicePtr dev = devs[i];

		if (!(dev->available_nodes & (1 << DRM_NODE_RENDER)))
			continue;

		int fd = open(dev->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		if (nouveau_fd_get_driver(fd) == NOUVEAU_DRIVER_NOUVEAU) {
			drv_logd("Found nouveau render node at %s\n",
				 dev->nodes[DRM_NODE_RENDER]);
			nvdev->nouveau_fd = fd;
			return 0;
		}

		close(fd);
	}

	return -ENOENT;
}

static int
nouveau_device_fd(struct driver *drv)
{
	struct nouveau_device *nvdev = drv->priv;
	if (nvdev->nouveau_fd >= 0)
		return nvdev->nouveau_fd;

	return drv->fd;
}

static int
nouveau_get_device_info(int fd, struct nouveau_device_info *devinfo)
{
	int err;

	memset(devinfo, 0, sizeof(devinfo));

	struct drm_nouveau_getparam bus_type_param = {
		.param = NOUVEAU_GETPARAM_BUS_TYPE,
	};
	err = drmCommandWriteRead(fd, DRM_NOUVEAU_GETPARAM,
				  &bus_type_param, sizeof(bus_type_param));
	if (err)
		return err;

	struct drm_nouveau_getparam chipset_param = {
		.param = NOUVEAU_GETPARAM_CHIPSET_ID,
	};
	err = drmCommandWriteRead(fd, DRM_NOUVEAU_GETPARAM,
				  &chipset_param, sizeof(chipset_param));
	if (err)
		return err;

	devinfo->bus_type = bus_type_param.value;
	devinfo->chipset = chipset_param.value;

	return 0;
}

bool nouveau_has_display(int fd);

bool
nouveau_has_display(int fd)
{
	struct nouveau_device_info devinfo;
	return !nouveau_get_device_info(fd, &devinfo) &&
	       !nouveau_wants_tegra_display(&devinfo);
}

static const uint32_t render_target_formats[] = {
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ABGR2101010,
	DRM_FORMAT_ABGR16161616F,
};

static const uint32_t texture_source_formats[] = {
	DRM_FORMAT_R8,
	DRM_FORMAT_NV12,
	DRM_FORMAT_YVU420,
	DRM_FORMAT_YVU420_ANDROID,
	DRM_FORMAT_P010,
};

static void
nouveau_add_format_combinations(struct driver *drv, uint32_t format,
				uint64_t use_flags)
{
	struct nouveau_device *nvdev = drv->priv;
	const uint8_t pte_kind = nouveau_choose_pte_kind(&nvdev->info);
	const uint32_t cpp = drv_bytes_per_pixel_from_format(format, 0);

	for (uint32_t y_log2 = 0; y_log2 <= 5; y_log2++) {
		const uint32_t tiling = NOUVEAU_TILING(y_log2);
		const uint64_t modifier =
			nouveau_get_modifier(nvdev, cpp, tiling, pte_kind);

		struct format_metadata metadata = {
			.priority = 2 + y_log2, /* Prefer larger tiles */
			.tiling = tiling,
			.modifier = modifier,
		};
		drv_add_combination(drv, format, &metadata, use_flags);
	}
}

static void
nouveau_add_combinations(struct driver *drv)
{
	struct nouveau_device *nvdev = drv->priv;

	const uint64_t render_use_flags =
		BO_USE_RENDER_MASK | BO_USE_SCANOUT | BO_USE_FRONT_RENDERING;
	uint64_t texture_use_flags =
		BO_USE_TEXTURE_MASK | BO_USE_HW_VIDEO_DECODER;
	const uint64_t sw_flags =
		(BO_USE_RENDERSCRIPT | BO_USE_SW_MASK | BO_USE_LINEAR);

	drv_add_combinations(drv, render_target_formats,
			     ARRAY_SIZE(render_target_formats),
			     &LINEAR_METADATA, render_use_flags);

	drv_add_combinations(drv, texture_source_formats,
			     ARRAY_SIZE(texture_source_formats),
			     &LINEAR_METADATA, texture_use_flags);

	for (uint32_t i = 0; i < ARRAY_SIZE(render_target_formats); i++) {
		nouveau_add_format_combinations(drv, render_target_formats[i],
						render_use_flags & ~sw_flags);
	}

	for (uint32_t i = 0; i < ARRAY_SIZE(texture_source_formats); i++) {
		nouveau_add_format_combinations(drv, texture_source_formats[i],
						texture_use_flags & ~sw_flags);
	}
}

static void
nouveau_device_destroy(struct nouveau_device *nvdev)
{
	if (nvdev->nouveau_fd >= 0)
		close(nvdev->nouveau_fd);
	free(nvdev);
}

static int
nouveau_init(struct driver *drv)
{
	struct nouveau_device *nvdev;
	int err = 0;

	nvdev = calloc(1, sizeof(*nvdev));
	nvdev->driver = NOUVEAU_DRIVER_NOUVEAU;
	nvdev->nouveau_fd = -1;

	drv->priv = nvdev;

	err = nouveau_get_device_info(drv->fd, &nvdev->info);
	if (err)
		goto fail;

	/* Fail to enumerate on the nouveau node if we're on a Tegra Xavier
	 * (Volta-based) or earlier. Those uses the tegra driver for display
	 * and we want gralloc and gbm to enumerate that one so SurfaceFlinger
	 * doesn't blow up when it fails to find a display.
	 */
	if (nouveau_wants_tegra_display(&nvdev->info)) {
		drv_logd("Tried to initialize nouveau on tegra\n");
		err = -EINVAL;
		goto fail;
	}

	nouveau_add_combinations(drv);

	return 0;

fail:
	nouveau_device_destroy(drv->priv);
	drv->priv = NULL;

	return err;
}

static int
tegra_init(struct driver *drv)
{
	struct nouveau_device *nvdev;
	int err = 0;

	nvdev = calloc(1, sizeof(*nvdev));
	nvdev->driver = NOUVEAU_DRIVER_TEGRA;
	nvdev->nouveau_fd = -1;

	drv->priv = nvdev;

	err = nouveau_device_open(drv, nvdev);
	if (err)
		goto fail;

	err = nouveau_get_device_info(nvdev->nouveau_fd, &nvdev->info);
	if (err)
		goto fail;

	nouveau_add_combinations(drv);

	return 0;

fail:
	nouveau_device_destroy(drv->priv);
	drv->priv = NULL;

	return err;
}

static void
nouveau_close(struct driver *drv)
{
	nouveau_device_destroy(drv->priv);
	drv->priv = NULL;
}

static int
nouveau_bo_create_for_modifier(struct bo *bo, uint32_t width, uint32_t height,
			       uint32_t format, const uint64_t modifier)
{
	struct nouveau_device *nvdev = bo->drv->priv;

	const uint32_t tiling = nouveau_modifier_to_tiling(modifier);
	const uint32_t pte_kind = nouveau_choose_pte_kind(&nvdev->info);
	const uint32_t tile_mode = nouveau_tiling_tile_mode(tiling);

	bo->meta.tiling = tiling;
	nouveau_calculate_layout(bo);

	uint32_t domain = 0;
	if (nvdev->info.bus_type == NOUVEAU_BUS_TYPE_SOC) {
		domain = NOUVEAU_GEM_DOMAIN_GART;
	} else {
		if (bo->meta.use_flags & BO_USE_SW_READ_OFTEN) {
			domain = NOUVEAU_GEM_DOMAIN_GART;
		} else {
			domain = NOUVEAU_GEM_DOMAIN_GART |
				 NOUVEAU_GEM_DOMAIN_VRAM;
		}
	}

	if (bo->meta.use_flags & (BO_USE_SW_READ_OFTEN |
				  BO_USE_SW_READ_RARELY |
				  BO_USE_SW_WRITE_OFTEN	|
				  BO_USE_SW_WRITE_RARELY))
		domain |= NOUVEAU_GEM_DOMAIN_MAPPABLE;

	const uint64_t bo_size = ALIGN(bo->meta.total_size, 0x1000);

	const char format_fourcc[5] = {
	    format >> 0,
	    format >> 8,
	    format >> 16,
	    format >> 24,
	    0,
	};
	drv_logd("Allocating new BO: %dx%d, fourcc = %s, size = %"PRId64" "
		 "domain = 0x%x, pte_kind = 0x%x, "
		 "tile_mode = 0x%x, modifier = 0x%"PRIx64"\n",
		 width, height, format_fourcc, bo_size,
		 domain, pte_kind, tile_mode, modifier);

	struct drm_nouveau_gem_new req = {
		.info = {
			.domain = domain,
			.tile_flags = pte_kind << 8,
			.tile_mode = tile_mode,
			.size = bo_size,
		},
	};
	int err = drmCommandWriteRead(nouveau_device_fd(bo->drv),
				      DRM_NOUVEAU_GEM_NEW,
				      &req, sizeof(req));
	if (err) {
		drv_loge("DRM_NOUVEAU_GEM_NEW failed with %s\n", strerror(errno));
		return -errno;
	}

	/* If it was allocated on the nouveau FD, move to the tegra fd */
	if (nvdev->nouveau_fd >= 0) {
		struct drm_gem_close gem_close = {
			.handle = req.info.handle,
		};

		int prime_fd;
		err = drmPrimeHandleToFD(nvdev->nouveau_fd, req.info.handle,
					 DRM_CLOEXEC | DRM_RDWR, &prime_fd);
		int close_err = drmIoctl(nvdev->nouveau_fd,
					 DRM_IOCTL_GEM_CLOSE, &gem_close);
		if (close_err) {
			drv_loge("DRM_GEM_CLOSE failed with %s\n", strerror(errno));
			return -errno;
		}
		if (err) {
			drv_loge("drmPrimeHandleToFD() failed with %s\n", strerror(errno));
			return -errno;
		}

		err = drmPrimeFDToHandle(bo->drv->fd, prime_fd, &req.info.handle);
		close(prime_fd);
		if (err) {
			drv_loge("drmPrimeFDToHandle() failed with %s\n", strerror(errno));
			return -errno;
		}
	}

	bo->handle.u32 = req.info.handle;
	bo->meta.format_modifier = modifier;

	return 0;
}

static int
nouveau_bo_create_with_modifiers(struct bo *bo, uint32_t width,
				 uint32_t height, uint32_t format,
				 const uint64_t *modifiers,
				 uint32_t modifier_count)
{
	const uint64_t modifier =
		nouveau_choose_modifier(bo->drv, format, height,
					modifiers, modifier_count);
	if (modifier == DRM_FORMAT_MOD_INVALID) {
		drv_loge("Invalid modifier list\n");
		return -EINVAL;
	}

	return nouveau_bo_create_for_modifier(bo, width, height, format, modifier);
}

/* nouveau_bo_create will create linear buffers for now */
static int
nouveau_bo_create(struct bo *bo, uint32_t width, uint32_t height,
		  uint32_t format, uint64_t flags)
{
	struct combination *combo = drv_get_combination(bo->drv, format, flags);

	if (!combo) {
		drv_loge("invalid format = %d, flags = %" PRIx64 " combination\n", format, flags);
		return -EINVAL;
	}

	return nouveau_bo_create_for_modifier(bo, width, height, format,
					      combo->metadata.modifier);
}

static void *
nouveau_bo_map(struct bo *bo, struct vma *vma, uint32_t map_flags)
{
	struct drm_nouveau_gem_info info = {
		.handle = bo->handle.u32,
	};
	int err = drmCommandWriteRead(nouveau_device_fd(bo->drv),
				      DRM_NOUVEAU_GEM_INFO,
				      &info, sizeof(info));
	if (err) {
		drv_loge("DRM_NOUVEAU_GEM_INFO failed with %s\n", strerror(errno));
		return MAP_FAILED;
	}

	vma->length = bo->meta.total_size;

	return mmap(0, bo->meta.total_size, drv_get_prot(map_flags), MAP_SHARED,
		    nouveau_device_fd(bo->drv), info.map_handle);
}

const struct backend backend_nouveau = {
	.name = "nouveau",
	.init = nouveau_init,
	.close = nouveau_close,
	.bo_create = nouveau_bo_create,
	.bo_create_with_modifiers = nouveau_bo_create_with_modifiers,
	.bo_destroy = drv_gem_bo_destroy,
	.bo_import = drv_prime_bo_import,
	.bo_map = nouveau_bo_map,
	.bo_unmap = drv_bo_munmap,
	.resolve_format_and_use_flags = drv_resolve_format_and_use_flags_helper,
};

const struct backend backend_tegra = {
	.name = "tegra",
	.init = tegra_init,
	.close = nouveau_close,
	.bo_create = nouveau_bo_create,
	.bo_create_with_modifiers = nouveau_bo_create_with_modifiers,
	.bo_destroy = drv_gem_bo_destroy,
	.bo_import = drv_prime_bo_import,
	.bo_map = nouveau_bo_map,
	.bo_unmap = drv_bo_munmap,
	.resolve_format_and_use_flags = drv_resolve_format_and_use_flags_helper,
};
#endif /* DRV_NOUVEAU */
