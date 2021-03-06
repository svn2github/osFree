/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved. 
 * Copyright (C) 2004-2005 Red Hat, Inc. All rights reserved.
 * Copyright (C) 2005 Zak Kipling. All rights reserved.
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

#include "tools.h"

struct pvresize_params {
	uint64_t new_size;

	unsigned done;
	unsigned total;
};

static int _pvresize_single(struct cmd_context *cmd,
			    struct volume_group *vg,
			    struct physical_volume *pv,
			    void *handle)
{
	struct pv_list *pvl;
	int consistent = 1;
	uint64_t size = 0;
	uint32_t new_pe_count = 0;
	struct list mdas;
	const char *pv_name = dev_name(pv_dev(pv));
	struct pvresize_params *params = (struct pvresize_params *) handle;
	const char *vg_name;

	list_init(&mdas);

	params->total++;

	if (!*pv_vg_name(pv)) {
		vg_name = ORPHAN;

		if (!lock_vol(cmd, vg_name, LCK_VG_WRITE)) {
			log_error("Can't get lock for orphans");
			return ECMD_FAILED;
		}

		if (!(pv = pv_read(cmd, pv_name, &mdas, NULL, 1))) {
			unlock_vg(cmd, vg_name);
			log_error("Unable to read PV \"%s\"", pv_name);
			return ECMD_FAILED;
		}

		/* FIXME Create function to test compatibility properly */
		if (list_size(&mdas) > 1) {
			log_error("%s: too many metadata areas for pvresize",
				  pv_name);
			unlock_vg(cmd, vg_name);
			return ECMD_FAILED;
		}
	} else {
		vg_name = pv_vg_name(pv);

		if (!lock_vol(cmd, vg_name, LCK_VG_WRITE)) {
			log_error("Can't get lock for %s", pv_vg_name(pv));
			return ECMD_FAILED;
		}

		if (!(vg = vg_read(cmd, vg_name, NULL, &consistent))) {
			unlock_vg(cmd, vg_name);
			log_error("Unable to find volume group of \"%s\"",
				  pv_name);
			return ECMD_FAILED;
		}

		if (!vg_check_status(vg, CLUSTERED | EXPORTED_VG | LVM_WRITE)) {
			unlock_vg(cmd, vg_name);
			return ECMD_FAILED;
		}

		if (!(pvl = find_pv_in_vg(vg, pv_name))) {
			unlock_vg(cmd, vg_name);
			log_error("Unable to find \"%s\" in volume group \"%s\"",
				  pv_name, vg->name);
			return ECMD_FAILED;
		}

		pv = pvl->pv;

		if (!archive(vg))
			return ECMD_FAILED;
	}

	if (!(pv->fmt->features & FMT_RESIZE_PV)) {
		log_error("Physical volume %s format does not support resizing.",
			  pv_name);
		unlock_vg(cmd, vg_name);
		return ECMD_FAILED;
	}

	/* Get new size */
	if (!dev_get_size(pv_dev(pv), &size)) {
		log_error("%s: Couldn't get size.", pv_name);
		unlock_vg(cmd, vg_name);
		return ECMD_FAILED;
	}
	
	if (params->new_size) {
		if (params->new_size > size)
			log_warn("WARNING: %s: Overriding real size. "
				  "You could lose data.", pv_name);
		log_verbose("%s: Pretending size is %" PRIu64 " not %" PRIu64
			    " sectors.", pv_name, params->new_size, pv_size(pv));
		size = params->new_size;
	}

	if (size < PV_MIN_SIZE) {
		log_error("%s: Size must exceed minimum of %ld sectors.",
			  pv_name, PV_MIN_SIZE);
		unlock_vg(cmd, vg_name);
		return ECMD_FAILED;
	}

	if (size < pv_pe_start(pv)) {
		log_error("%s: Size must exceed physical extent start of "
			  "%" PRIu64 " sectors.", pv_name, pv_pe_start(pv));
		unlock_vg(cmd, vg_name);
		return ECMD_FAILED;
	}

	pv->size = size;

	if (vg) {
		pv->size -= pv_pe_start(pv);
		new_pe_count = pv_size(pv) / vg->extent_size;
		
 		if (!new_pe_count) {
			log_error("%s: Size must leave space for at "
				  "least one physical extent of "
				  "%" PRIu32 " sectors.", pv_name,
				  pv_pe_size(pv));
			unlock_vg(cmd, vg_name);
			return ECMD_FAILED;
		}

		if (!pv_resize(pv, vg, new_pe_count)) {
			stack;
			unlock_vg(cmd, vg_name);
			return ECMD_FAILED;
		}
	}

	log_verbose("Resizing volume \"%s\" to %" PRIu64 " sectors.",
		    pv_name, pv_size(pv));

	log_verbose("Updating physical volume \"%s\"", pv_name);
	if (*pv_vg_name(pv)) {
		if (!vg_write(vg) || !vg_commit(vg)) {
			unlock_vg(cmd, pv_vg_name(pv));
			log_error("Failed to store physical volume \"%s\" in "
				  "volume group \"%s\"", pv_name, vg->name);
			return ECMD_FAILED;
		}
		backup(vg);
		unlock_vg(cmd, vg_name);
	} else {
		if (!(pv_write(cmd, pv, NULL, INT64_C(-1)))) {
			unlock_vg(cmd, ORPHAN);
			log_error("Failed to store physical volume \"%s\"",
				  pv_name);
			return ECMD_FAILED;
		}
		unlock_vg(cmd, vg_name);
	}

	log_print("Physical volume \"%s\" changed", pv_name);

	params->done++;

	return 1;
}

int pvresize(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvresize_params params;
	int ret;

	if (!argc) {
		log_error("Please supply physical volume(s)");
		return EINVALID_CMD_LINE;
	}

	if (arg_sign_value(cmd, physicalvolumesize_ARG, 0) == SIGN_MINUS) {
		log_error("Physical volume size may not be negative");
		return 0;
	}

	params.new_size = arg_uint64_value(cmd, physicalvolumesize_ARG,
					   UINT64_C(0)) * 2;

	params.done = 0;
	params.total = 0;

	ret = process_each_pv(cmd, argc, argv, NULL, &params, _pvresize_single);

	log_print("%d physical volume(s) resized / %d physical volume(s) "
		  "not resized", params.done, params.total - params.done);

	return ret;
}
