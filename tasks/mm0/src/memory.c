/*
 * Initialise the memory structures.
 *
 * Copyright (C) 2007, 2008 Bahadir Balban
 */
#include <init.h>
#include <memory.h>
#include <l4/macros.h>
#include <l4/config.h>
#include <l4/types.h>
#include <l4/api/errno.h>
#include <l4/generic/space.h>
#include <l4lib/arch/syslib.h>
#include INC_GLUE(memory.h)
#include INC_SUBARCH(mm.h)
#include <memory.h>
#include <file.h>
#include <user.h>


struct address_pool pager_vaddr_pool;

/* FIXME:
 * ID pool id allocation size (i.e. bitlimit/nwords parameters)
 * must be in sync with address pool allocation range. Here, since
 * the id pool needs to be determined at compile time, the two
 * parameters don't match yet.
 */

/* Bitmap size to represent an address pool of 256 MB. */
#define ADDRESS_POOL_256MB		2048

/* Same as a regular id pool except that its bitmap size is fixed */
static struct pager_virtual_address_id_pool {
	int nwords;
	int bitlimit;
	u32 bitmap[ADDRESS_POOL_256MB];
} pager_virtual_address_id_pool = {
	.nwords = ADDRESS_POOL_256MB,
	.bitlimit = ADDRESS_POOL_256MB * 32,
};

/* End of pager image */
extern unsigned char _end[];

int pager_address_pool_init(void)
{
	/*
	 * Initialise id pool for pager virtual address
	 * allocation. This spans from end of pager image
	 * until the end of the virtual address range
	 * allocated for the pager
	 */
	address_pool_init_with_idpool(&pager_vaddr_pool,
			  	      (struct id_pool *)
				      &pager_virtual_address_id_pool,
				      page_align_up((unsigned long)_end + PAGE_SIZE),
				      (unsigned long)0xF0000000);
	return 0;
}

void *l4_new_virtual(int npages)
{
	return pager_new_address(npages);
}

void *l4_del_virtual(void *virt, int npages)
{
	pager_delete_address(virt, npages);
	return 0;
}

/* Maps a page from a vm_file to the pager's address space */
void *pager_map_page(struct vm_file *f, unsigned long page_offset)
{
	int err;
	struct page *p;

	if ((err = read_file_pages(f, page_offset, page_offset + 1)) < 0)
		return PTR_ERR(err);

	if ((p = find_page(&f->vm_obj, page_offset)))
		return (void *)l4_map_helper((void *)page_to_phys(p), 1);
	else
		return 0;
}

/* Unmaps a page's virtual address from the pager's address space */
void pager_unmap_page(void *addr)
{
	l4_unmap_helper(addr, 1);
}

void *pager_new_address(int npages)
{
	return address_new(&pager_vaddr_pool, npages);
}

int pager_delete_address(void *virt_addr, int npages)
{
	return address_del(&pager_vaddr_pool, virt_addr, npages);
}

/* Maps a page from a vm_file to the pager's address space */
void *pager_map_pages(struct vm_file *f, unsigned long page_offset, unsigned long npages)
{
	int err;
	struct page *p;
	void *addr_start, *addr;

	/* Get the pages */
	if ((err = read_file_pages(f, page_offset, page_offset + npages)) < 0)
		return PTR_ERR(err);

	/* Get the address range */
	if (!(addr_start = pager_new_address(npages)))
		return PTR_ERR(-ENOMEM);
	addr = addr_start;

	/* Map pages contiguously one by one */
	for (unsigned long pfn = page_offset; pfn < page_offset + npages; pfn++) {
		BUG_ON(!(p = find_page(&f->vm_obj, pfn)))
			l4_map((void *)page_to_phys(p), addr, 1, MAP_USR_RW_FLAGS, self_tid());
			addr += PAGE_SIZE;
	}

	return addr_start;
}

/* Unmaps a page's virtual address from the pager's address space */
void pager_unmap_pages(void *addr, unsigned long npages)
{
	/* Align to page if unaligned */
	if (!is_page_aligned(addr))
		addr = (void *)page_align(addr);

	/* Unmap so many pages */
	l4_unmap_helper(addr, npages);
}

/*
 * Maps multiple pages on a contiguous virtual address range,
 * returns pointer to byte offset in the file.
 */
void *pager_map_file_range(struct vm_file *f, unsigned long byte_offset,
			   unsigned long size)
{
	unsigned long mapsize = (byte_offset & PAGE_MASK) + size;

	void *page = pager_map_pages(f, __pfn(byte_offset), __pfn(page_align_up(mapsize)));

	return (void *)((unsigned long)page | (PAGE_MASK & byte_offset));
}

void *pager_validate_map_user_range2(struct tcb *user, void *userptr,
				    unsigned long size, unsigned int vm_flags)
{
	unsigned long start = page_align(userptr);
	unsigned long end = page_align_up(userptr + size);
	unsigned long npages = __pfn(end - start);
	void *virt, *virt_start;
	void *mapped = 0;

	/* Validate that user task owns this address range */
	if (pager_validate_user_range(user, userptr, size, vm_flags) < 0)
		return 0;

	/* Get the address range */
	if (!(virt_start = pager_new_address(npages)))
		return PTR_ERR(-ENOMEM);
	virt = virt_start;

	/* Map every page contiguously in the allocated virtual address range */
	for (unsigned long addr = start; addr < end; addr += PAGE_SIZE) {
		struct page *p = task_virt_to_page(user, addr);

		if (IS_ERR(p)) {
			/* Unmap pages mapped so far */
			l4_unmap_helper(virt_start, __pfn(addr - start));

			/* Delete virtual address range */
			pager_delete_address(virt_start, npages);

			return p;
		}

		l4_map((void *)page_to_phys(task_virt_to_page(user, addr)),
		       virt, 1, MAP_USR_RW_FLAGS, self_tid());
		virt += PAGE_SIZE;
	}

	/* Set the mapped pointer to offset of user pointer given */
	mapped = virt_start;
	mapped = (void *)(((unsigned long)mapped) |
			  ((unsigned long)(PAGE_MASK &
				  	   (unsigned long)userptr)));

	/* Return the mapped pointer */
	return mapped;
}


