// kernel/exec.c

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

// Читает первые max байт из файла (inode должен быть уже заблокирован)
static int read_first_bytes(struct inode *ip, char *buf, int max) {
    int i;
    uint off = 0;
    
    for (i = 0; i < max; i++) {
        if (readi(ip, 0, (uint64)&buf[i], off, 1) != 1)
            break;
        off++;
    }
    return i;
}

// Извлекает путь интерпретатора и опциональный аргумент из shebang строки
// Формат: "#! /path/to/interpreter [optional-argument]"
// Возвращает:
//   0 - не shebang
//   1 - shebang без аргумента
//   2 - shebang с аргументом
static int parse_shebang(char *buf, int n, char *interp, int interp_size, 
                          char *interp_arg, int arg_size) {
    int i, j;
    
    // Проверяем первые два символа
    if (n < 2 || buf[0] != '#' || buf[1] != '!')
        return 0;
    
    // Пропускаем "#!" и возможные пробелы
    i = 2;
    while (i < n && (buf[i] == ' ' || buf[i] == '\t'))
        i++;
    
    // Извлекаем путь интерпретатора
    j = 0;
    while (i < n && j < interp_size - 1 && 
           buf[i] != '\n' && buf[i] != ' ' && buf[i] != '\t') {
        interp[j++] = buf[i++];
    }
    interp[j] = '\0';
    
    if (j == 0)
        return 0;
    
    // Проверяем, есть ли аргумент интерпретатора
    // Пропускаем пробелы после пути
    while (i < n && (buf[i] == ' ' || buf[i] == '\t'))
        i++;
    
    // Извлекаем аргумент (если есть и не является новой строкой)
    if (i < n && buf[i] != '\n' && buf[i] != '\r') {
        j = 0;
        while (i < n && j < arg_size - 1 && 
               buf[i] != '\n' && buf[i] != '\r' && buf[i] != ' ') {
            interp_arg[j++] = buf[i++];
        }
        interp_arg[j] = '\0';
        return 2;  // shebang с аргументом
    }
    
    interp_arg[0] = '\0';
    return 1;  // shebang без аргумента
}

static int loadseg(pagetable_t, uint64, struct inode *, uint, uint);

// map ELF permissions to PTE permission bits.
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

// Рекурсивная глубина для shebang
static int shebang_depth = 0;

//
// the implementation of the exec() system call
//
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();
  
  // ========== ПОДДЕРЖКА SHEBANG (#!) ==========
  char interp[MAXPATH];
  char interp_argument[MAXPATH];
  char first_bytes[256];
  int nread;
  int shebang_type;
  
  begin_op();
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  
  // Читаем первые байты (ip уже заблокирован)
  nread = read_first_bytes(ip, first_bytes, sizeof(first_bytes));
  
  // Проверяем shebang и извлекаем интерпретатор с аргументом
  shebang_type = parse_shebang(first_bytes, nread, interp, sizeof(interp),
                                interp_argument, sizeof(interp_argument));
  
  if(shebang_type > 0) {
    // Это скрипт - нужно запустить интерпретатор
    iunlockput(ip);
    end_op();
    
    // Защита от бесконечной рекурсии
    if(shebang_depth > 8) {
      printf("exec: shebang recursion too deep\n");
      return -1;
    }
    
    // Формируем новые аргументы в соответствии со стандартом Unix:
    // Порядок: [interp] [optional-arg] [script-path] [original-args...]
    char *new_argv[MAXARG];
    int new_argc = 0;
    
    // 0. Интерпретатор
    new_argv[new_argc++] = interp;
    
    // 1. Аргумент интерпретатора (если есть)
    if(shebang_type == 2 && interp_argument[0] != '\0') {
      new_argv[new_argc++] = interp_argument;
    }
    
    // 2. Путь к файлу скрипта
    new_argv[new_argc++] = path;
    
    // 3. Оригинальные аргументы пользователя (пропускаем argv[0], т.к. это имя скрипта)
    for(i = 1; argv[i] != 0; i++) {
      if(new_argc < MAXARG - 1) {
        new_argv[new_argc++] = argv[i];
      }
    }
    new_argv[new_argc] = 0;
    
    // Отладочный вывод всей сформированной строки
    printf("shebang running: ");
    for(i = 0; i < new_argc; i++)
      printf("%s ", new_argv[i]);
    printf("\n");
    
    shebang_depth++;
    int ret = kexec(interp, new_argv);
    shebang_depth--;
    return ret;
  }
  
  // ========== КОНЕЦ ПОДДЕРЖКИ SHEBANG ==========
  // Если это не shebang, продолжаем с обычным ELF файлом

  // Read the ELF header.
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
      goto bad;
    sz = sz1;
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate some pages at the next page boundary.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK+1)*PGSIZE, PTE_W)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz-(USERSTACK+1)*PGSIZE);
  sp = sz;
  stackbase = sp - USERSTACK*PGSIZE;

  // Copy argument strings into new stack
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16;
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  p->trapframe->a1 = sp;

  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;
  p->trapframe->sp = sp;

  // КРИТИЧЕСКИЙ МОМЕНТ (ИСПРАВЛЕНО)
  // Обновляем kpagetable ТОЛЬКО ЗДЕСЬ, когда мы уверены, что exec прошел успешно.
  // Очищаем старые маппинги, чтобы новый процесс не имел доступа к памяти старого
  if(oldsz > 0) {
    uvmunmap(p->kpagetable, 0, PGROUNDUP(oldsz)/PGSIZE, 0);
  }
  u2kvmcopy(p->pagetable, p->kpagetable, 0, p->sz);

  proc_freepagetable(oldpagetable, oldsz);

  return argc;

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}