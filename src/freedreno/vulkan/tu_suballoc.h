/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 */

#ifndef TU_SUBALLOC_H
#define TU_SUBALLOC_H

#include "tu_common.h"

#include "tu_knl.h"

/* externally-synchronized BO suballocator. */
#define TU_SUBALLOC_CACHED_BOS 8
struct tu_suballocator
{
   struct tu_device *dev;

   uint32_t default_size;
   enum tu_bo_alloc_flags flags;
   /* When true, new BOs use tu_bo_init_new_cached (CPU-polled data like
    * events/autotune); otherwise WC via tu_bo_init_new. Default false. */
   bool use_cached;

   /** Current BO we're suballocating out of. */
   struct tu_bo *bo;
   uint32_t next_offset;

    /** Small MRU pool of recycled BOs instead of a single slot: with one
    * slot a working set larger than default_size ping-pongs between
    * alloc+free every time. Externally synchronized, no locking. */
   struct tu_bo *cached_bos[TU_SUBALLOC_CACHED_BOS];
   uint32_t cached_count;
   /** Small MRU pool of recycled BOs instead of a single slot: with one
    * slot a working set larger than default_size ping-pongs between
    * alloc+free every time. Total pooled bytes are capped so a transient
    * peak never pins large BOs forever. Externally synchronized, no
    * locking. */
   uint64_t cached_bytes;

   const char *name;
};

struct tu_suballoc_bo
{
   struct tu_bo *bo;
   uint64_t iova;
   uint32_t size; /* bytes */
};

void
tu_bo_suballocator_init(struct tu_suballocator *suballoc,
                        struct tu_device *dev,
                        uint32_t default_size,
                        enum tu_bo_alloc_flags flags,
                        const char *name);
void
tu_bo_suballocator_init_cached(struct tu_suballocator *suballoc,
                               struct tu_device *dev,
                               uint32_t default_size,
                               enum tu_bo_alloc_flags flags,
                               const char *name);
void
tu_bo_suballocator_finish(struct tu_suballocator *suballoc);

VkResult
tu_suballoc_bo_alloc(struct tu_suballoc_bo *suballoc_bo, struct tu_suballocator *suballoc,
                     uint32_t size, uint32_t alignment);

void *
tu_suballoc_bo_map(struct tu_suballoc_bo *bo);

void
tu_suballoc_bo_free(struct tu_suballocator *suballoc, struct tu_suballoc_bo *bo);

#endif /* TU_SUBALLOC_H */
