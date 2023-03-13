/*
 * Based on arch/arm/mm/init.c
 *
 * Copyright (C) 1995-2005 Russell King
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/errno.h>
#include <linux/swap.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/cache.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/initrd.h>
#include <linux/gfp.h>
#include <linux/memblock.h>
#include <linux/sort.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/dma-mapping.h>
#include <linux/dma-contiguous.h>
#include <linux/efi.h>
#include <linux/swiotlb.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/kexec.h>
#include <linux/crash_dump.h>
#include <linux/iommu.h>
#include <linux/suspend.h>
#ifdef CONFIG_PIN_MEMORY
#include <linux/pin_mem.h>
#endif

#include <asm/boot.h>
#include <asm/fixmap.h>
#include <asm/kasan.h>
#include <asm/kernel-pgtable.h>
#include <asm/memory.h>
#include <asm/numa.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/sizes.h>
#include <asm/tlb.h>
#include <asm/alternative.h>

/*
 * We need to be able to catch inadvertent references to memstart_addr
 * that occur (potentially in generic code) before arm64_memblock_init()
 * executes, which assigns it its actual value. So use a default value
 * that cannot be mistaken for a real physical address.
 */
s64 memstart_addr __ro_after_init = -1;
phys_addr_t arm64_dma_phys_limit __ro_after_init;

struct res_mem res_mem[MAX_RES_REGIONS];
int res_mem_count;

#ifdef CONFIG_BLK_DEV_INITRD
static int __init early_initrd(char *p)
{
	unsigned long start, size;
	char *endp;

	start = memparse(p, &endp);
	if (*endp == ',') {
		size = memparse(endp + 1, NULL);

		initrd_start = start;
		initrd_end = start + size;
	}
	return 0;
}
early_param("initrd", early_initrd);
#endif

/* The main usage of linux,usable-memory-range is for crash dump kernel.
 * Originally, the number of usable-memory regions is one. Now crash dump
 * kernel support at most two crash kernel regions, low_region and high
 * region.
 */
#define MAX_USABLE_RANGES	2

#ifdef CONFIG_PIN_MEMORY
struct resource pin_memory_resource = {
	.name = "Pin memory",
	.start = 0,
	.end = 0,
	.flags = IORESOURCE_MEM,
	.desc = IOMMU_RESV_RESERVED
};

static void __init reserve_pin_memory_res(void)
{
	unsigned long long mem_start, mem_len;
	int ret;

	ret = parse_pin_memory(boot_command_line, memblock_phys_mem_size(),
		&mem_len, &mem_start);
	if (ret || !mem_len)
		return;

	mem_len = PAGE_ALIGN(mem_len);

	if (!memblock_is_region_memory(mem_start, mem_len)) {
		pr_warn("cannot reserve for pin memory: region is not memory!\n");
		return;
	}

	if (memblock_is_region_reserved(mem_start, mem_len)) {
		pr_warn("cannot reserve for pin memory: region overlaps reserved memory!\n");
		return;
	}

	if (!IS_ALIGNED(mem_start, SZ_2M)) {
		pr_warn("cannot reserve for pin memory: base address is not 2MB aligned\n");
		return;
	}

	memblock_reserve(mem_start, mem_len);
	pin_memory_resource.start = mem_start;
	pin_memory_resource.end = mem_start + mem_len - 1;
}
#else
static void __init reserve_pin_memory_res(void)
{
}
#endif /* CONFIG_PIN_MEMORY */

#ifdef CONFIG_KEXEC_CORE

/*
 * reserve_crashkernel() - reserves memory for crash kernel
 *
 * This function reserves memory area given in "crashkernel=" kernel command
 * line parameter. The memory reserved is used by dump capture kernel when
 * primary kernel is crashing.
 */
static void __init reserve_crashkernel(void)
{
	unsigned long long crash_base, crash_size;
	bool high = false;
	int ret;

	ret = parse_crashkernel(boot_command_line, memblock_phys_mem_size(),
				&crash_size, &crash_base);
	/* no crashkernel= or invalid value specified */
	if (ret || !crash_size) {
		/* crashkernel=X,high */
		ret = parse_crashkernel_high(boot_command_line,
				memblock_phys_mem_size(),
				&crash_size, &crash_base);
		if (ret || !crash_size)
			return;
		high = true;
	}

	crash_size = PAGE_ALIGN(crash_size);

	if (crash_base == 0) {
		/* Current arm64 boot protocol requires 2MB alignment */
		crash_base = memblock_find_in_range(0,
				high ? memblock_end_of_DRAM()
				: ARCH_LOW_ADDRESS_LIMIT,
				crash_size, CRASH_ALIGN);
		if (crash_base == 0) {
			pr_warn("cannot allocate crashkernel (size:0x%llx)\n",
				crash_size);
			return;
		}
	} else {
		/* User specifies base address explicitly. */
		if (!memblock_is_region_memory(crash_base, crash_size)) {
			pr_warn("cannot reserve crashkernel: region is not memory\n");
			return;
		}

		if (memblock_is_region_reserved(crash_base, crash_size)) {
			pr_warn("cannot reserve crashkernel: region overlaps reserved memory\n");
			return;
		}

		if (!IS_ALIGNED(crash_base, CRASH_ALIGN)) {
			pr_warn("cannot reserve crashkernel: base address is not 2MB aligned\n");
			return;
		}
	}
	memblock_reserve(crash_base, crash_size);

	if (crash_base >= SZ_4G && reserve_crashkernel_low()) {
		memblock_free(crash_base, crash_size);
		return;
	}

	pr_info("crashkernel reserved: 0x%016llx - 0x%016llx (%lld MB)\n",
		crash_base, crash_base + crash_size, crash_size >> 20);

	crashk_res.start = crash_base;
	crashk_res.end = crash_base + crash_size - 1;
}

static void __init kexec_reserve_crashkres_pages(void)
{
#ifdef CONFIG_HIBERNATION
	phys_addr_t addr;
	struct page *page;

	if (!crashk_res.end)
		return;

	/*
	 * To reduce the size of hibernation image, all the pages are
	 * marked as Reserved initially.
	 */
	for (addr = crashk_res.start; addr < (crashk_res.end + 1);
			addr += PAGE_SIZE) {
		page = phys_to_page(addr);
		SetPageReserved(page);
	}
#endif
}
#else
static void __init reserve_crashkernel(void)
{
}

static void __init kexec_reserve_crashkres_pages(void)
{
}
#endif /* CONFIG_KEXEC_CORE */

#ifdef CONFIG_QUICK_KEXEC
static int __init parse_quick_kexec(char *p)
{
	if (!p)
		return 0;

	quick_kexec_res.end = PAGE_ALIGN(memparse(p, NULL));

	return 0;
}
early_param("quickkexec", parse_quick_kexec);

static void __init reserve_quick_kexec(void)
{
	unsigned long long mem_start, mem_len;

	mem_len = quick_kexec_res.end;
	if (mem_len == 0)
		return;

	/* Current arm64 boot protocol requires 2MB alignment */
	mem_start = memblock_find_in_range(0, ARCH_LOW_ADDRESS_LIMIT,
			mem_len, CRASH_ALIGN);
	if (mem_start == 0) {
		pr_warn("cannot allocate quick kexec mem (size:0x%llx)\n",
			mem_len);
		quick_kexec_res.end = 0;
		return;
	}

	memblock_reserve(mem_start, mem_len);
	pr_info("quick kexec mem reserved: 0x%016llx - 0x%016llx (%lld MB)\n",
		mem_start, mem_start + mem_len,	mem_len >> 20);

	quick_kexec_res.start = mem_start;
	quick_kexec_res.end = mem_start + mem_len - 1;
}
#endif

#ifdef CONFIG_CRASH_DUMP
static int __init early_init_dt_scan_elfcorehdr(unsigned long node,
		const char *uname, int depth, void *data)
{
	const __be32 *reg;
	int len;

	if (depth != 1 || strcmp(uname, "chosen") != 0)
		return 0;

	reg = of_get_flat_dt_prop(node, "linux,elfcorehdr", &len);
	if (!reg || (len < (dt_root_addr_cells + dt_root_size_cells)))
		return 1;

	elfcorehdr_addr = dt_mem_next_cell(dt_root_addr_cells, &reg);
	elfcorehdr_size = dt_mem_next_cell(dt_root_size_cells, &reg);

	return 1;
}

/*
 * reserve_elfcorehdr() - reserves memory for elf core header
 *
 * This function reserves the memory occupied by an elf core header
 * described in the device tree. This region contains all the
 * information about primary kernel's core image and is used by a dump
 * capture kernel to access the system memory on primary kernel.
 */
static void __init reserve_elfcorehdr(void)
{
	of_scan_flat_dt(early_init_dt_scan_elfcorehdr, NULL);

	if (!elfcorehdr_size)
		return;

	if (memblock_is_region_reserved(elfcorehdr_addr, elfcorehdr_size)) {
		pr_warn("elfcorehdr is overlapped\n");
		return;
	}

	memblock_reserve(elfcorehdr_addr, elfcorehdr_size);

	pr_info("Reserving %lldKB of memory at 0x%llx for elfcorehdr\n",
		elfcorehdr_size >> 10, elfcorehdr_addr);
}
#else
static void __init reserve_elfcorehdr(void)
{
}
#endif /* CONFIG_CRASH_DUMP */
/*
 * Return the maximum physical address for ZONE_DMA32 (DMA_BIT_MASK(32)). It
 * currently assumes that for memory starting above 4G, 32-bit devices will
 * use a DMA offset.
 */
static phys_addr_t __init max_zone_dma_phys(void)
{
	phys_addr_t offset = memblock_start_of_DRAM() & GENMASK_ULL(63, 32);
	return min(offset + (1ULL << 32), memblock_end_of_DRAM());
}

#ifdef CONFIG_NUMA

static void __init zone_sizes_init(unsigned long min, unsigned long max)
{
	unsigned long max_zone_pfns[MAX_NR_ZONES]  = {0};

#ifdef CONFIG_ZONE_DMA32
	max_zone_pfns[ZONE_DMA32] = PFN_DOWN(max_zone_dma_phys());
#endif
	max_zone_pfns[ZONE_NORMAL] = max;

	free_area_init_nodes(max_zone_pfns);
}

#else

static void __init zone_sizes_init(unsigned long min, unsigned long max)
{
	struct memblock_region *reg;
	unsigned long zone_size[MAX_NR_ZONES], zhole_size[MAX_NR_ZONES];
	unsigned long max_dma = min;

	memset(zone_size, 0, sizeof(zone_size));

	/* 4GB maximum for 32-bit only capable devices */
#ifdef CONFIG_ZONE_DMA32
	max_dma = PFN_DOWN(arm64_dma_phys_limit);
	zone_size[ZONE_DMA32] = max_dma - min;
#endif
	zone_size[ZONE_NORMAL] = max - max_dma;

	memcpy(zhole_size, zone_size, sizeof(zhole_size));

	for_each_memblock(memory, reg) {
		unsigned long start = memblock_region_memory_base_pfn(reg);
		unsigned long end = memblock_region_memory_end_pfn(reg);

		if (start >= max)
			continue;

#ifdef CONFIG_ZONE_DMA32
		if (start < max_dma) {
			unsigned long dma_end = min(end, max_dma);
			zhole_size[ZONE_DMA32] -= dma_end - start;
		}
#endif
		if (end > max_dma) {
			unsigned long normal_end = min(end, max);
			unsigned long normal_start = max(start, max_dma);
			zhole_size[ZONE_NORMAL] -= normal_end - normal_start;
		}
	}

	free_area_init_node(0, zone_size, min, zhole_size);
}

#endif /* CONFIG_NUMA */

#ifdef CONFIG_HAVE_ARCH_PFN_VALID
int pfn_valid(unsigned long pfn)
{
	phys_addr_t addr = pfn << PAGE_SHIFT;

	if ((addr >> PAGE_SHIFT) != pfn)
		return 0;

#ifdef CONFIG_SPARSEMEM
	if (pfn_to_section_nr(pfn) >= NR_MEM_SECTIONS)
		return 0;

	if (!valid_section(__nr_to_section(pfn_to_section_nr(pfn))))
		return 0;
#endif
	return memblock_is_map_memory(addr);
}
EXPORT_SYMBOL(pfn_valid);
#endif

#ifndef CONFIG_SPARSEMEM
static void __init arm64_memory_present(void)
{
}
#else
static void __init arm64_memory_present(void)
{
	struct memblock_region *reg;

	for_each_memblock(memory, reg) {
		int nid = memblock_get_region_node(reg);

		memory_present(nid, memblock_region_memory_base_pfn(reg),
				memblock_region_memory_end_pfn(reg));
	}
}
#endif

static phys_addr_t memory_limit = PHYS_ADDR_MAX;

/*
 * Limit the memory size that was specified via FDT.
 */
static int __init early_mem(char *p)
{
	if (!p)
		return 1;

	memory_limit = memparse(p, &p) & PAGE_MASK;
	pr_notice("Memory limited to %lldMB\n", memory_limit >> 20);

	return 0;
}
early_param("mem", early_mem);

static int __init early_init_dt_scan_usablemem(unsigned long node,
		const char *uname, int depth, void *data)
{
	struct memblock_type *usablemem = data;
	const __be32 *reg, *endp;
	int len, nr = 0;

	if (depth != 1 || strcmp(uname, "chosen") != 0)
		return 0;

	reg = of_get_flat_dt_prop(node, "linux,usable-memory-range", &len);
	if (!reg || (len < (dt_root_addr_cells + dt_root_size_cells)))
		return 1;

	endp = reg + (len / sizeof(__be32));
	while ((endp - reg) >= (dt_root_addr_cells + dt_root_size_cells)) {
		unsigned long base = dt_mem_next_cell(dt_root_addr_cells, &reg);
		unsigned long size = dt_mem_next_cell(dt_root_size_cells, &reg);

		if (memblock_add_range(usablemem, base, size, NUMA_NO_NODE,
				       MEMBLOCK_NONE))
			return 0;
		if (++nr >= MAX_USABLE_RANGES)
			break;
	}

	return 1;
}

static void __init fdt_enforce_memory_region(void)
{
	struct memblock_region usable_regions[MAX_USABLE_RANGES];
	struct memblock_type usablemem = {
		.max = MAX_USABLE_RANGES,
		.regions = usable_regions,
	};

	of_scan_flat_dt(early_init_dt_scan_usablemem, &usablemem);

	if (usablemem.cnt)
		memblock_cap_memory_ranges(usablemem.regions, usablemem.cnt);
}

static int __init parse_memmap_one(char *p)
{
	char *oldp;
	phys_addr_t start_at, mem_size;
	int ret;

	if (!p)
		return -EINVAL;

	oldp = p;
	mem_size = memparse(p, &p);
	if (p == oldp)
		return -EINVAL;

	if (!mem_size)
		return -EINVAL;

	mem_size = PAGE_ALIGN(mem_size);

	if (*p == '$') {
		start_at = memparse(p+1, &p);
		if (!IS_ALIGNED(start_at, SZ_2M)) {
			pr_warn("cannot reserve memory: bad address is not 2MB aligned\n");
			return -EINVAL;
		}

		ret = memblock_reserve(start_at, mem_size);
		if (!ret) {
			res_mem[res_mem_count].base = start_at;
			res_mem[res_mem_count].size = mem_size;
			res_mem_count++;
		} else
			pr_warn("memmap memblock_reserve failed.\n");
	} else
		pr_info("Unrecognized memmap option, please check the parameter.\n");

	return *p == '\0' ? 0 : -EINVAL;
}

static int __init parse_memmap_opt(char *str)
{
	while (str) {
		char *k = strchr(str, ',');

		if (k)
			*k++ = 0;

		parse_memmap_one(str);
		str = k;
	}

	return 0;
}
early_param("memmap", parse_memmap_opt);

#ifdef CONFIG_ARM64_CPU_PARK
struct cpu_park_info park_info = {
	.start = 0,
	.len = PARK_SECTION_SIZE * NR_CPUS,
	.start_v = 0,
};

static int __init parse_park_mem(char *p)
{
	if (!p)
		return 0;

	park_info.start = PAGE_ALIGN(memparse(p, NULL));
	if (park_info.start == 0)
		pr_info("cpu park mem params[%s]", p);

	return 0;
}
early_param("cpuparkmem", parse_park_mem);

static int __init reserve_park_mem(void)
{
	if (park_info.start == 0 || park_info.len == 0)
		return 0;

	park_info.start = PAGE_ALIGN(park_info.start);
	park_info.len = PAGE_ALIGN(park_info.len);

	if (!memblock_is_region_memory(park_info.start, park_info.len)) {
		pr_warn("cannot reserve park mem: region is not memory!");
		goto out;
	}

	if (memblock_is_region_reserved(park_info.start, park_info.len)) {
		pr_warn("cannot reserve park mem: region overlaps reserved memory!");
		goto out;
	}

	memblock_remove(park_info.start, park_info.len);
	pr_info("cpu park mem reserved: 0x%016lx - 0x%016lx (%ld MB)",
		park_info.start, park_info.start + park_info.len,
		park_info.len >> 20);

	return 0;
out:
	park_info.start = 0;
	park_info.len = 0;
	return -EINVAL;
}
#endif

void __init arm64_memblock_init(void)
{
	const s64 linear_region_size = -(s64)PAGE_OFFSET;

	/* Handle linux,usable-memory-range property */
	fdt_enforce_memory_region();

	/* Remove memory above our supported physical address size */
	memblock_remove(1ULL << PHYS_MASK_SHIFT, ULLONG_MAX);

	/*
	 * Ensure that the linear region takes up exactly half of the kernel
	 * virtual address space. This way, we can distinguish a linear address
	 * from a kernel/module/vmalloc address by testing a single bit.
	 */
	BUILD_BUG_ON(linear_region_size != BIT(VA_BITS - 1));

	/*
	 * Select a suitable value for the base of physical memory.
	 */
	memstart_addr = round_down(memblock_start_of_DRAM(),
				   ARM64_MEMSTART_ALIGN);

	/*
	 * Remove the memory that we will not be able to cover with the
	 * linear mapping. Take care not to clip the kernel which may be
	 * high in memory.
	 */
	memblock_remove(max_t(u64, memstart_addr + linear_region_size,
			__pa_symbol(_end)), ULLONG_MAX);
	if (memstart_addr + linear_region_size < memblock_end_of_DRAM()) {
		/* ensure that memstart_addr remains sufficiently aligned */
		memstart_addr = round_up(memblock_end_of_DRAM() - linear_region_size,
					 ARM64_MEMSTART_ALIGN);
		memblock_remove(0, memstart_addr);
	}

	/*
	 * Apply the memory limit if it was set. Since the kernel may be loaded
	 * high up in memory, add back the kernel region that must be accessible
	 * via the linear mapping.
	 */
	if (memory_limit != PHYS_ADDR_MAX) {
		memblock_mem_limit_remove_map(memory_limit);
		memblock_add(__pa_symbol(_text), (u64)(_end - _text));
	}

	if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && initrd_start) {
		/*
		 * Add back the memory we just removed if it results in the
		 * initrd to become inaccessible via the linear mapping.
		 * Otherwise, this is a no-op
		 */
		u64 base = initrd_start & PAGE_MASK;
		u64 size = PAGE_ALIGN(initrd_end) - base;

		/*
		 * We can only add back the initrd memory if we don't end up
		 * with more memory than we can address via the linear mapping.
		 * It is up to the bootloader to position the kernel and the
		 * initrd reasonably close to each other (i.e., within 32 GB of
		 * each other) so that all granule/#levels combinations can
		 * always access both.
		 */
		if (WARN(base < memblock_start_of_DRAM() ||
			 base + size > memblock_start_of_DRAM() +
				       linear_region_size,
			"initrd not fully accessible via the linear mapping -- please check your bootloader ...\n")) {
			initrd_start = 0;
		} else {
			memblock_remove(base, size); /* clear MEMBLOCK_ flags */
			memblock_add(base, size);
			memblock_reserve(base, size);
		}
	}

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
		extern u16 memstart_offset_seed;
		u64 mmfr0 = read_cpuid(ID_AA64MMFR0_EL1);
		int parange = cpuid_feature_extract_unsigned_field(
					mmfr0, ID_AA64MMFR0_PARANGE_SHIFT);
		s64 range = linear_region_size -
			    BIT(id_aa64mmfr0_parange_to_phys_shift(parange));

		/*
		 * If the size of the linear region exceeds, by a sufficient
		 * margin, the size of the region that the physical memory can
		 * span, randomize the linear region as well.
		 */
		if (memstart_offset_seed > 0 && range >= (s64)ARM64_MEMSTART_ALIGN) {
			range /= ARM64_MEMSTART_ALIGN;
			memstart_addr -= ARM64_MEMSTART_ALIGN *
					 ((range * memstart_offset_seed) >> 16);
		}
	}

	/*
	 * Register the kernel text, kernel data, initrd, and initial
	 * pagetables with memblock.
	 */
	memblock_reserve(__pa_symbol(_text), _end - _text);
#ifdef CONFIG_BLK_DEV_INITRD
	if (initrd_start) {
		memblock_reserve(initrd_start, initrd_end - initrd_start);

		/* the generic initrd code expects virtual addresses */
		initrd_start = __phys_to_virt(initrd_start);
		initrd_end = __phys_to_virt(initrd_end);
	}
#endif

	early_init_fdt_scan_reserved_mem();

	/* 4GB maximum for 32-bit only capable devices */
	if (IS_ENABLED(CONFIG_ZONE_DMA32))
		arm64_dma_phys_limit = max_zone_dma_phys();
	else
		arm64_dma_phys_limit = PHYS_MASK + 1;

	reserve_pin_memory_res();

	/*
	 * Reserve park memory before crashkernel and quick kexec.
	 * Because park memory must be specified by address, but
	 * crashkernel and quickkexec may be specified by memory length,
	 * then find one sutiable memory region to reserve.
	 *
	 * So reserve park memory firstly is better, but it may cause
	 * crashkernel or quickkexec reserving failed.
	 */
#ifdef CONFIG_ARM64_CPU_PARK
	reserve_park_mem();
#endif

	reserve_crashkernel();

#ifdef CONFIG_QUICK_KEXEC
	reserve_quick_kexec();
#endif

	reserve_elfcorehdr();

	high_memory = __va(memblock_end_of_DRAM() - 1) + 1;

	dma_contiguous_reserve(arm64_dma_phys_limit);
}

void __init bootmem_init(void)
{
	unsigned long min, max;

	min = PFN_UP(memblock_start_of_DRAM());
	max = PFN_DOWN(memblock_end_of_DRAM());

	early_memtest(min << PAGE_SHIFT, max << PAGE_SHIFT);

	max_pfn = max_low_pfn = max;

	arm64_numa_init();
	/*
	 * Sparsemem tries to allocate bootmem in memory_present(), so must be
	 * done after the fixed reservations.
	 */
	arm64_memory_present();

	sparse_init();
	zone_sizes_init(min, max);

	memblock_dump_all();
}

#ifndef CONFIG_SPARSEMEM_VMEMMAP
static inline void free_memmap(unsigned long start_pfn, unsigned long end_pfn)
{
	struct page *start_pg, *end_pg;
	unsigned long pg, pgend;

	/*
	 * Convert start_pfn/end_pfn to a struct page pointer.
	 */
	start_pg = pfn_to_page(start_pfn - 1) + 1;
	end_pg = pfn_to_page(end_pfn - 1) + 1;

	/*
	 * Convert to physical addresses, and round start upwards and end
	 * downwards.
	 */
	pg = (unsigned long)PAGE_ALIGN(__pa(start_pg));
	pgend = (unsigned long)__pa(end_pg) & PAGE_MASK;

	/*
	 * If there are free pages between these, free the section of the
	 * memmap array.
	 */
	if (pg < pgend)
		free_bootmem(pg, pgend - pg);
}

/*
 * The mem_map array can get very big. Free the unused area of the memory map.
 */
static void __init free_unused_memmap(void)
{
	unsigned long start, prev_end = 0;
	struct memblock_region *reg;

	for_each_memblock(memory, reg) {
		start = __phys_to_pfn(reg->base);

#ifdef CONFIG_SPARSEMEM
		/*
		 * Take care not to free memmap entries that don't exist due
		 * to SPARSEMEM sections which aren't present.
		 */
		start = min(start, ALIGN(prev_end, PAGES_PER_SECTION));
#endif
		/*
		 * If we had a previous bank, and there is a space between the
		 * current bank and the previous, free it.
		 */
		if (prev_end && prev_end < start)
			free_memmap(prev_end, start);

		/*
		 * Align up here since the VM subsystem insists that the
		 * memmap entries are valid from the bank end aligned to
		 * MAX_ORDER_NR_PAGES.
		 */
		prev_end = ALIGN(__phys_to_pfn(reg->base + reg->size),
				 MAX_ORDER_NR_PAGES);
	}

#ifdef CONFIG_SPARSEMEM
	if (!IS_ALIGNED(prev_end, PAGES_PER_SECTION))
		free_memmap(prev_end, ALIGN(prev_end, PAGES_PER_SECTION));
#endif
}
#endif	/* !CONFIG_SPARSEMEM_VMEMMAP */

/*
 * mem_init() marks the free areas in the mem_map and tells us how much memory
 * is free.  This is done after various parts of the system have claimed their
 * memory after the kernel image.
 */
void __init mem_init(void)
{
	if (swiotlb_force == SWIOTLB_FORCE ||
	    max_pfn > (arm64_dma_phys_limit >> PAGE_SHIFT))
		swiotlb_init(1);
	else
		swiotlb_force = SWIOTLB_NO_FORCE;

	set_max_mapnr(pfn_to_page(max_pfn) - mem_map);

#ifndef CONFIG_SPARSEMEM_VMEMMAP
	free_unused_memmap();
#endif
	/* this will put all unused low memory onto the freelists */
	free_all_bootmem();

#ifdef CONFIG_PIN_MEMORY
	/* pre alloc the pages for pin memory */
	init_reserve_page_map((unsigned long)pin_memory_resource.start,
		(unsigned long)(pin_memory_resource.end - pin_memory_resource.start));
#endif

	kexec_reserve_crashkres_pages();

	mem_init_print_info(NULL);

	/*
	 * Check boundaries twice: Some fundamental inconsistencies can be
	 * detected at build time already.
	 */
#ifdef CONFIG_COMPAT
	BUILD_BUG_ON(TASK_SIZE_32			> TASK_SIZE_64);
#endif

#ifdef CONFIG_SPARSEMEM_VMEMMAP
	/*
	 * Make sure we chose the upper bound of sizeof(struct page)
	 * correctly when sizing the VMEMMAP array.
	 */
	BUILD_BUG_ON(sizeof(struct page) > (1 << STRUCT_PAGE_MAX_SHIFT));
#endif

	if (PAGE_SIZE >= 16384 && get_num_physpages() <= 128) {
		extern int sysctl_overcommit_memory;
		/*
		 * On a machine this small we won't get anywhere without
		 * overcommit, so turn it on by default.
		 */
		sysctl_overcommit_memory = OVERCOMMIT_ALWAYS;
	}
}

void free_initmem(void)
{
	free_reserved_area(lm_alias(__init_begin),
			   lm_alias(__init_end),
			   0, "unused kernel");
	/*
	 * Unmap the __init region but leave the VM area in place. This
	 * prevents the region from being reused for kernel modules, which
	 * is not supported by kallsyms.
	 */
	unmap_kernel_range((u64)__init_begin, (u64)(__init_end - __init_begin));
}

#ifdef CONFIG_BLK_DEV_INITRD

static int keep_initrd __initdata;

void __init free_initrd_mem(unsigned long start, unsigned long end)
{
	if (!keep_initrd) {
		free_reserved_area((void *)start, (void *)end, 0, "initrd");
		memblock_free(__virt_to_phys(start), end - start);
	}
}

static int __init keepinitrd_setup(char *__unused)
{
	keep_initrd = 1;
	return 1;
}

__setup("keepinitrd", keepinitrd_setup);
#endif

#ifdef CONFIG_ASCEND_FEATURES

#include <linux/perf/arm_pmu.h>
#ifdef CONFIG_CORELOCKUP_DETECTOR
#include <linux/nmi.h>
#endif

void ascend_enable_all_features(void)
{
#ifdef CONFIG_GPIO_DWAPB
	extern bool enable_ascend_gpio_dwapb;

	enable_ascend_gpio_dwapb = true;
#endif

	if (IS_ENABLED(CONFIG_ASCEND_DVPP_MMAP))
		enable_mmap_dvpp = 1;

	if (IS_ENABLED(CONFIG_ASCEND_IOPF_HIPRI))
		enable_iopf_hipri = 1;

	if (IS_ENABLED(CONFIG_ASCEND_CHARGE_MIGRATE_HUGEPAGES))
		enable_charge_mighp = 1;

	if (IS_ENABLED(CONFIG_SUSPEND))
		mem_sleep_current = PM_SUSPEND_ON;

	if (IS_ENABLED(CONFIG_PMU_WATCHDOG))
		pmu_nmi_enable = true;

	if (IS_ENABLED(CONFIG_MEMCG_KMEM)) {
		extern bool cgroup_memory_nokmem;
		cgroup_memory_nokmem = false;
	}

#ifdef CONFIG_ARM64_PSEUDO_NMI
	enable_pseudo_nmi = true;
#endif

#ifdef CONFIG_CORELOCKUP_DETECTOR
	enable_corelockup_detector = true;
#endif
}

static int __init ascend_enable_setup(char *__unused)
{
	ascend_enable_all_features();

	return 1;
}

early_param("ascend_enable_all", ascend_enable_setup);

static int __init ascend_mini_enable_setup(char *s)
{
#ifdef CONFIG_GPIO_DWAPB
	extern bool enable_ascend_mini_gpio_dwapb;

	enable_ascend_mini_gpio_dwapb = true;
#endif
	return 1;
}
__setup("ascend_mini_enable", ascend_mini_enable_setup);
#endif


/*
 * Dump out memory limit information on panic.
 */
static int dump_mem_limit(struct notifier_block *self, unsigned long v, void *p)
{
	if (memory_limit != PHYS_ADDR_MAX) {
		pr_emerg("Memory Limit: %llu MB\n", memory_limit >> 20);
	} else {
		pr_emerg("Memory Limit: none\n");
	}
	return 0;
}

static struct notifier_block mem_limit_notifier = {
	.notifier_call = dump_mem_limit,
};

static int __init register_mem_limit_dumper(void)
{
	atomic_notifier_chain_register(&panic_notifier_list,
				       &mem_limit_notifier);
	return 0;
}
__initcall(register_mem_limit_dumper);
