#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)
/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio network interface
  kvmmap(kpgtbl, VIRTIO1, VIRTIO1, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;   
    if((*pte & PTE_V) == 0)  // has physical page been allocated?
      continue;
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;   // page table entry hasn't been allocated
    if((*pte & PTE_V) == 0)
      continue;   // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
      
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Обновлено
// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
  struct proc *p = myproc();
  // Проверка границ: нельзя читать за пределами памяти процесса
  if (srcva >= p->sz || srcva + len > p->sz || srcva + len < srcva) return -1;
  
  // ДОБАВЛЕНО ДЛЯ ЛЕНИВОЙ ВЫДАЧИ ПАМЯТИ: 
  // Принудительно вызываем выделение страницы перед использованием memmove,
  // если страница еще не была физически выделена.
  for(uint64 a = PGROUNDDOWN(srcva); a < srcva + len; a += PGSIZE) {
    if(!ismapped(p->pagetable, a)) {
       if(vmfault(p->pagetable, a, 0) == 0) return -1;
    }
  }
  
  memmove(dst, (void*)srcva, len);
  return 0;
}

// Обновлено
// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
  struct proc *p = myproc();
  if (srcva >= p->sz) return -1;

  // Ограничиваем чтение размером памяти процесса
  uint64 n = p->sz - srcva;
  if(n > max) n = max;

  // ДОБАВЛЕНО ДЛЯ ЛЕНИВОЙ ВЫДАЧИ ПАМЯТИ: 
  // Принудительно выделяем страницу перед чтением строки.
  for(uint64 a = PGROUNDDOWN(srcva); a < srcva + n; a += PGSIZE) {
    if(!ismapped(p->pagetable, a)) {
       if(vmfault(p->pagetable, a, 0) == 0) return -1;
    }
  }

  // Используем системную функцию для строк
  char *s = (char *)srcva;
  for(uint64 i = 0; i < n; i++){
    dst[i] = s[i];
    if(s[i] == '\0') return 0;
  }
  return -1;
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  
  // КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: синхронизируем новую страницу с ядерной таблицей
  // Без этого copyin/copyout (memmove) выдадут Load Page Fault
  u2kvmcopy(p->pagetable, p->kpagetable, va, va + PGSIZE);
  
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}


// Рекурсивный вывод таблицы страниц
void vmprintlevel(pagetable_t pagetable, int level) {
    // level: 1 - корневой уровень, 2 - средний, 3 - листовой
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        if (pte & PTE_V) {
            // Выводим отступы
            for (int j = 0; j < level; j++) {
                printf("..");
            }
            // Используем %lx для 64-битных целых чисел (НЕ %p)
            printf("%d: pte %lx pa %lx\n", i, pte, PTE2PA(pte));
            
            // Если это не листовой уровень (нет флагов R/W/X)
            if ((pte & (PTE_R|PTE_W|PTE_X)) == 0) {
                pagetable_t next = (pagetable_t)PTE2PA(pte);
                vmprintlevel(next, level + 1);
            }
        }
    }
}

void vmprint(pagetable_t pagetable) {
    printf("page table %p\n", pagetable);
    vmprintlevel(pagetable, 1);
}

// Создает копию таблицы страниц ядра для процесса
pagetable_t
proc_kpagetable(struct proc *p)
{
  pagetable_t kpt = uvmcreate();
  if (kpt == 0) return 0;

  // 1. Стандартные маппинги ядра из kvmmake
  if(mappages(kpt, UART0, PGSIZE, UART0, PTE_R | PTE_W) != 0) goto err;
  if(mappages(kpt, VIRTIO0, PGSIZE, VIRTIO0, PTE_R | PTE_W) != 0) goto err;
  if(mappages(kpt, VIRTIO1, PGSIZE, VIRTIO1, PTE_R | PTE_W) != 0) goto err;
  if(mappages(kpt, PLIC, 0x4000000, PLIC, PTE_R | PTE_W) != 0) goto err;
  if(mappages(kpt, KERNBASE, (uint64)etext-KERNBASE, KERNBASE, PTE_R | PTE_X) != 0) goto err;
  if(mappages(kpt, (uint64)etext, PHYSTOP-(uint64)etext, (uint64)etext, PTE_R | PTE_W) != 0) goto err;
  if(mappages(kpt, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X) != 0) goto err;

  // 2. Маппинг CLINT (если определен в memlayout.h)
  #ifdef CLINT
  if(mappages(kpt, CLINT, 0x10000, CLINT, PTE_R | PTE_W) != 0) goto err;
  #endif

  // 3. Исправленный маппинг стеков всех ядер (KSTACK)
  // Мы ищем физический адрес каждого стека в глобальной kernel_pagetable
  for(int i = 0; i < NPROC; i++){
    uint64 va = KSTACK(i);
    pte_t *pte = walk(kernel_pagetable, va, 0); 
    if(pte == 0 || (*pte & PTE_V) == 0) continue;
    uint64 pa = PTE2PA(*pte);
    if(mappages(kpt, va, PGSIZE, pa, PTE_R | PTE_W) != 0) goto err;
  }

  return kpt;

err:
  if (kpt) uvmfree(kpt, 0); // Нужна функция очистки kpagetable без удаления физ. страниц
  return 0;
}

// Копирует маппинги пользователя в таблицу ядра процесса
void
u2kvmcopy(pagetable_t upgt, pagetable_t kpgt, uint64 oldsz, uint64 newsz) {
  uint64 a;
  pte_t *upte, *kpte;

  // Безопасность: не заходим на территорию PLIC
  if (newsz > PLIC) newsz = PLIC;
  if (newsz <= oldsz) return;

  for(a = PGROUNDUP(oldsz); a < newsz; a += PGSIZE){
    upte = walk(upgt, a, 0);
    if(upte == 0 || (*upte & PTE_V) == 0) continue;
    
    if((kpte = walk(kpgt, a, 1)) == 0) panic("u2kvmcopy: walk failed");
    
    uint64 pa = PTE2PA(*upte);
    // Сбрасываем PTE_U, чтобы ядро RISC-V могло получить доступ
    uint flags = (PTE_FLAGS(*upte) & ~PTE_U); 
    *kpte = PA2PTE(pa) | flags | PTE_V;
  }
}

// Освобождает директории таблицы страниц ядра процесса.
// НЕ освобождает сами физические страницы (т.к. они принадлежат ядру).
void
free_kpagetable(pagetable_t kpt)
{
  // 512 записей в таблице страниц RISC-V
  for(int i = 0; i < 512; i++){
    pte_t pte = kpt[i];
    if(pte & PTE_V){
      // Если это не листовая запись (указывает на следующий уровень)
      if((pte & (PTE_R|PTE_W|PTE_X)) == 0){
        uint64 child = PTE2PA(pte);
        free_kpagetable((pagetable_t)child);
      }
      kpt[i] = 0;
    }
  }
  kfree((void*)kpt);
}