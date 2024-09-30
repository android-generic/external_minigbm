/*
 * Copyright 2017 Advanced Micro Devices. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifdef DRV_AMDGPU

#include "drv.h"

/**
 * These formats correspond to the similarly named MESA_FORMAT_*
 * tokens, except in the native endian of the CPU.  For example, on
 * little endian __DRI_IMAGE_FORMAT_XRGB8888 corresponds to
 * MESA_FORMAT_XRGB8888, but MESA_FORMAT_XRGB8888_REV on big endian.
 *
 * __DRI_IMAGE_FORMAT_NONE is for images that aren't directly usable
 * by the driver (YUV planar formats) but serve as a base image for
 * creating sub-images for the different planes within the image.
 *
 * R8, GR88 and NONE should not be used with createImageFromName or
 * createImage, and are returned by query from sub images created with
 * createImageFromNames (NONE, see above) and fromPlane (R8 & GR88).
 */
#define __DRI_IMAGE_FORMAT_RGB565       0x1001
#define __DRI_IMAGE_FORMAT_XRGB8888     0x1002
#define __DRI_IMAGE_FORMAT_ARGB8888     0x1003
#define __DRI_IMAGE_FORMAT_ABGR8888     0x1004
#define __DRI_IMAGE_FORMAT_XBGR8888     0x1005
#define __DRI_IMAGE_FORMAT_R8           0x1006 /* Since version 5 */
#define __DRI_IMAGE_FORMAT_GR88         0x1007
#define __DRI_IMAGE_FORMAT_NONE         0x1008
#define __DRI_IMAGE_FORMAT_XRGB2101010  0x1009
#define __DRI_IMAGE_FORMAT_ARGB2101010  0x100a
#define __DRI_IMAGE_FORMAT_SARGB8       0x100b
#define __DRI_IMAGE_FORMAT_ARGB1555     0x100c
#define __DRI_IMAGE_FORMAT_R16          0x100d
#define __DRI_IMAGE_FORMAT_GR1616       0x100e
#define __DRI_IMAGE_FORMAT_YUYV         0x100f
#define __DRI_IMAGE_FORMAT_XBGR2101010  0x1010
#define __DRI_IMAGE_FORMAT_ABGR2101010  0x1011
#define __DRI_IMAGE_FORMAT_SABGR8       0x1012
#define __DRI_IMAGE_FORMAT_UYVY         0x1013
#define __DRI_IMAGE_FORMAT_XBGR16161616F 0x1014
#define __DRI_IMAGE_FORMAT_ABGR16161616F 0x1015
#define __DRI_IMAGE_FORMAT_SXRGB8       0x1016
#define __DRI_IMAGE_FORMAT_ABGR16161616 0x1017
#define __DRI_IMAGE_FORMAT_XBGR16161616 0x1018
#define __DRI_IMAGE_FORMAT_ARGB4444	0x1019
#define __DRI_IMAGE_FORMAT_XRGB4444	0x101a
#define __DRI_IMAGE_FORMAT_ABGR4444	0x101b
#define __DRI_IMAGE_FORMAT_XBGR4444	0x101c
#define __DRI_IMAGE_FORMAT_XRGB1555	0x101d
#define __DRI_IMAGE_FORMAT_ABGR1555	0x101e
#define __DRI_IMAGE_FORMAT_XBGR1555	0x101f

struct dri_driver;

void *dri_dlopen(const char *dri_so_path);
void dri_dlclose(void *dri_so_handle);

struct dri_driver *dri_init(struct driver *drv, const char *dri_so_path, const char *driver_suffix);
void dri_close(struct dri_driver *dri);

int dri_bo_create(struct dri_driver *dri, struct bo *bo, uint32_t width, uint32_t height,
		  uint32_t format, uint64_t use_flags);
int dri_bo_create_with_modifiers(struct dri_driver *dri, struct bo *bo, uint32_t width,
				 uint32_t height, uint32_t format, uint64_t use_flags,
				 const uint64_t *modifiers, uint32_t modifier_count);
int dri_bo_import(struct dri_driver *dri, struct bo *bo, struct drv_import_fd_data *data);
int dri_bo_release(struct dri_driver *dri, struct bo *bo);
int dri_bo_destroy(struct dri_driver *dri, struct bo *bo);
void *dri_bo_map(struct dri_driver *dri, struct bo *bo, struct vma *vma, size_t plane,
		 uint32_t map_flags);
int dri_bo_unmap(struct dri_driver *dri, struct bo *bo, struct vma *vma);

size_t dri_num_planes_from_modifier(struct dri_driver *dri, uint32_t format, uint64_t modifier);
bool dri_query_modifiers(struct dri_driver *dri, uint32_t format, int max, uint64_t *modifiers,
			 int *count);
#endif
