/*
 * Copyright (C) 2025      Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-arch.h"
#include "core-builtin.h"
#include "core-capabilities.h"

static const stress_help_t help[] = {
	{ NULL,	"physmmap N",	  "start N workers performing /dev/mem physical page mmaps/munmaps" },
	{ NULL,	"physmmap-ops N", "stop after N /dev/mem physical page mmap/munmap bogo operations" },
	{ NULL,	NULL,		  NULL }
};

#if defined(__linux__)

typedef struct stress_physmmap {
	uintptr_t	addr;		/* address range begin */
	size_t		pages;		/* pages in range */
	uint64_t	*bitmap;	/* bitmap, 1 = mmap, 0 = don't mmap */
	size_t		bitmap_size;	/* size of bitmap in bytes */
	bool		mappable;	/* true if some pages are mappable */
	struct stress_physmmap	*next;	/* next physmmap in list */
} stress_physmmap_t;

/*
 *  stress_physmmap_supported()
 *      check if we can run this with SHIM_CAP_SYS_ADMIN capability
 */
static int stress_physmmap_supported(const char *name)
{
	if (!stress_check_capability(SHIM_CAP_SYS_ADMIN)) {
		pr_inf_skip("%s stressor will be skipped, "
			"need to be running with CAP_SYS_ADMIN "
			"rights for this stressor\n", name);
		return -1;
	}
	return 0;
}

static stress_physmmap_t *stress_physmmap_get_ranges(stress_args_t *args)
{
	FILE *fp;
	char buf[4096];
	stress_physmmap_t *head = NULL, *tail = NULL;

	fp = fopen("/proc/iomem", "r");
	if (!fp) {
		pr_inf_skip("%s: cannot open /proc/iomem, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		return NULL;
	}

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		if (strstr(buf, "System RAM")) {
			uintptr_t addr_begin, addr_end;
			stress_physmmap_t *new_physmmap;
			const size_t bitmap_obj_size = sizeof(*new_physmmap->bitmap);

			/* can parse? ignore */
			if (sscanf(buf, "%" SCNxPTR "-%" SCNxPTR, &addr_begin, &addr_end) != 2)
				continue;
			/* bad begin / end addresses? */
			if (addr_begin >= addr_end)
				continue;
			/* too small for a 1 page/ */
			if (addr_end - addr_begin < args->page_size)
				continue;

			new_physmmap = malloc(sizeof(*new_physmmap));
			if (!new_physmmap)
				break;
			new_physmmap->pages = (addr_end - addr_begin) / args->page_size;
			new_physmmap->bitmap_size = (new_physmmap->pages + bitmap_obj_size - 1) & ~(bitmap_obj_size - 1);
			new_physmmap->bitmap = malloc(new_physmmap->bitmap_size);
			if (!new_physmmap->bitmap) {
				free(new_physmmap);
				break;
			}

			(void)shim_memset(new_physmmap->bitmap, 0xff, new_physmmap->bitmap_size);
			new_physmmap->mappable = true;	/* assume true */
			new_physmmap->addr = addr_begin;
			new_physmmap->next = NULL;

			if (!head) {
				head = new_physmmap;
				tail = new_physmmap;
			} else {
				tail->next = new_physmmap;
				tail = new_physmmap;
			}
		}
	}
	(void)fclose(fp);

	if (!head) {
		pr_inf_skip("%s: could not find any System RAM entries in /proc/iomem\n",
			args->name);
	}
	return head;
}

static void stress_physmmap_free_ranges(stress_physmmap_t *head)
{
	while (head) {
		stress_physmmap_t *next = head->next;

		if (head->bitmap)
			free(head->bitmap);
		free(head);
		head = next;
	}
}

/*
 *  stress_physmmap()
 *	stress physical page lookups
 */
static int stress_physmmap(stress_args_t *args)
{
	int fd_mem;
	const size_t page_size = args->page_size;
	stress_physmmap_t *physmmap_head, *physmmap;
	uint64_t map_success = 0, map_failed = 0;
	double t1, t2;
	size_t total_pages = 0;
	bool mappable = false;

	fd_mem = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd_mem < 0) {
		pr_inf_skip("%s: could not open /dev/mem, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		return EXIT_NO_RESOURCE;
	}

	physmmap_head = stress_physmmap_get_ranges(args);
	if (!physmmap_head) {
		(void)close(fd_mem);
		return EXIT_NO_RESOURCE;
	}

	stress_set_proc_state(args->name, STRESS_STATE_SYNC_WAIT);
	stress_sync_start_wait(args);
	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	if (args->instance == 0) {
		for (physmmap = physmmap_head; physmmap; physmmap = physmmap->next)
			total_pages += physmmap->pages;
		pr_inf("%s: attempting mmap/munap %zu pages\n", args->name, total_pages);
	}

	t2 = -1.0;
	t1 = stress_time_now();
	do {
		mappable = false;
		for (physmmap = physmmap_head; physmmap; physmmap = physmmap->next) {
			register size_t i;
			register uintptr_t offset = physmmap->addr;
			register bool physmmap_mappable;

			if (!physmmap->mappable)
				continue;

			physmmap_mappable = false;
			for (i = 0; (i < physmmap->pages); i++, offset += page_size) {
				void *ptr;
				int flags;

				if (!stress_continue(args))
					goto done;

				flags = stress_mwc1() ? MAP_SHARED : MAP_PRIVATE;
#if defined(MAP_POPULATE)
				flags |= stress_mwc1() ? 0 : MAP_POPULATE;
#endif
				if (!STRESS_GETBIT(physmmap->bitmap, i))
					continue;
				ptr = mmap(NULL, page_size, PROT_READ, flags, fd_mem, (off_t)offset);
				if (ptr != MAP_FAILED) {
					map_success++;
					mappable = true;
					physmmap_mappable = true;
					(void)munmap(ptr, page_size);
				} else {
					map_failed++;
					STRESS_CLRBIT(physmmap->bitmap, i);
				}
				stress_bogo_inc(args);
			}
			physmmap->mappable = physmmap_mappable;
		}
		if (t2 < 0.0)
			t2 = stress_time_now();
	} while (mappable && stress_continue(args));
done:
	if (!mappable)
		pr_inf("%s: unable to mmap any pages from /dev/mem\n", args->name);
	if ((args->instance == 0) && (t2 > 0.0)) {
		uint64_t mappable_pages = 0;

		mappable_pages = 0;
		for (physmmap = physmmap_head; physmmap; physmmap = physmmap->next) {
			register size_t i;

			if (!physmmap->mappable)
				continue;
			for (i = 0; (i < physmmap->pages); i++) {
				if (STRESS_GETBIT(physmmap->bitmap, i))
					mappable_pages++;
			}
		}
		pr_dbg("%s: %.2f seconds to perform initial %zu page /dev/mem mmap scan, %zu pages were mappable\n",
			args->name, t2 - t1, total_pages, mappable_pages);
	}

	stress_metrics_set(args, 0, "/dev/kmem pages mmapped", (double)map_success, STRESS_METRIC_TOTAL);
	stress_metrics_set(args, 1, "/dev/kmem pages not mmappable", (double)map_failed , STRESS_METRIC_TOTAL);

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	(void)close(fd_mem);
	stress_physmmap_free_ranges(physmmap_head);

	return EXIT_SUCCESS;
}

const stressor_info_t stress_physmmap_info = {
	.stressor = stress_physmmap,
	.supported = stress_physmmap_supported,
	.class = CLASS_VM,
	.verify = VERIFY_ALWAYS,
	.help = help
};
#else
const stressor_info_t stress_physmmap_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_VM,
	.verify = VERIFY_ALWAYS,
	.help = help,
	.unimplemented_reason = "only supported on Linux"
};
#endif
