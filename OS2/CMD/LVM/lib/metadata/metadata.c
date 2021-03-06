/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "device.h"
#include "metadata.h"
#include "toolcontext.h"
#include "lvm-string.h"
#include "lvmcache.h"
#include "memlock.h"
#include "str_list.h"
#include "pv_alloc.h"
#include "activate.h"
#include "display.h"
#include "locking.h"

#include <libdevmapper.h>

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#ifndef MAX
# define MAX max
#endif
#endif

//#include <sys/param.h>

/*
 * FIXME: Check for valid handle before dereferencing field or log error?
 */
#define pv_field(handle, field)                         \
        (((struct physical_volume *)(handle))->field)

static struct physical_volume *_pv_read(struct cmd_context *cmd,
                                        const char *pv_name,
                                        struct list *mdas,
                                        uint64_t *label_sector,
                                        int warnings);

static struct physical_volume *_pv_create(const struct format_type *fmt,
                                  struct device *dev,
                                  struct id *id, uint64_t size,
                                  uint64_t pe_start,
                                  uint32_t existing_extent_count,
                                  uint32_t existing_extent_size,
                                  int pvmetadatacopies,
                                  uint64_t pvmetadatasize, struct list *mdas);

static int _pv_write(struct cmd_context *cmd, // __attribute((unused)),
                     struct physical_volume *pv,
                     struct list *mdas, int64_t label_sector);

static struct physical_volume *_find_pv_by_name(struct cmd_context *cmd,
                                                const char *pv_name);

static struct pv_list *_find_pv_in_vg(struct volume_group *vg, const char *pv_name);

static struct physical_volume *_find_pv_in_vg_by_uuid(struct volume_group *vg,
                                                      struct id *id);

unsigned long pe_align(void)
{
        return MAX(65536UL, lvm_getpagesize()) >> SECTOR_SHIFT;
}

/**
 * add_pv_to_vg - Add a physical volume to a volume group
 * @vg - volume group to add to
 * @pv_name - name of the pv (to be removed)
 * @pv - physical volume to add to volume group
 *
 * Returns:
 *  0 - failure
 *  1 - success
 * FIXME: remove pv_name - obtain safely from pv
 */
int add_pv_to_vg(struct volume_group *vg, const char *pv_name,
                 struct physical_volume *pv)
{
        struct pv_list *pvl;
        struct format_instance *fid = vg->fid;
        struct dm_pool *mem = fid->fmt->cmd->mem;

        log_verbose("Adding physical volume '%s' to volume group '%s'",
                    pv_name, vg->name);

        if (!(pvl = dm_pool_zalloc(mem, sizeof(*pvl)))) {
                log_error("pv_list allocation for '%s' failed", pv_name);
                return 0;
        }

        if (*pv->vg_name) {
                log_error("Physical volume '%s' is already in volume group "
                          "'%s'", pv_name, pv->vg_name);
                return 0;
        }

        if (pv->fmt != fid->fmt) {
                log_error("Physical volume %s is of different format type (%s)",
                          pv_name, pv->fmt->name);
                return 0;
        }

        /* Ensure PV doesn't depend on another PV already in the VG */
        if (pv_uses_vg(pv, vg)) {
                log_error("Physical volume %s might be constructed from same "
                          "volume group %s", pv_name, vg->name);
                return 0;
        }

        if (!(pv->vg_name = dm_pool_strdup(mem, vg->name))) {
                log_error("vg->name allocation failed for '%s'", pv_name);
                return 0;
        }

        memcpy(&pv->vgid, &vg->id, sizeof(vg->id));

        /* Units of 512-byte sectors */
        pv->pe_size = vg->extent_size;

        /* FIXME Do proper rounding-up alignment? */
        /* Reserved space for label; this holds 0 for PVs created by LVM1 */
        if (pv->pe_start < pe_align())
                pv->pe_start = pe_align();

        /*
         * pe_count must always be calculated by pv_setup
         */
        pv->pe_alloc_count = 0;

        if (!fid->fmt->ops->pv_setup(fid->fmt, UINT64_C(0), 0,
                                     vg->extent_size, 0, UINT64_C(0),
                                     &fid->metadata_areas, pv, vg)) {
                log_error("Format-specific setup of physical volume '%s' "
                          "failed.", pv_name);
                return 0;
        }

        if (_find_pv_in_vg(vg, pv_name)) {
                log_error("Physical volume '%s' listed more than once.",
                          pv_name);
                return 0;
        }

        if (vg->pv_count && (vg->pv_count == vg->max_pv)) {
                log_error("No space for '%s' - volume group '%s' "
                          "holds max %d physical volume(s).", pv_name,
                          vg->name, vg->max_pv);
                return 0;
        }

        if (!alloc_pv_segment_whole_pv(mem, pv)) {
                stack;
                return 0;
        }

        pvl->pv = pv;
        list_add(&vg->pvs, &pvl->list);

        if ((uint64_t) vg->extent_count + pv->pe_count > UINT32_MAX) {
                log_error("Unable to add %s to %s: new extent count (%"
                          PRIu64 ") exceeds limit (%" PRIu32 ").",
                          pv_name, vg->name,
                          (uint64_t) vg->extent_count + pv->pe_count,
                          UINT32_MAX);
                return 0;
        }

        vg->pv_count++;
        vg->extent_count += pv->pe_count;
        vg->free_count += pv->pe_count;

        return 1;
}

static int _copy_pv(struct physical_volume *pv_to,
                    struct physical_volume *pv_from)
{
        memcpy(pv_to, pv_from, sizeof(*pv_to));

        if (!str_list_dup(pv_to->fmt->cmd->mem, &pv_to->tags, &pv_from->tags)) {
                log_error("PV tags duplication failed");
                return 0;
        }

        if (!peg_dup(pv_to->fmt->cmd->mem, &pv_to->segments,
                     &pv_from->segments)) {
                stack;
                return 0;
        }

        return 1;
}

int get_pv_from_vg_by_id(const struct format_type *fmt, const char *vg_name,
                         const char *vgid, const char *pvid,
                         struct physical_volume *pv)
{
        struct volume_group *vg;
        struct pv_list *pvl;
        int consistent = 0;

        if (!(vg = vg_read(fmt->cmd, vg_name, vgid, &consistent))) {
                log_error("get_pv_from_vg_by_id: vg_read failed to read VG %s",
                          vg_name);
                return 0;
        }

        if (!consistent)
                log_warn("WARNING: Volume group %s is not consistent",
                         vg_name);

        //list_iterate_items(pvl, &vg->pvs) {
        list_iterate_items(pvl, struct pv_list, &vg->pvs) {
                if (id_equal(&pvl->pv->id, (const struct id *) pvid)) {
                        if (!_copy_pv(pv, pvl->pv)) {
                                stack;
                                return 0;
                        }
                        return 1;
                }
        }

        return 0;
}

int vg_rename(struct cmd_context *cmd, struct volume_group *vg,
              const char *new_name)
{
        struct dm_pool *mem = cmd->mem;
        struct pv_list *pvl;

        if (!(vg->name = dm_pool_strdup(mem, new_name))) {
                log_error("vg->name allocation failed for '%s'", new_name);
                return 0;
        }

        //list_iterate_items(pvl, &vg->pvs) {
        list_iterate_items(pvl, struct pv_list, &vg->pvs) {
                if (!(pvl->pv->vg_name = dm_pool_strdup(mem, new_name))) {
                        log_error("pv->vg_name allocation failed for '%s'",
                                  dev_name(pvl->pv->dev));
                        return 0;
                }
        }

        return 1;
}

int vg_extend(struct volume_group *vg, int pv_count, char **pv_names)
{
        int i;
        struct physical_volume *pv;

        /* attach each pv */
        for (i = 0; i < pv_count; i++) {
                if (!(pv = pv_by_path(vg->fid->fmt->cmd, pv_names[i]))) {
                        log_error("%s not identified as an existing "
                                  "physical volume", pv_names[i]);
                        goto bad;
                }

                if (!add_pv_to_vg(vg, pv_names[i], pv))
                        goto bad;
        }

/* FIXME Decide whether to initialise and add new mdahs to format instance */

        return 1;

      bad:
        log_error("Unable to add physical volume '%s' to "
                  "volume group '%s'.", pv_names[i], vg->name);
        return 0;
}

const char *strip_dir(const char *vg_name, const char *dev_dir)
{
        size_t len = strlen(dev_dir);
        if (!strncmp(vg_name, dev_dir, len))
                vg_name += len;

        return vg_name;
}

struct volume_group *vg_create(struct cmd_context *cmd, const char *vg_name,
                               uint32_t extent_size, uint32_t max_pv,
                               uint32_t max_lv, alloc_policy_t alloc,
                               int pv_count, char **pv_names)
{
        struct volume_group *vg;
        struct dm_pool *mem = cmd->mem;
        int consistent = 0;
        int old_partial;

        if (!(vg = dm_pool_zalloc(mem, sizeof(*vg)))) {
                stack;
                return NULL;
        }

        /* is this vg name already in use ? */
        old_partial = partial_mode();
        init_partial(1);
        if (vg_read(cmd, vg_name, NULL, &consistent)) {
                log_err("A volume group called '%s' already exists.", vg_name);
                goto bad;
        }
        init_partial(old_partial);

        if (!id_create(&vg->id)) {
                log_err("Couldn't create uuid for volume group '%s'.", vg_name);
                goto bad;
        }

        /* Strip dev_dir if present */
        vg_name = strip_dir(vg_name, cmd->dev_dir);

        vg->cmd = cmd;

        if (!(vg->name = dm_pool_strdup(mem, vg_name))) {
                stack;
                goto bad;
        }

        vg->seqno = 0;

        vg->status = (RESIZEABLE_VG | LVM_READ | LVM_WRITE);
        if (!(vg->system_id = dm_pool_alloc(mem, NAME_LEN)))
                goto_bad;

        *vg->system_id = '\0';

        vg->extent_size = extent_size;
        vg->extent_count = 0;
        vg->free_count = 0;

        vg->max_lv = max_lv;
        vg->max_pv = max_pv;

        vg->alloc = alloc;

        vg->pv_count = 0;
        list_init(&vg->pvs);

        vg->lv_count = 0;
        list_init(&vg->lvs);

        vg->snapshot_count = 0;

        list_init(&vg->tags);

        if (!(vg->fid = cmd->fmt->ops->create_instance(cmd->fmt, vg_name,
                                                       NULL, NULL))) {
                log_error("Failed to create format instance");
                goto bad;
        }

        if (vg->fid->fmt->ops->vg_setup &&
            !vg->fid->fmt->ops->vg_setup(vg->fid, vg)) {
                log_error("Format specific setup of volume group '%s' failed.",
                          vg_name);
                goto bad;
        }

        /* attach the pv's */
        if (!vg_extend(vg, pv_count, pv_names))
                goto_bad;

        return vg;

      bad:
        dm_pool_free(mem, vg);
        return NULL;
}

static int _recalc_extents(uint32_t *extents, const char *desc1,
                           const char *desc2, uint32_t old_size,
                           uint32_t new_size)
{
        uint64_t size = (uint64_t) old_size * (*extents);

        if (size % new_size) {
                log_error("New size %" PRIu64 " for %s%s not an exact number "
                          "of new extents.", size, desc1, desc2);
                return 0;
        }

        size /= new_size;

        if (size > UINT32_MAX) {
                log_error("New extent count %" PRIu64 " for %s%s exceeds "
                          "32 bits.", size, desc1, desc2);
                return 0;
        }

        *extents = (uint32_t) size;

        return 1;
}

int vg_change_pesize(struct cmd_context *cmd, struct volume_group *vg,
                     uint32_t new_size)
{
        uint32_t old_size = vg->extent_size;
        struct pv_list *pvl;
        struct lv_list *lvl;
        struct physical_volume *pv;
        struct logical_volume *lv;
        struct lv_segment *seg;
        struct pv_segment *pvseg;
        uint32_t s;

        vg->extent_size = new_size;

        if (vg->fid->fmt->ops->vg_setup &&
            !vg->fid->fmt->ops->vg_setup(vg->fid, vg)) {
                stack;
                return 0;
        }

        if (!_recalc_extents(&vg->extent_count, vg->name, "", old_size,
                             new_size)) {
                stack;
                return 0;
        }

        if (!_recalc_extents(&vg->free_count, vg->name, " free space",
                             old_size, new_size)) {
                stack;
                return 0;
        }

        /* foreach PV */
        //list_iterate_items(pvl, &vg->pvs) {
        list_iterate_items(pvl, struct pv_list, &vg->pvs) {
                pv = pvl->pv;

                pv->pe_size = new_size;
                if (!_recalc_extents(&pv->pe_count, dev_name(pv->dev), "",
                                     old_size, new_size)) {
                        stack;
                        return 0;
                }

                if (!_recalc_extents(&pv->pe_alloc_count, dev_name(pv->dev),
                                     " allocated space", old_size, new_size)) {
                        stack;
                        return 0;
                }

                /* foreach free PV Segment */
                //list_iterate_items(pvseg, &pv->segments) {
                list_iterate_items(pvseg, struct pv_segment, &pv->segments) {
                        if (pvseg->lvseg)
                                continue;

                        if (!_recalc_extents(&pvseg->pe, dev_name(pv->dev),
                                             " PV segment start", old_size,
                                             new_size)) {
                                stack;
                                return 0;
                        }
                        if (!_recalc_extents(&pvseg->len, dev_name(pv->dev),
                                             " PV segment length", old_size,
                                             new_size)) {
                                stack;
                                return 0;
                        }
                }
        }

        /* foreach LV */
        //list_iterate_items(lvl, &vg->lvs) {
        list_iterate_items(lvl, struct lv_list, &vg->lvs) {
                lv = lvl->lv;

                if (!_recalc_extents(&lv->le_count, lv->name, "", old_size,
                                     new_size)) {
                        stack;
                        return 0;
                }

                //list_iterate_items(seg, &lv->segments) {
                list_iterate_items(seg, struct lv_segment, &lv->segments) {
                        if (!_recalc_extents(&seg->le, lv->name,
                                             " segment start", old_size,
                                             new_size)) {
                                stack;
                                return 0;
                        }

                        if (!_recalc_extents(&seg->len, lv->name,
                                             " segment length", old_size,
                                             new_size)) {
                                stack;
                                return 0;
                        }

                        if (!_recalc_extents(&seg->area_len, lv->name,
                                             " area length", old_size,
                                             new_size)) {
                                stack;
                                return 0;
                        }

                        if (!_recalc_extents(&seg->extents_copied, lv->name,
                                             " extents moved", old_size,
                                             new_size)) {
                                stack;
                                return 0;
                        }

                        /* foreach area */
                        for (s = 0; s < seg->area_count; s++) {
                                switch (seg_type(seg, s)) {
                                case AREA_PV:
                                        if (!_recalc_extents
                                            (&seg_pe(seg, s),
                                             lv->name,
                                             " pvseg start", old_size,
                                             new_size)) {
                                                stack;
                                                return 0;
                                        }
                                        if (!_recalc_extents
                                            (&seg_pvseg(seg, s)->len,
                                             lv->name,
                                             " pvseg length", old_size,
                                             new_size)) {
                                                stack;
                                                return 0;
                                        }
                                        break;
                                case AREA_LV:
                                        if (!_recalc_extents
                                            (&seg_le(seg, s), lv->name,
                                             " area start", old_size,
                                             new_size)) {
                                                stack;
                                                return 0;
                                        }
                                        break;
                                case AREA_UNASSIGNED:
                                        log_error("Unassigned area %u found in "
                                                  "segment", s);
                                        return 0;
                                }
                        }
                }

        }

        return 1;
}

int vg_split_mdas(struct cmd_context *cmd, struct volume_group *vg_from,
                  struct volume_group *vg_to)
{
        struct metadata_area *mda, *mda2;
        struct list *mdas_from, *mdas_to;
        int common_mda = 0;

        mdas_from = &vg_from->fid->metadata_areas;
        mdas_to = &vg_to->fid->metadata_areas;

        //list_iterate_items_safe(mda, mda2, mdas_from) {
        list_iterate_items_safe(mda, mda2, struct metadata_area, mdas_from) {
                if (!mda->ops->mda_in_vg) {
                        common_mda = 1;
                        continue;
                }

                if (!mda->ops->mda_in_vg(vg_from->fid, vg_from, mda)) {
                        list_del(&mda->list);
                        list_add(mdas_to, &mda->list);
                }
        }

        if (list_empty(mdas_from) || list_empty(mdas_to))
                return common_mda;

        return 1;
}

/**
 * pv_create - initialize a physical volume for use with a volume group
 * @fmt: format type
 * @dev: PV device to initialize
 * @id: PV UUID to use for initialization
 * @size: size of the PV in sectors
 * @pe_start: physical extent start
 * @existing_extent_count
 * @existing_extent_size
 * @pvmetadatacopies
 * @pvmetadatasize
 * @mdas
 *
 * Returns:
 *   PV handle - physical volume initialized successfully
 *   NULL - invalid parameter or problem initializing the physical volume
 *
 * Note:
 *   FIXME - liblvm todo - tidy up arguments for external use (fmt, mdas, etc)
 */
pv_t *pv_create(const struct format_type *fmt,
                struct device *dev,
                struct id *id, uint64_t size,
                uint64_t pe_start,
                uint32_t existing_extent_count,
                uint32_t existing_extent_size,
                int pvmetadatacopies,
                uint64_t pvmetadatasize, struct list *mdas)
{
        return _pv_create(fmt, dev, id, size, pe_start,
                          existing_extent_count,
                          existing_extent_size,
                          pvmetadatacopies,
                          pvmetadatasize, mdas);
}

/* Sizes in sectors */
static struct physical_volume *_pv_create(const struct format_type *fmt,
                                  struct device *dev,
                                  struct id *id, uint64_t size,
                                  uint64_t pe_start,
                                  uint32_t existing_extent_count,
                                  uint32_t existing_extent_size,
                                  int pvmetadatacopies,
                                  uint64_t pvmetadatasize, struct list *mdas)
{
        struct dm_pool *mem = fmt->cmd->mem;
        struct physical_volume *pv = dm_pool_zalloc(mem, sizeof(*pv));

        if (!pv) {
                stack;
                return NULL;
        }

        if (id)
                memcpy(&pv->id, id, sizeof(*id));
        else if (!id_create(&pv->id)) {
                log_error("Failed to create random uuid for %s.",
                          dev_name(dev));
                return NULL;
        }

        pv->dev = dev;

        if (!(pv->vg_name = dm_pool_zalloc(mem, NAME_LEN)))
                goto_bad;

        pv->status = ALLOCATABLE_PV;

        if (!dev_get_size(pv->dev, &pv->size)) {
                log_error("%s: Couldn't get size.", dev_name(pv->dev));
                goto bad;
        }

        if (size) {
                if (size > pv->size)
                        log_warn("WARNING: %s: Overriding real size. "
                                  "You could lose data.", dev_name(pv->dev));
                log_verbose("%s: Pretending size is %" PRIu64 " sectors.",
                            dev_name(pv->dev), size);
                pv->size = size;
        }

        if (pv->size < PV_MIN_SIZE) {
                log_error("%s: Size must exceed minimum of %ld sectors.",
                          dev_name(pv->dev), PV_MIN_SIZE);
                goto bad;
        }

        pv->pe_size = 0;
        pv->pe_start = 0;
        pv->pe_count = 0;
        pv->pe_alloc_count = 0;
        pv->fmt = fmt;

        list_init(&pv->tags);
        list_init(&pv->segments);

        if (!fmt->ops->pv_setup(fmt, pe_start, existing_extent_count,
                                existing_extent_size,
                                pvmetadatacopies, pvmetadatasize, mdas,
                                pv, NULL)) {
                log_error("%s: Format-specific setup of physical volume "
                          "failed.", dev_name(pv->dev));
                goto bad;
        }
        return pv;

      bad:
        dm_pool_free(mem, pv);
        return NULL;
}

/* FIXME: liblvm todo - make into function that returns handle */
struct pv_list *find_pv_in_vg(struct volume_group *vg, const char *pv_name)
{
        return _find_pv_in_vg(vg, pv_name);
}

static struct pv_list *_find_pv_in_vg(struct volume_group *vg, const char *pv_name)
{
        struct pv_list *pvl;

        //list_iterate_items(pvl, &vg->pvs)
        list_iterate_items(pvl, struct pv_list, &vg->pvs)
                if (pvl->pv->dev == dev_cache_get(pv_name, vg->cmd->filter))
                        return pvl;

        return NULL;
}

int pv_is_in_vg(struct volume_group *vg, struct physical_volume *pv)
{
        struct pv_list *pvl;

        //list_iterate_items(pvl, &vg->pvs)
        list_iterate_items(pvl, struct pv_list, &vg->pvs)
                if (pv == pvl->pv)
                         return 1;

        return 0;
}

/**
 * find_pv_in_vg_by_uuid - Find PV in VG by PV UUID
 * @vg: volume group to search
 * @id: UUID of the PV to match
 *
 * Returns:
 *   PV handle - if UUID of PV found in VG
 *   NULL - invalid parameter or UUID of PV not found in VG
 *
 * Note
 *   FIXME - liblvm todo - make into function that takes VG handle
 */
pv_t *find_pv_in_vg_by_uuid(struct volume_group *vg, struct id *id)
{
        return _find_pv_in_vg_by_uuid(vg, id);
}


static struct physical_volume *_find_pv_in_vg_by_uuid(struct volume_group *vg,
                                                      struct id *id)
{
        struct pv_list *pvl;

        //list_iterate_items(pvl, &vg->pvs)
        list_iterate_items(pvl, struct pv_list, &vg->pvs)
                if (id_equal(&pvl->pv->id, id))
                        return pvl->pv;

        return NULL;
}

struct lv_list *find_lv_in_vg(struct volume_group *vg, const char *lv_name)
{
        struct lv_list *lvl;
        const char *ptr;

        /* Use last component */
        if ((ptr = strrchr(lv_name, '/')))
                ptr++;
        else
                ptr = lv_name;

        //list_iterate_items(lvl, &vg->lvs)
                if (!strcmp(lvl->lv->name, ptr))
                        return lvl;

        return NULL;
}

struct lv_list *find_lv_in_vg_by_lvid(struct volume_group *vg,
                                      const union lvid *lvid)
{
        struct lv_list *lvl;

        //list_iterate_items(lvl, &vg->lvs)
        list_iterate_items(lvl, struct lv_list, &vg->lvs)
                if (!strncmp(lvl->lv->lvid.s, lvid->s, sizeof(*lvid)))
                        return lvl;

        return NULL;
}

struct logical_volume *find_lv(struct volume_group *vg, const char *lv_name)
{
        struct lv_list *lvl = find_lv_in_vg(vg, lv_name);
        return lvl ? lvl->lv : NULL;
}

struct physical_volume *find_pv(struct volume_group *vg, struct device *dev)
{
        struct pv_list *pvl;

        //list_iterate_items(pvl, &vg->pvs)
        list_iterate_items(pvl, struct pv_list, &vg->pvs)
                if (dev == pvl->pv->dev)
                        return pvl->pv;

        return NULL;
}

/* FIXME: liblvm todo - make into function that returns handle */
struct physical_volume *find_pv_by_name(struct cmd_context *cmd,
                                        const char *pv_name)
{
        return _find_pv_by_name(cmd, pv_name);
}


static struct physical_volume *_find_pv_by_name(struct cmd_context *cmd,
                                                const char *pv_name)
{
        struct physical_volume *pv;

        if (!(pv = _pv_read(cmd, pv_name, NULL, NULL, 1))) {
                log_error("Physical volume %s not found", pv_name);
                return NULL;
        }

        if (!pv->vg_name[0]) {
                log_error("Physical volume %s not in a volume group", pv_name);
                return NULL;
        }

        return pv;
}

/* Find segment at a given logical extent in an LV */
struct lv_segment *find_seg_by_le(struct logical_volume *lv, uint32_t le)
{
        struct lv_segment *seg;

        //list_iterate_items(seg, &lv->segments)
        list_iterate_items(seg, struct lv_segment, &lv->segments)
                if (le >= seg->le && le < seg->le + seg->len)
                        return seg;

        return NULL;
}

struct lv_segment *first_seg(struct logical_volume *lv)
{
        struct lv_segment *seg = NULL;

        //list_iterate_items(seg, &lv->segments)
        list_iterate_items(seg, struct lv_segment, &lv->segments)
                break;

        return seg;
}

/* Find segment at a given physical extent in a PV */
struct pv_segment *find_peg_by_pe(struct physical_volume *pv, uint32_t pe)
{
        struct pv_segment *peg;

        //list_iterate_items(peg, &pv->segments)
        list_iterate_items(peg, struct pv_segment, &pv->segments)
                if (pe >= peg->pe && pe < peg->pe + peg->len)
                        return peg;

        return NULL;
}

int vg_remove(struct volume_group *vg)
{
        struct metadata_area *mda;

        /* FIXME Improve recovery situation? */
        /* Remove each copy of the metadata */
        //list_iterate_items(mda, &vg->fid->metadata_areas) {
        list_iterate_items(mda, struct metadata_area, &vg->fid->metadata_areas) {
                if (mda->ops->vg_remove &&
                    !mda->ops->vg_remove(vg->fid, vg, mda)) {
                        stack;
                        return 0;
                }
        }

        return 1;
}

int vg_validate(struct volume_group *vg)
{
        char uuid[64];
        struct pv_list *pvl, *pvl2;
        struct lv_list *lvl, *lvl2;
        // is 8-bits-aligned because previous 4 pointers take 16 bytes == 2 * 8
        //char uuid[64] __attribute((aligned(8)));
        int r = 1;

        /* FIXME Also check there's no data/metadata overlap */

        //list_iterate_items(pvl, &vg->pvs) {
        list_iterate_items(pvl, struct pv_list, &vg->pvs) {
                //list_iterate_items(pvl2, &vg->pvs) {
                list_iterate_items(pvl2, struct pv_list, &vg->pvs) {
                        if (pvl == pvl2)
                                break;
                        if (id_equal(&pvl->pv->id,
                                     &pvl2->pv->id)) {
                                if (!id_write_format(&pvl->pv->id, uuid,
                                                     sizeof(uuid)))
                                         stack;
                                log_error("Internal error: Duplicate PV id "
                                          "%s detected for %s in %s.",
                                          uuid, dev_name(pvl->pv->dev),
                                          vg->name);
                                r = 0;
                        }
                }

                if (strcmp(pvl->pv->vg_name, vg->name)) {
                        log_error("Internal error: VG name for PV %s is corrupted",
                                  dev_name(pvl->pv->dev));
                        r = 0;
                }
        }

        if (!check_pv_segments(vg)) {
                log_error("Internal error: PV segments corrupted in %s.",
                          vg->name);
                r = 0;
        }

        //list_iterate_items(lvl, &vg->lvs) {
        list_iterate_items(lvl, struct lv_list, &vg->lvs) {
                //list_iterate_items(lvl2, &vg->lvs) {
                list_iterate_items(lvl2, struct lv_list, &vg->lvs) {
                        if (lvl == lvl2)
                                break;
                        if (!strcmp(lvl->lv->name, lvl2->lv->name)) {
                                log_error("Internal error: Duplicate LV name "
                                          "%s detected in %s.", lvl->lv->name,
                                          vg->name);
                                r = 0;
                        }
                        if (id_equal(&lvl->lv->lvid.id[1],
                                     &lvl2->lv->lvid.id[1])) {
                                if (!id_write_format(&lvl->lv->lvid.id[1], uuid,
                                                     sizeof(uuid)))
                                         stack;
                                log_error("Internal error: Duplicate LV id "
                                          "%s detected for %s and %s in %s.",
                                          uuid, lvl->lv->name, lvl2->lv->name,
                                          vg->name);
                                r = 0;
                        }
                }
        }

        //list_iterate_items(lvl, &vg->lvs) {
        list_iterate_items(lvl, struct lv_list, &vg->lvs) {
                if (!check_lv_segments(lvl->lv, 1)) {
                        log_error("Internal error: LV segments corrupted in %s.",
                                  lvl->lv->name);
                        r = 0;
                }
        }

        return r;
}

/*
 * After vg_write() returns success,
 * caller MUST call either vg_commit() or vg_revert()
 */
int vg_write(struct volume_group *vg)
{
        struct list *mdah;
        struct metadata_area *mda;

        if (!vg_validate(vg)) {
                stack;
                return 0;
        }

        if (vg->status & PARTIAL_VG) {
                log_error("Cannot change metadata for partial volume group %s",
                          vg->name);
                return 0;
        }

        if (list_empty(&vg->fid->metadata_areas)) {
                log_error("Aborting vg_write: No metadata areas to write to!");
                return 0;
        }

        vg->seqno++;

        /* Write to each copy of the metadata area */
        //list_iterate_items(mda, &vg->fid->metadata_areas) {
        list_iterate_items(mda, struct metadata_area, &vg->fid->metadata_areas) {
                if (!mda->ops->vg_write) {
                        log_error("Format does not support writing volume"
                                  "group metadata areas");
                        /* Revert */
                        list_uniterate(mdah, &vg->fid->metadata_areas, &mda->list) {
                                mda = list_item(mdah, struct metadata_area);

                                if (mda->ops->vg_revert &&
                                    !mda->ops->vg_revert(vg->fid, vg, mda)) {
                                        stack;
                                }
                        }
                        return 0;
                }
                if (!mda->ops->vg_write(vg->fid, vg, mda)) {
                        stack;
                        /* Revert */
                        list_uniterate(mdah, &vg->fid->metadata_areas, &mda->list) {
                                mda = list_item(mdah, struct metadata_area);

                                if (mda->ops->vg_revert &&
                                    !mda->ops->vg_revert(vg->fid, vg, mda)) {
                                        stack;
                                }
                        }
                        return 0;
                }
        }

        /* Now pre-commit each copy of the new metadata */
        //list_iterate_items(mda, &vg->fid->metadata_areas) {
        list_iterate_items(mda, struct metadata_area, &vg->fid->metadata_areas) {
                if (mda->ops->vg_precommit &&
                    !mda->ops->vg_precommit(vg->fid, vg, mda)) {
                        stack;
                        /* Revert */
                        //list_iterate_items(mda, &vg->fid->metadata_areas) {
                        list_iterate_items(mda, struct metadata_area, &vg->fid->metadata_areas) {
                                if (mda->ops->vg_revert &&
                                    !mda->ops->vg_revert(vg->fid, vg, mda)) {
                                        stack;
                                }
                        }
                        return 0;
                }
        }

        return 1;
}

/* Commit pending changes */
int vg_commit(struct volume_group *vg)
{
        struct metadata_area *mda;
        int cache_updated = 0;
        int failed = 0;

        /* Commit to each copy of the metadata area */
        //list_iterate_items(mda, &vg->fid->metadata_areas) {
        list_iterate_items(mda, struct metadata_area, &vg->fid->metadata_areas) {
                failed = 0;
                if (mda->ops->vg_commit &&
                    !mda->ops->vg_commit(vg->fid, vg, mda)) {
                        stack;
                        failed = 1;
                }
                /* Update cache first time we succeed */
                if (!failed && !cache_updated) {
                        lvmcache_update_vg(vg);
                        cache_updated = 1;
                }
        }

        /* If at least one mda commit succeeded, it was committed */
        return cache_updated;
}

/* Don't commit any pending changes */
int vg_revert(struct volume_group *vg)
{
        struct metadata_area *mda;

        //list_iterate_items(mda, &vg->fid->metadata_areas) {
        list_iterate_items(mda, struct metadata_area, &vg->fid->metadata_areas) {
                if (mda->ops->vg_revert &&
                    !mda->ops->vg_revert(vg->fid, vg, mda)) {
                        stack;
                }
        }

        return 1;
}

/* Make orphan PVs look like a VG */
static struct volume_group *_vg_read_orphans(struct cmd_context *cmd)
{
        struct lvmcache_vginfo *vginfo;
        struct lvmcache_info *info;
        struct pv_list *pvl;
        struct volume_group *vg;
        struct physical_volume *pv;

        if (!(vginfo = vginfo_from_vgname(ORPHAN, NULL))) {
                stack;
                return NULL;
        }

        if (!(vg = dm_pool_zalloc(cmd->mem, sizeof(*vg)))) {
                log_error("vg allocation failed");
                return NULL;
        }
        list_init(&vg->pvs);
        list_init(&vg->lvs);
        list_init(&vg->tags);
        vg->cmd = cmd;
        if (!(vg->name = dm_pool_strdup(cmd->mem, ORPHAN))) {
                log_error("vg name allocation failed");
                return NULL;
        }

        //list_iterate_items(info, &vginfo->infos) {
        list_iterate_items(info, struct lvmcache_info, &vginfo->infos) {
                if (!(pv = _pv_read(cmd, dev_name(info->dev), NULL, NULL, 1))) {
                        continue;
                }
                if (!(pvl = dm_pool_zalloc(cmd->mem, sizeof(*pvl)))) {
                        log_error("pv_list allocation failed");
                        return NULL;
                }
                pvl->pv = pv;
                list_add(&vg->pvs, &pvl->list);
                vg->pv_count++;
        }

        return vg;
}

static int _update_pv_list(struct list *all_pvs, struct volume_group *vg)
{
        struct pv_list *pvl, *pvl2;

        //list_iterate_items(pvl, &vg->pvs) {
        list_iterate_items(pvl, struct pv_list, &vg->pvs) {
                //list_iterate_items(pvl2, all_pvs) {
                list_iterate_items(pvl2, struct pv_list, all_pvs) {
                        if (pvl->pv->dev == pvl2->pv->dev)
                                goto next_pv;
                }
                /* PV is not on list so add it.  Note that we don't copy it. */
                if (!(pvl2 = dm_pool_zalloc(vg->cmd->mem, sizeof(*pvl2)))) {
                        log_error("pv_list allocation for '%s' failed",
                                  dev_name(pvl->pv->dev));
                        return 0;
                }
                pvl2->pv = pvl->pv;
                list_add(all_pvs, &pvl2->list);
  next_pv:
                ;
        }

        return 1;
}

/* Caller sets consistent to 1 if it's safe for vg_read to correct
 * inconsistent metadata on disk (i.e. the VG write lock is held).
 * This guarantees only consistent metadata is returned unless PARTIAL_VG.
 * If consistent is 0, caller must check whether consistent == 1 on return
 * and take appropriate action if it isn't (e.g. abort; get write lock
 * and call vg_read again).
 *
 * If precommitted is set, use precommitted metadata if present.
 */
static struct volume_group *_vg_read(struct cmd_context *cmd,
                                     const char *vgname,
                                     const char *vgid,
                                     int *consistent, int precommitted)
{
        char uuid[64]; //
        struct format_instance *fid;
        const struct format_type *fmt;
        struct volume_group *vg, *correct_vg = NULL;
        struct metadata_area *mda;
        int inconsistent = 0;
        int inconsistent_vgid = 0;
        int use_precommitted = precommitted;
        struct list *pvids;
        struct pv_list *pvl, *pvl2;
        struct list all_pvs;
        //char uuid[64] __attribute((aligned(8)));

        if (!*vgname) {
                if (use_precommitted) {
                        log_error("Internal error: vg_read requires vgname "
                                  "with pre-commit.");
                        return NULL;
                }
                *consistent = 1;
                return _vg_read_orphans(cmd);
        }

        /* Find the vgname in the cache */
        /* If it's not there we must do full scan to be completely sure */
        if (!(fmt = fmt_from_vgname(vgname, vgid))) {
                lvmcache_label_scan(cmd, 0);
                if (!(fmt = fmt_from_vgname(vgname, vgid))) {
                        if (memlock()) {
                                stack;
                                return NULL;
                        }
                        lvmcache_label_scan(cmd, 2);
                        if (!(fmt = fmt_from_vgname(vgname, vgid))) {
                                stack;
                                return NULL;
                        }
                }
        }

        if (use_precommitted && !(fmt->features & FMT_PRECOMMIT))
                use_precommitted = 0;

        /* Store pvids for later so we can check if any are missing */
        if (!(pvids = lvmcache_get_pvids(cmd, vgname, vgid))) {
                stack;
                return NULL;
        }

        /* create format instance with appropriate metadata area */
        if (!(fid = fmt->ops->create_instance(fmt, vgname, vgid, NULL))) {
                log_error("Failed to create format instance");
                return NULL;
        }

        /* Ensure contents of all metadata areas match - else do recovery */
        //list_iterate_items(mda, &fid->metadata_areas) {
        list_iterate_items(mda, struct metadata_area, &fid->metadata_areas) {
                if ((use_precommitted &&
                     !(vg = mda->ops->vg_read_precommit(fid, vgname, mda))) ||
                    (!use_precommitted &&
                     !(vg = mda->ops->vg_read(fid, vgname, mda)))) {
                        inconsistent = 1;
                        continue;
                }
                if (!correct_vg) {
                        correct_vg = vg;
                        continue;
                }
                /* FIXME Also ensure contents same - checksum compare? */
                if (correct_vg->seqno != vg->seqno) {
                        inconsistent = 1;
                        if (vg->seqno > correct_vg->seqno)
                                correct_vg = vg;
                }
        }

        /* Ensure every PV in the VG was in the cache */
        if (correct_vg) {
                if (list_size(&correct_vg->pvs) != list_size(pvids)) {
                        log_debug("Cached VG %s had incorrect PV list",
                                  vgname);

                        if (memlock())
                                inconsistent = 1;
                        else
                                correct_vg = NULL;
                //} else list_iterate_items(pvl, &correct_vg->pvs) {
                } else list_iterate_items(pvl, struct pv_list, &correct_vg->pvs) {
                        if (!str_list_match_item(pvids, pvl->pv->dev->pvid)) {
                                log_debug("Cached VG %s had incorrect PV list",
                                          vgname);
                                correct_vg = NULL;
                                break;
                        }
                }
        }

        list_init(&all_pvs);

        /* Failed to find VG where we expected it - full scan and retry */
        if (!correct_vg) {
                inconsistent = 0;

                if (memlock()) {
                        stack;
                        return NULL;
                }
                lvmcache_label_scan(cmd, 2);
                if (!(fmt = fmt_from_vgname(vgname, vgid))) {
                        stack;
                        return NULL;
                }

                if (precommitted && !(fmt->features & FMT_PRECOMMIT))
                        use_precommitted = 0;

                /* create format instance with appropriate metadata area */
                if (!(fid = fmt->ops->create_instance(fmt, vgname, vgid, NULL))) {
                        log_error("Failed to create format instance");
                        return NULL;
                }

                /* Ensure contents of all metadata areas match - else recover */
                //list_iterate_items(mda, &fid->metadata_areas) {
                list_iterate_items(mda, struct metadata_area, &fid->metadata_areas) {
                        if ((use_precommitted &&
                             !(vg = mda->ops->vg_read_precommit(fid, vgname,
                                                                mda))) ||
                            (!use_precommitted &&
                             !(vg = mda->ops->vg_read(fid, vgname, mda)))) {
                                inconsistent = 1;
                                continue;
                        }
                        if (!correct_vg) {
                                correct_vg = vg;
                                if (!_update_pv_list(&all_pvs, correct_vg))
                                        return_NULL;
                                continue;
                        }

                        if (strncmp((char *)vg->id.uuid,
                            (char *)correct_vg->id.uuid, ID_LEN)) {
                                inconsistent = 1;
                                inconsistent_vgid = 1;
                        }

                        /* FIXME Also ensure contents same - checksums same? */
                        if (correct_vg->seqno != vg->seqno) {
                                inconsistent = 1;
                                if (vg->seqno > correct_vg->seqno) {
                                        if (!_update_pv_list(&all_pvs, vg))
                                                return_NULL;
                                        correct_vg = vg;
                                }
                        }
                }

                /* Give up looking */
                if (!correct_vg) {
                        stack;
                        return NULL;
                }
        }

        lvmcache_update_vg(correct_vg);

        if (inconsistent) {
                /* FIXME Test should be if we're *using* precommitted metadata not if we were searching for it */
                if (use_precommitted) {
                        log_error("Inconsistent pre-commit metadata copies "
                                  "for volume group %s", vgname);
                        return NULL;
                }

                if (!*consistent)
                        return correct_vg;

                /* Don't touch partial volume group metadata */
                /* Should be fixed manually with vgcfgbackup/restore etc. */
                if ((correct_vg->status & PARTIAL_VG)) {
                        log_error("Inconsistent metadata copies found for "
                                  "partial volume group %s", vgname);
                        *consistent = 0;
                        return correct_vg;
                }

                /* Don't touch if vgids didn't match */
                if (inconsistent_vgid) {
                        log_error("Inconsistent metadata UUIDs found for "
                                  "volume group %s", vgname);
                        *consistent = 0;
                        return correct_vg;
                }

                log_warn("WARNING: Inconsistent metadata found for VG %s - updating "
                         "to use version %u", vgname, correct_vg->seqno);

                if (!vg_write(correct_vg)) {
                        log_error("Automatic metadata correction failed");
                        return NULL;
                }

                if (!vg_commit(correct_vg)) {
                        log_error("Automatic metadata correction commit "
                                  "failed");
                        return NULL;
                }

                //list_iterate_items(pvl, &all_pvs) {
                list_iterate_items(pvl, struct pv_list, &all_pvs) {
                        //list_iterate_items(pvl2, &correct_vg->pvs) {
                        list_iterate_items(pvl2, struct pv_list, &correct_vg->pvs) {
                                if (pvl->pv->dev == pvl2->pv->dev)
                                        goto next_pv;
                        }
                        if (!id_write_format(&pvl->pv->id, uuid, sizeof(uuid)))
                                return_NULL;
                        log_error("Removing PV %s (%s) that no longer belongs to VG %s",
                                  dev_name(pvl->pv->dev), uuid, correct_vg->name);
                        if (!pv_write_orphan(cmd, pvl->pv))
                                return_NULL;
      next_pv:
                        ;
                }
        }

        if ((correct_vg->status & PVMOVE) && !pvmove_mode()) {
                log_error("WARNING: Interrupted pvmove detected in "
                          "volume group %s", correct_vg->name);
                log_error("Please restore the metadata by running "
                          "vgcfgrestore.");
                return NULL;
        }

        *consistent = 1;
        return correct_vg;
}

struct volume_group *vg_read(struct cmd_context *cmd, const char *vgname,
                             const char *vgid, int *consistent)
{
        struct volume_group *vg;
        struct lv_list *lvl;

        if (!(vg = _vg_read(cmd, vgname, vgid, consistent, 0)))
                return NULL;

        if (!check_pv_segments(vg)) {
                log_error("Internal error: PV segments corrupted in %s.",
                          vg->name);
                return NULL;
        }

        //list_iterate_items(lvl, &vg->lvs) {
        list_iterate_items(lvl, struct lv_list, &vg->lvs) {
                if (!check_lv_segments(lvl->lv, 1)) {
                        log_error("Internal error: LV segments corrupted in %s.",
                                  lvl->lv->name);
                        return NULL;
                }
        }

        return vg;
}

/* This is only called by lv_from_lvid, which is only called from
 * activate.c so we know the appropriate VG lock is already held and
 * the vg_read is therefore safe.
 */
static struct volume_group *_vg_read_by_vgid(struct cmd_context *cmd,
                                            const char *vgid,
                                            int precommitted)
{
        const char *vgname;
        struct list *vgnames;
        struct volume_group *vg;
        struct lvmcache_vginfo *vginfo;
        struct str_list *strl;
        int consistent = 0;

        /* Is corresponding vgname already cached? */
        if ((vginfo = vginfo_from_vgid(vgid)) &&
            vginfo->vgname && *vginfo->vgname) {
                if ((vg = _vg_read(cmd, vginfo->vgname, vgid,
                                   &consistent, precommitted)) &&
                    !strncmp((char *)vg->id.uuid, vgid, ID_LEN)) {
                        if (!consistent) {
                                log_error("Volume group %s metadata is "
                                          "inconsistent", vginfo->vgname);
                                if (!partial_mode())
                                        return NULL;
                        }
                        return vg;
                }
        }

        /* Mustn't scan if memory locked: ensure cache gets pre-populated! */
        if (memlock())
                return NULL;

        /* FIXME Need a genuine read by ID here - don't vg_read by name! */
        /* FIXME Disabled vgrenames while active for now because we aren't
         *       allowed to do a full scan here any more. */

        // The slow way - full scan required to cope with vgrename
        if (!(vgnames = get_vgs(cmd, 2))) {
                log_error("vg_read_by_vgid: get_vgs failed");
                return NULL;
        }

        //list_iterate_items(strl, vgnames) {
        list_iterate_items(strl, struct str_list, vgnames) {
                vgname = strl->str;
                if (!vgname || !*vgname)
                        continue;       // FIXME Unnecessary?
                consistent = 0;
                if ((vg = _vg_read(cmd, vgname, vgid, &consistent,
                                   precommitted)) &&
                    !strncmp((char *)vg->id.uuid, vgid, ID_LEN)) {
                        if (!consistent) {
                                log_error("Volume group %s metadata is "
                                          "inconsistent", vgname);
                                return NULL;
                        }
                        return vg;
                }
        }

        return NULL;
}

/* Only called by activate.c */
struct logical_volume *lv_from_lvid(struct cmd_context *cmd, const char *lvid_s,
                                    int precommitted)
{
        struct lv_list *lvl;
        struct volume_group *vg;
        const union lvid *lvid;

        lvid = (const union lvid *) lvid_s;

        log_very_verbose("Finding volume group for uuid %s", lvid_s);
        if (!(vg = _vg_read_by_vgid(cmd, (char *)lvid->id[0].uuid, precommitted))) {
                log_error("Volume group for uuid not found: %s", lvid_s);
                return NULL;
        }

        log_verbose("Found volume group \"%s\"", vg->name);
        if (vg->status & EXPORTED_VG) {
                log_error("Volume group \"%s\" is exported", vg->name);
                return NULL;
        }
        if (!(lvl = find_lv_in_vg_by_lvid(vg, lvid))) {
                log_very_verbose("Can't find logical volume id %s", lvid_s);
                return NULL;
        }

        return lvl->lv;
}

/**
 * pv_read - read and return a handle to a physical volume
 * @cmd: LVM command initiating the pv_read
 * @pv_name: full device name of the PV, including the path
 * @mdas: list of metadata areas of the PV
 * @label_sector: sector number where the PV label is stored on @pv_name
 * @warnings:
 *
 * Returns:
 *   PV handle - valid pv_name and successful read of the PV, or
 *   NULL - invalid parameter or error in reading the PV
 *
 * Note:
 *   FIXME - liblvm todo - make into function that returns handle
 */
struct physical_volume *pv_read(struct cmd_context *cmd, const char *pv_name,
                                struct list *mdas, uint64_t *label_sector,
                                int warnings)
{
        return _pv_read(cmd, pv_name, mdas, label_sector, warnings);
}

/* FIXME Use label functions instead of PV functions */
static struct physical_volume *_pv_read(struct cmd_context *cmd,
                                        const char *pv_name,
                                        struct list *mdas,
                                        uint64_t *label_sector,
                                        int warnings)
{
        struct physical_volume *pv;
        struct label *label;
        struct lvmcache_info *info;
        struct device *dev;

        if (!(dev = dev_cache_get(pv_name, cmd->filter))) {
                stack;
                return NULL;
        }

        if (!(label_read(dev, &label, UINT64_C(0)))) {
                if (warnings)
                        log_error("No physical volume label read from %s",
                                  pv_name);
                return NULL;
        }

        info = (struct lvmcache_info *) label->info;
        if (label_sector && *label_sector)
                *label_sector = label->sector;

        if (!(pv = dm_pool_zalloc(cmd->mem, sizeof(*pv)))) {
                log_error("pv allocation for '%s' failed", pv_name);
                return NULL;
        }

        list_init(&pv->tags);
        list_init(&pv->segments);

        /* FIXME Move more common code up here */
        if (!(info->fmt->ops->pv_read(info->fmt, pv_name, pv, mdas))) {
                log_error("Failed to read existing physical volume '%s'",
                          pv_name);
                return NULL;
        }

        if (!pv->size)
                return NULL;

        if (!alloc_pv_segment_whole_pv(cmd->mem, pv)) {
                stack;
                return NULL;
        }

        return pv;
}

/* May return empty list */
struct list *get_vgs(struct cmd_context *cmd, int full_scan)
{
        return lvmcache_get_vgnames(cmd, full_scan);
}

struct list *get_vgids(struct cmd_context *cmd, int full_scan)
{
        return lvmcache_get_vgids(cmd, full_scan);
}

struct list *get_pvs(struct cmd_context *cmd)
{
        struct str_list *strl;
        struct list *results;
        const char *vgname, *vgid;
        struct list *pvh, *tmp;
        struct list *vgids;
        struct volume_group *vg;
        int consistent = 0;
        int old_partial;
        int old_pvmove;

        lvmcache_label_scan(cmd, 0);

        if (!(results = dm_pool_alloc(cmd->mem, sizeof(*results)))) {
                log_error("PV list allocation failed");
                return NULL;
        }

        list_init(results);

        /* Get list of VGs */
        if (!(vgids = get_vgids(cmd, 0))) {
                log_error("get_pvs: get_vgs failed");
                return NULL;
        }

        /* Read every VG to ensure cache consistency */
        /* Orphan VG is last on list */
        old_partial = partial_mode();
        old_pvmove = pvmove_mode();
        init_partial(1);
        init_pvmove(1);
        //list_iterate_items(strl, vgids) {
        list_iterate_items(strl, struct str_list, vgids) {
                vgid = strl->str;
                if (!vgid)
                        continue;       /* FIXME Unnecessary? */
                consistent = 0;
                if (!(vgname = vgname_from_vgid(NULL, vgid))) {
                        stack;
                        continue;
                }
                if (!(vg = vg_read(cmd, vgname, vgid, &consistent))) {
                        stack;
                        continue;
                }
                if (!consistent)
                        log_warn("WARNING: Volume Group %s is not consistent",
                                 vgname);

                /* Move PVs onto results list */
                list_iterate_safe(pvh, tmp, &vg->pvs) {
                        list_add(results, pvh);
                }
        }
        init_pvmove(old_pvmove);
        init_partial(old_partial);

        return results;
}

/* FIXME: liblvm todo - make into function that takes handle */
int pv_write(struct cmd_context *cmd, //__attribute((unused)),
             struct physical_volume *pv,
             struct list *mdas, int64_t label_sector)
{
        return _pv_write(cmd, pv, mdas, label_sector);
}

static int _pv_write(struct cmd_context *cmd, //__attribute((unused)),
                     struct physical_volume *pv,
                     struct list *mdas, int64_t label_sector)
{
        if (!pv->fmt->ops->pv_write) {
                log_error("Format does not support writing physical volumes");
                return 0;
        }

        if (*pv->vg_name || pv->pe_alloc_count) {
                log_error("Assertion failed: can't _pv_write non-orphan PV "
                          "(in VG %s)", pv->vg_name);
                return 0;
        }

        if (!pv->fmt->ops->pv_write(pv->fmt, pv, mdas, label_sector)) {
                stack;
                return 0;
        }

        return 1;
}

int pv_write_orphan(struct cmd_context *cmd, struct physical_volume *pv)
{
        const char *old_vg_name = pv->vg_name;

        pv->vg_name = ORPHAN;
        pv->status = ALLOCATABLE_PV;

        if (!dev_get_size(pv->dev, &pv->size)) {
                log_error("%s: Couldn't get size.", dev_name(pv->dev));
                return 0;
        }

        if (!_pv_write(cmd, pv, NULL, INT64_C(-1))) {
                log_error("Failed to clear metadata from physical "
                          "volume \"%s\" after removal from \"%s\"",
                          dev_name(pv->dev), old_vg_name);
                return 0;
        }

        return 1;
}

/**
 * is_orphan - Determine whether a pv is an orphan based on its vg_name
 * @pv: handle to the physical volume
 */
int is_orphan(pv_t *pv)
{
        return (pv_field(pv, vg_name)[0] ? 0 : 1);
}


/*
 * Returns:
 *  0 - fail
 *  1 - success
 */
int pv_analyze(struct cmd_context *cmd, const char *pv_name,
               int64_t label_sector)
{
        struct label *label;
        struct device *dev;
        struct metadata_area *mda;
        struct lvmcache_info *info;

        dev = dev_cache_get(pv_name, cmd->filter);
        if (!dev) {
                log_error("Device %s not found (or ignored by filtering).",
                          pv_name);
                return 0;
        }

        /*
         * First, scan for LVM labels.
         */
        if (!label_read(dev, &label, label_sector)) {
                log_error("Could not find LVM label on %s",
                          pv_name);
                return 0;
        }

        log_print("Found label on %s, sector %"PRIu64", type=%s",
                  pv_name, label->sector, label->type);

        /*
         * Next, loop through metadata areas
         */
        info = label->info;
        //list_iterate_items(mda, &info->mdas)
        list_iterate_items(mda, struct metadata_area, &info->mdas)
                mda->ops->pv_analyze_mda(info->fmt, mda);

        return 1;
}



/**
 * vg_check_status - check volume group status flags and log error
 * @vg - volume group to check status flags
 * @status - specific status flags to check (e.g. EXPORTED_VG)
 *
 * Returns:
 * 0 - fail
 * 1 - success
 */
int vg_check_status(struct volume_group *vg, uint32_t status)
{
        if ((status & CLUSTERED) &&
            (vg->status & CLUSTERED) && !locking_is_clustered() &&
            !lockingfailed()) {
                log_error("Skipping clustered volume group %s", vg->name);
                return 0;
        }

        if ((status & EXPORTED_VG) &&
            (vg->status & EXPORTED_VG)) {
                log_error("Volume group %s is exported", vg->name);
                return 0;
        }

        if ((status & LVM_WRITE) &&
            !(vg->status & LVM_WRITE)) {
                log_error("Volume group %s is read-only", vg->name);
                return 0;
        }
        if ((status & RESIZEABLE_VG) &&
            !(vg->status & RESIZEABLE_VG)) {
                log_error("Volume group %s is not resizeable.", vg->name);
                return 0;
        }

        return 1;
}


/*
 * Gets/Sets for external LVM library
 */
struct id pv_id(pv_t *pv)
{
        return pv_field(pv, id);
}

const struct format_type *pv_format_type(pv_t *pv)
{
        return pv_field(pv, fmt);
}

struct id pv_vgid(pv_t *pv)
{
        return pv_field(pv, vgid);
}

struct device *pv_dev(pv_t *pv)
{
        return pv_field(pv, dev);
}

const char *pv_vg_name(pv_t *pv)
{
        return pv_field(pv, vg_name);
}

uint64_t pv_size(pv_t *pv)
{
        return pv_field(pv, size);
}

uint32_t pv_status(pv_t *pv)
{
        return pv_field(pv, status);
}

uint32_t pv_pe_size(pv_t *pv)
{
        return pv_field(pv, pe_size);
}

uint64_t pv_pe_start(pv_t *pv)
{
        return pv_field(pv, pe_start);
}

uint32_t pv_pe_count(pv_t *pv)
{
        return pv_field(pv, pe_count);
}

uint32_t pv_pe_alloc_count(pv_t *pv)
{
        return pv_field(pv, pe_alloc_count);
}

uint32_t vg_status(vg_t *vg)
{
        return vg->status;
}


/**
 * pv_by_path - Given a device path return a PV handle if it is a PV
 * @cmd - handle to the LVM command instance
 * @pv_name - device path to read for the PV
 *
 * Returns:
 *  NULL - device path does not contain a valid PV
 *  non-NULL - PV handle corresponding to device path
 *
 * FIXME: merge with find_pv_by_name ?
 */
pv_t *pv_by_path(struct cmd_context *cmd, const char *pv_name)
{
        struct list mdas;

        list_init(&mdas);
        return _pv_read(cmd, pv_name, &mdas, NULL, 1);
}
