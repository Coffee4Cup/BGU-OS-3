#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if (growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n)
  {
    if (killed(myproc()))
    {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

/*
sys_flip_display: zero-copy page flip.

Syscall argument 0: user virtual address of a page-aligned buffer
that is exactly GPU_FB_PAGES (300) * PGSIZE bytes (i.e. 640x480x4 =
1,228,800 bytes).  The buffer must already be fully mapped in the
calling process's address space.

TODO: Students implement this syscall.
*/
uint64
sys_flip_display(void)
{
  uint64 buf;
  struct proc *p = myproc();

  // Parse the *buf argument and validate it:
  argaddr(0, &buf);

  if (buf % PGSIZE != 0 ||                        // Must be page-aligned
      buf >= MAXVA || buf + 300 * PGSIZE > MAXVA) // Must be within valid userspace bounds
    return -1;

  // Find the physical addresses of the 300 pages starting at buf. Validate that all pages are mapped and user-accessible.
  uint64 physical_addrs[300];
  for (int i = 0; i < 300; i++)
  {
    uint64 va = buf + i * PGSIZE;

    // Retrieve the physical address corresponding to this virtual address from the process's pagetable
    uint64 pa = walkaddr(p->pagetable, va);

    if (pa == 0) // Check if page is unmapped or lacks user permissions
      return -1;

    physical_addrs[i] = pa;
  }

  // Tell the GPU to flip to these physical pages
  virtio_gpu_flip(physical_addrs);

  // Mark the process as having flipped the display for future reference.
  p->flipped_gpu = 1;

  return 0;
}

/*
sys_map_display: map the GPU's kernel framebuffer pages (fb[]) directly
into the calling process's address space with PTE_U|PTE_R|PTE_W.

Syscall argument 0: desired user virtual address (must be page-aligned).
  Pass 0 to let the kernel auto-select the next available VA above p->sz.

Returns the mapped virtual address on success, (uint64)-1 on failure.
*/
uint64
sys_map_display(void)
{
  uint64 addr;
  struct proc *p = myproc();

  // Fetch the user argument
  argaddr(0, &addr);
  if (addr < 0)
    return -1;

  uint64 size = 300 * PGSIZE; // 300 * 4096 bytes

  if (addr == 0)
  {
    // The framebuffer will be placed below the kernel's trampoline and trapframe pages,
    // which are at the top of the user address space, ensuring it doesn't collide with normal heap growth.
    addr = PGROUNDDOWN(MAXVA - 2 * PGSIZE - size);
    if (addr < p->sz)
      return -1;
  }

  // If the user provided an address, validate it:
  else
  {
    if ((addr % PGSIZE) != 0) // Must be page-aligned
      return -1;

    if (addr >= MAXVA || addr + size > MAXVA - 2 * PGSIZE) // Must be within valid userspace bounds
      return -1;

    // Check for collisions with existing mappings
    for (uint64 va = addr; va < addr + size; va += PGSIZE)
    {
      // Check if the virtual address is already mapped in the process's pagetable
      pte_t *pte = walk(p->pagetable, va, 0);

      if (pte && (*pte & PTE_V)) // If there's a valid mapping at this VA, it's a collision
        return -1;
    }
  }

  // Map the framebuffer into the user process's pagetable at the specified address.
  if (map_framebuffer_to_user_pagetable(p->pagetable, addr) < 0)
    return -1;

  // On success, store the mapped framebuffer VA in the proc struct for future use.
  p->fbva = addr;

  return addr;
}
