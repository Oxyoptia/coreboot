/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2015 Intel Corp.
 * (Written by Andrey Petrov <andrey.petrov@intel.com> for Intel Corp.)
 * (Written by Alexandru Gagniuc <alexandrux.gagniuc@intel.com> for Intel Corp.)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <arch/io.h>
#include <arch/cpu.h>
#include <arch/symbols.h>
#include <bootmode.h>
#include <cbfs.h>
#include <cbmem.h>
#include <console/console.h>
#include <fsp/api.h>
#include <fsp/util.h>
#include <memrange.h>
#include <program_loading.h>
#include <reset.h>
#include <romstage_handoff.h>
#include <soc/intel/common/mrc_cache.h>
#include <string.h>
#include <symbols.h>
#include <timestamp.h>

typedef asmlinkage enum fsp_status (*fsp_memory_init_fn)
				   (void *raminit_upd, void **hob_list);

static void save_memory_training_data(bool s3wake, uint32_t fsp_version)
{
	size_t  mrc_data_size;
	const void *mrc_data;

	if (!IS_ENABLED(CONFIG_CACHE_MRC_SETTINGS) || s3wake)
		return;

	mrc_data = fsp_find_nv_storage_data(&mrc_data_size);
	if (!mrc_data) {
		printk(BIOS_ERR, "Couldn't find memory training data HOB.\n");
		return;
	}

	/*
	 * Save MRC Data to CBMEM. By always saving the data this forces
	 * a retrain after a trip through Chrome OS recovery path. The
	 * code which saves the data to flash doesn't write if the latest
	 * training data matches this one.
	 */
	if (mrc_cache_stash_data_with_version(mrc_data, mrc_data_size,
						fsp_version) < 0)
			printk(BIOS_ERR, "Failed to stash MRC data\n");
}

static enum fsp_status do_fsp_post_memory_init(void *hob_list_ptr, bool s3wake,
						uint32_t fsp_version)
{
	struct range_entry fsp_mem;
	struct romstage_handoff *handoff;

	fsp_find_reserved_memory(&fsp_mem, hob_list_ptr);

	/* initialize cbmem by adding FSP reserved memory first thing */
	if (!s3wake) {
		cbmem_initialize_empty_id_size(CBMEM_ID_FSP_RESERVED_MEMORY,
			range_entry_size(&fsp_mem));
	} else if (cbmem_initialize_id_size(CBMEM_ID_FSP_RESERVED_MEMORY,
				range_entry_size(&fsp_mem))) {
		if (IS_ENABLED(CONFIG_HAVE_ACPI_RESUME)) {
			printk(BIOS_DEBUG, "Failed to recover CBMEM in S3 resume.\n");
			/* Failed S3 resume, reset to come up cleanly */
			hard_reset();
		}
	}

	/* make sure FSP memory is reserved in cbmem */
	if (range_entry_base(&fsp_mem) !=
		(uintptr_t)cbmem_find(CBMEM_ID_FSP_RESERVED_MEMORY))
		die("Failed to accommodate FSP reserved memory request");

	/* Now that CBMEM is up, save the list so ramstage can use it */
	fsp_save_hob_list(hob_list_ptr);

	save_memory_training_data(s3wake, fsp_version);

	/* Create romstage handof information */
	handoff = romstage_handoff_find_or_add();
	if (handoff != NULL)
		handoff->s3_resume = s3wake;
	else
		printk(BIOS_DEBUG, "Romstage handoff structure not added!\n");

	return FSP_SUCCESS;
}

static void fsp_fill_mrc_cache(struct FSPM_ARCH_UPD *arch_upd, bool s3wake,
				uint32_t fsp_version)
{
	const struct mrc_saved_data *mrc_cache;

	arch_upd->NvsBufferPtr = NULL;

	if (!IS_ENABLED(CONFIG_CACHE_MRC_SETTINGS))
		return;

	/* Don't use saved training data when recovery mode is enabled. */
	if (recovery_mode_enabled()) {
		printk(BIOS_DEBUG, "Recovery mode. Not using MRC cache.\n");
		return;
	}

	if (mrc_cache_get_current_with_version(&mrc_cache, fsp_version)) {
		printk(BIOS_DEBUG, "MRC cache was not found\n");
		return;
	}

	/* MRC cache found */
	arch_upd->NvsBufferPtr = (void *)mrc_cache->data;
	arch_upd->BootMode = s3wake ?
		FSP_BOOT_ON_S3_RESUME:
		FSP_BOOT_ASSUMING_NO_CONFIGURATION_CHANGES;
	printk(BIOS_DEBUG, "MRC cache found, size %x bootmode:%d\n",
				mrc_cache->size, arch_upd->BootMode);
}

static enum cb_err check_region_overlap(const struct memranges *ranges,
					const char *description,
					uintptr_t begin, uintptr_t end)
{
	const struct range_entry *r;

	memranges_each_entry(r, ranges) {
		if (end <= range_entry_base(r))
			continue;
		if (begin >= range_entry_end(r))
			continue;
		printk(BIOS_ERR, "'%s' overlaps currently running program: "
			"[%p, %p)\n", description, (void *)begin, (void *)end);
		return CB_ERR;
	}

	return CB_SUCCESS;
}

static enum cb_err fsp_fill_common_arch_params(struct FSPM_ARCH_UPD *arch_upd,
					bool s3wake, uint32_t fsp_version,
					const struct memranges *memmap)
{
	uintptr_t stack_begin;
	uintptr_t stack_end;

	/*
	 * FSPM_UPD passed here is populated with default values provided by
	 * the blob itself. We let FSPM use top of CAR region of the size it
	 * requests.
	 */
	stack_end = (uintptr_t)_car_region_end;
	stack_begin = stack_end - arch_upd->StackSize;

	if (check_region_overlap(memmap, "FSPM stack", stack_begin,
				stack_end) != CB_SUCCESS)
		return CB_ERR;

	arch_upd->StackBase = (void *)stack_begin;

	arch_upd->BootMode = FSP_BOOT_WITH_FULL_CONFIGURATION;

	fsp_fill_mrc_cache(arch_upd, s3wake, fsp_version);

	return CB_SUCCESS;
}

static enum fsp_status do_fsp_memory_init(struct fsp_header *hdr, bool s3wake,
					const struct memranges *memmap)
{
	enum fsp_status status;
	fsp_memory_init_fn fsp_raminit;
	struct FSPM_UPD fspm_upd, *upd;
	void *hob_list_ptr;
	struct FSPM_ARCH_UPD *arch_upd;

	post_code(0x34);

	upd = (struct FSPM_UPD *)(hdr->cfg_region_offset + hdr->image_base);

	if (upd->FspUpdHeader.Signature != FSPM_UPD_SIGNATURE) {
		printk(BIOS_ERR, "Invalid FSPM signature\n");
		return FSP_INCOMPATIBLE_VERSION;
	}

	/* Copy the default values from the UPD area */
	memcpy(&fspm_upd, upd, sizeof(fspm_upd));

	arch_upd = &fspm_upd.FspmArchUpd;

	/* Reserve enough memory under TOLUD to save CBMEM header */
	arch_upd->BootLoaderTolumSize = cbmem_overhead_size();

	/* Fill common settings on behalf of chipset. */
	if (fsp_fill_common_arch_params(arch_upd, s3wake, hdr->fsp_revision,
					memmap) != CB_SUCCESS)
		return FSP_NOT_FOUND;

	/* Give SoC and mainboard a chance to update the UPD */
	platform_fsp_memory_init_params_cb(&fspm_upd);

	/* Call FspMemoryInit */
	fsp_raminit = (void *)(hdr->image_base + hdr->memory_init_entry_offset);
	printk(BIOS_DEBUG, "Calling FspMemoryInit: 0x%p\n", fsp_raminit);
	printk(BIOS_SPEW, "\t%p: raminit_upd\n", &fspm_upd);
	printk(BIOS_SPEW, "\t%p: hob_list ptr\n", &hob_list_ptr);

	post_code(POST_FSP_MEMORY_INIT);
	timestamp_add_now(TS_FSP_MEMORY_INIT_START);
	status = fsp_raminit(&fspm_upd, &hob_list_ptr);
	post_code(POST_FSP_MEMORY_INIT);
	timestamp_add_now(TS_FSP_MEMORY_INIT_END);

	printk(BIOS_DEBUG, "FspMemoryInit returned 0x%08x\n", status);

	/* Handle any resets requested by FSPM. */
	fsp_handle_reset(status);

	if (status != FSP_SUCCESS)
		return status;

	return do_fsp_post_memory_init(hob_list_ptr, s3wake, hdr->fsp_revision);
}

/* Load the binary into the memory specified by the info header. */
static enum cb_err load_fspm_mem(struct fsp_header *hdr,
					const struct region_device *rdev,
					const struct memranges *memmap)
{
	uintptr_t fspm_begin;
	uintptr_t fspm_end;

	if (fsp_validate_component(hdr, rdev) != CB_SUCCESS)
		return CB_ERR;

	fspm_begin = hdr->image_base;
	fspm_end = fspm_begin + hdr->image_size;

	if (check_region_overlap(memmap, "FSPM", fspm_begin, fspm_end) !=
		CB_SUCCESS)
		return CB_ERR;

	/* Load binary into memory at provided address. */
	if (rdev_readat(rdev, (void *)fspm_begin, 0, fspm_end - fspm_begin) < 0)
		return CB_ERR;

	return CB_SUCCESS;
}

/* Handle the case when FSPM is running XIP. */
static enum cb_err load_fspm_xip(struct fsp_header *hdr,
					const struct region_device *rdev)
{
	void *base;

	if (fsp_validate_component(hdr, rdev) != CB_SUCCESS)
		return CB_ERR;

	base = rdev_mmap_full(rdev);
	if ((uintptr_t)base != hdr->image_base) {
		printk(BIOS_ERR, "FSPM XIP base does not match: %p vs %p\n",
			(void *)(uintptr_t)hdr->image_base, base);
		return CB_ERR;
	}

	/*
	 * Since the component is XIP it's already in the address space. Thus,
	 * there's no need to rdev_munmap().
	 */
	return CB_SUCCESS;
}

enum fsp_status fsp_memory_init(bool s3wake)
{
	struct fsp_header hdr;
	enum cb_err status;
	struct cbfsf file_desc;
	struct region_device file_data;
	const char *name = CONFIG_FSP_M_CBFS;
	struct memranges memmap;
	struct range_entry freeranges[2];

	if (cbfs_boot_locate(&file_desc, name, NULL)) {
		printk(BIOS_ERR, "Could not locate %s in CBFS\n", name);
		return FSP_NOT_FOUND;
	}

	cbfs_file_data(&file_data, &file_desc);

	/* Build up memory map of romstage address space including CAR. */
	memranges_init_empty(&memmap, &freeranges[0], ARRAY_SIZE(freeranges));
	memranges_insert(&memmap, (uintptr_t)_car_region_start,
		_car_relocatable_data_end - _car_region_start, 0);
	memranges_insert(&memmap, (uintptr_t)_program, _program_size, 0);

	if (IS_ENABLED(CONFIG_NO_XIP_EARLY_STAGES))
		status = load_fspm_mem(&hdr, &file_data, &memmap);
	else
		status = load_fspm_xip(&hdr, &file_data);

	if (status != CB_SUCCESS) {
		printk(BIOS_ERR, "Loading FSPM failed.\n");
		return FSP_NOT_FOUND;
	}

	/* Signal that FSP component has been loaded. */
	prog_segment_loaded(hdr.image_base, hdr.image_size, SEG_FINAL);

	return do_fsp_memory_init(&hdr, s3wake, &memmap);
}
