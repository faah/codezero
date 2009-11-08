/*
 * Main initialisation code for the ARM kernel
 *
 * Copyright (C) 2007 Bahadir Balban
 */
#include <l4/lib/mutex.h>
#include <l4/lib/printk.h>
#include <l4/lib/string.h>
#include <l4/lib/idpool.h>
#include <l4/generic/platform.h>
#include <l4/generic/scheduler.h>
#include <l4/generic/space.h>
#include <l4/generic/tcb.h>
#include <l4/generic/bootmem.h>
#include <l4/generic/resource.h>
#include <l4/generic/container.h>
#include INC_ARCH(linker.h)
#include INC_ARCH(asm.h)
#include INC_SUBARCH(mm.h)
#include INC_SUBARCH(mmu_ops.h)
#include INC_GLUE(memlayout.h)
#include INC_GLUE(memory.h)
#include INC_GLUE(message.h)
#include INC_GLUE(syscall.h)
#include INC_GLUE(init.h)
#include INC_PLAT(platform.h)
#include INC_PLAT(printascii.h)
#include INC_API(syscall.h)
#include INC_API(kip.h)
#include INC_API(mutex.h)

unsigned int kernel_mapping_end;

/* Maps the early memory regions needed to bootstrap the system */
void init_kernel_mappings(void)
{
	memset(&init_pgd, 0, sizeof(pgd_table_t));

	/* Map kernel area to its virtual region */
	add_section_mapping_init(align(virt_to_phys(_start_text),SZ_1MB),
				 align((unsigned int)_start_text, SZ_1MB), 1,
				 cacheable | bufferable);

	/* Map kernel one-to-one to its physical region */
	add_section_mapping_init(align(virt_to_phys(_start_text),SZ_1MB),
				 align(virt_to_phys(_start_text),SZ_1MB),
				 1, 0);
}

void print_sections(void)
{
	dprintk("_start_kernel: ",(unsigned int)_start_kernel);
	dprintk("_start_text: ",(unsigned int)_start_text);
	dprintk("_end_text: ", 	(unsigned int)_end_text);
	dprintk("_start_data: ", (unsigned int)_start_data);
	dprintk("_end_data: ", 	(unsigned int)_end_data);
	dprintk("_start_vectors: ",(unsigned int)_start_vectors);
	dprintk("arm_high_vector: ",(unsigned int)arm_high_vector);
	dprintk("_end_vectors: ",(unsigned int)_end_vectors);
	dprintk("_start_kip: ", (unsigned int) _start_kip);
	dprintk("_end_kip: ", (unsigned int) _end_kip);
	dprintk("_bootstack: ",	(unsigned int)_bootstack);
	dprintk("_end_kernel: ", (unsigned int)_end_kernel);
	dprintk("_start_init: ", (unsigned int)_start_init);
	dprintk("_end_init: ", (unsigned int)_end_init);
	dprintk("_end: ", (unsigned int)_end);
}

/*
 * Enable virtual memory using kernel's pgd
 * and continue execution on virtual addresses.
 */
void start_vm()
{
	/*
	 * TTB must be 16K aligned. This is because first level tables are
	 * sized 16K.
	 */
	if ((unsigned int)&init_pgd & 0x3FFF)
		dprintk("kspace not properly aligned for ttb:",
			(u32)&init_pgd);
	// memset((void *)&kspace, 0, sizeof(pgd_table_t));
	arm_set_ttb(virt_to_phys(&init_pgd));

	/*
	 * This sets all 16 domains to zero and  domain 0 to 1. The outcome
	 * is that page table access permissions are in effect for domain 0.
	 * All other domains have no access whatsoever.
	 */
	arm_set_domain(1);

	/* Enable everything before mmu permissions are in place */
	arm_enable_caches();
	arm_enable_wbuffer();

	/*
	 * Leave the past behind. Tlbs are invalidated, write buffer is drained.
	 * The whole of I + D caches are invalidated unconditionally. This is
	 * important to ensure that the cache is free of previously loaded
	 * values. Otherwise unpredictable data aborts may occur at arbitrary
	 * times, each time a load/store operation hits one of the invalid
	 * entries and those entries are cleaned to main memory.
	 */
	arm_invalidate_cache();
	arm_drain_writebuffer();
	arm_invalidate_tlb();
	arm_enable_mmu();

	/* Jump to virtual memory addresses */
	__asm__ __volatile__ (
		"add	sp, sp, %0	\n"	/* Update stack pointer */
		"add	fp, fp, %0	\n"	/* Update frame pointer */
		/* On the next instruction below, r0 gets
		 * current PC + KOFFSET + 2 instructions after itself. */
		"add	r0, pc, %0	\n"
		/* Special symbol that is extracted and included in the loader.
		 * Debuggers can break on it to load the virtual symbol table */
		".global break_virtual;\n"
		"break_virtual:\n"
		"mov	pc, r0		\n" /* (r0 has next instruction) */
		:
		: "r" (KERNEL_OFFSET)
		: "r0"
	);

	/* At this point, execution is on virtual addresses. */
	remove_section_mapping(virt_to_phys(_start_kernel));

	/*
	 * Restore link register (LR) for this function.
	 *
	 * NOTE: LR values are pushed onto the stack at each function call,
	 * which means the restored return values will be physical for all
	 * functions in the call stack except this function. So the caller
	 * of this function must never return but initiate scheduling etc.
	 */
	__asm__ __volatile__ (
		"add	%0, %0, %1	\n"
		"mov	pc, %0		\n"
		:: "r" (__builtin_return_address(0)), "r" (KERNEL_OFFSET)
	);
	while(1);
}

/* This calculates what address the kip field would have in userspace. */
#define KIP_USR_OFFSETOF(kip, field) ((void *)(((unsigned long)&kip.field - \
					(unsigned long)&kip) + USER_KIP_PAGE))

/* The kip is non-standard, using 0xBB to indicate mine for now ;-) */
void kip_init()
{
	struct utcb **utcb_ref;

	/*
	 * TODO: Adding utcb size might be useful
	 */
	memset(&kip, 0, PAGE_SIZE);
	memcpy(&kip, "L4\230K", 4); /* Name field = l4uK */
	kip.api_version 	= 0xBB;
	kip.api_subversion 	= 1;
	kip.api_flags 		= 0; 		/* LE, 32-bit architecture */
	kip.kdesc.magic		= 0xBBB;
	kip.kdesc.version	= CODEZERO_VERSION;
	kip.kdesc.subversion	= CODEZERO_SUBVERSION;
	strncpy(kip.kdesc.date, __DATE__, KDESC_DATE_SIZE);
	strncpy(kip.kdesc.time, __TIME__, KDESC_TIME_SIZE);

	kip_init_syscalls();

	/* KIP + 0xFF0 is pointer to UTCB segment start address */
	utcb_ref = (struct utcb **)((unsigned long)&kip + UTCB_KIP_OFFSET);

	add_boot_mapping(virt_to_phys(&kip), USER_KIP_PAGE, PAGE_SIZE,
			 MAP_USR_RO_FLAGS);
	printk("%s: Kernel built on %s, %s\n", __KERNELNAME__,
	       kip.kdesc.date, kip.kdesc.time);
}


void vectors_init()
{
	unsigned int size = ((u32)_end_vectors - (u32)arm_high_vector);

	/* Map the vectors in high vector page */
	add_boot_mapping(virt_to_phys(arm_high_vector),
			 ARM_HIGH_VECTOR, size, 0);
	arm_enable_high_vectors();

	/* Kernel memory trapping is enabled at this point. */
}

void abort()
{
	printk("Aborting on purpose to halt system.\n");
#if 0
	/* Prefetch abort */
	__asm__ __volatile__ (
		"mov	pc, #0x0\n"
		::
	);
#endif
	/* Data abort */
	__asm__ __volatile__ (
		"mov	r0, #0		\n"
		"ldr	r0, [r0]	\n"
		::
	);
}

void jump(struct ktcb *task)
{
	__asm__ __volatile__ (
		"mov	lr,	%0\n"	/* Load pointer to context area */
		"ldr	r0,	[lr]\n"	/* Load spsr value to r0 */
		"msr	spsr,	r0\n"	/* Set SPSR as ARM_MODE_USR */
		"add	sp, lr, %1\n"	/* Reset SVC stack */
		"sub	sp, sp, %2\n"	/* Align to stack alignment */
		"ldmib	lr, {r0-r14}^\n" /* Load all USR registers */

		"nop		\n"	/* Spec says dont touch banked registers
					 * right after LDM {no-pc}^ for one instruction */
		"add	lr, lr, #64\n"	/* Manually move to PC location. */
		"ldr	lr,	[lr]\n"	/* Load the PC_USR to LR */
		"movs	pc,	lr\n"	/* Jump to userspace, also switching SPSR/CPSR */
		:
		: "r" (task), "r" (PAGE_SIZE), "r" (STACK_ALIGNMENT)
	);
}

void switch_to_user(struct ktcb *task)
{
	arm_clean_invalidate_cache();
	arm_invalidate_tlb();
	arm_set_ttb(virt_to_phys(TASK_PGD(task)));
	arm_invalidate_tlb();
	jump(task);
}

void setup_dummy_current()
{
	/*
	 * Temporarily iInitialize the beginning of
	 * last page of stack as the current ktcb
	 */
	memset(current, 0, sizeof(struct ktcb));

	current->space = &init_space;
	TASK_PGD(current) = &init_pgd;
}

void init_finalize(struct kernel_resources *kres)
{
	volatile register unsigned int stack;
	volatile register unsigned int newstack;
	struct ktcb *first_task;
	struct container *c;

	/* Get the first container */
	c = link_to_struct(kres->containers.list.next,
			   struct container, list);

	/* Get the first pager in container */
	first_task = link_to_struct(c->ktcb_list.list.next,
				    struct ktcb, task_list);

	/* Calculate first stack address */
	newstack = align((unsigned long)first_task + PAGE_SIZE - 1,
			 STACK_ALIGNMENT);

	/* Switch to new stack */
	stack = newstack;
	asm("mov sp, %0\n\t"::"r"(stack));

	/* -- Point of no stack unwinding -- */

	/*
	 * Unmap boot memory, and add it as
	 * an unused kernel memcap
	 */
	free_boot_memory(&kernel_resources);

	/*
	 * Set up initial KIP UTCB ref
	 */
	kip.utcb = (u32)current->utcb_address;

	/*
	 * Start the scheduler, jumping to task
	 */
	scheduler_start();
}

void start_kernel(void)
{
	printascii("\n"__KERNELNAME__": start kernel...\n");

	/*
	 * Initialise section mappings
	 * for the kernel area
	 */
	init_kernel_mappings();

	/*
	 * Enable virtual memory
	 * and jump to virtual addresses
	 */
	start_vm();

	/*
	 * Set up a dummy current ktcb on
	 * boot stack with initial pgd
	 */
	setup_dummy_current();

	/*
	 * Initialise platform-specific
	 * page mappings, and peripherals
	 */
	platform_init();

	/* Can only print when uart is mapped */
	printk("%s: Virtual memory enabled.\n",
	       __KERNELNAME__);

	/*
	 * Map and enable high vector page.
	 * Faults can be handled after here.
	 */
	vectors_init();

	/* Remap 1MB kernel sections as 4Kb pages. */
	remap_as_pages((void *)page_align(_start_kernel),
		       (void *)page_align_up(_end_kernel));

	/*
	 * Initialise kip and map
	 * for userspace access
	 */
	kip_init();

	/* Initialise system call page */
	syscall_init();

	/* Init scheduler */
	sched_init(&scheduler);

	/*
	 * Evaluate system resources
	 * and set up resource pools
	 */
	init_system_resources(&kernel_resources);

	/*
	 * Free boot memory, switch to first
	 * task's stack and start scheduler
	 */
	init_finalize(&kernel_resources);

	BUG();
}

