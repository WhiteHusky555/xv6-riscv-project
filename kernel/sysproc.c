#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

#include "kalloc.h"   // для объявления getfreemem()

uint64
sys_getfreemem(void)
{
    // Возвращает количество свободной памяти в байтах
    return getfreemem();
}

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;

  if(n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
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
  return kkill(pid);
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


// lab3_2
uint64
sys_printpagetable(void)
{
    struct proc *p = myproc();
    vmprint(p->pagetable);
    return 0;
}

// lab 6
uint64
sys_kht_put(void)
{
  int key, value;
  argint(0, &key);
  argint(1, &value);
  kht_put(key, value);
  return 0;
}

uint64
sys_kht_get(void)
{
  int key;
  argint(0, &key);
  return kht_get(key);
}

uint64
sys_kht_clear(void)
{
  kht_clear();
  return 0;
}

uint64 sys_clone(void) {
  uint64 fcn, arg, stack;
  // Получаем аргументы: указатель на функцию, аргумент, и адрес нового стека
  argaddr(0, &fcn);
  argaddr(1, &arg);
  argaddr(2, &stack);
  return clone(fcn, arg, stack);
}

uint64 sys_join(void) {
  uint64 stack_addr;
  
  // Достаем первый аргумент системного вызова (указатель, куда мы запишем адрес стека)
  argaddr(0, &stack_addr);
  
  // Вызываем настоящую ядерную функцию, которую мы написали в proc.c
  return join(stack_addr);
}