#include "kernel/types.h"
#include "user/user.h"

#define PGSIZE 4096

int pthread_create(void (*fcn)(void *), void *arg) {
  // Выделяем память. malloc возвращает базу (нижний адрес)
  void *stack = malloc(PGSIZE);
  if(stack == 0) return -1;

  // Передаем в clone именно базу. 
  // Мы договоримся, что clone сам прибавит PGSIZE для установки sp,
  // либо мы передадим (stack + PGSIZE), но тогда join должен вернуть 
  // нам именно этот ОРИГИНАЛЬНЫЙ адрес.
  
  uint64 stack_top = (uint64)stack + PGSIZE;
  // Важно: если ты делаешь выравнивание здесь, то join должен вернуть 
  // НЕ выровненный sp из trapframe, а оригинальную вершину.
  
  return clone((uint64)fcn, (uint64)arg, stack_top);
}

int pthread_join() {
  uint64 stack_ret_addr;
  int pid = join((uint64)&stack_ret_addr);
  
  if (pid > 0) {
    // В xv6 trapframe->sp будет указывать на вершину стека.
    // Если мы передавали stack + PGSIZE, то вычитание вернет базу.
    void *stack_base = (void*)(stack_ret_addr - PGSIZE);
    free(stack_base); 
  }
  return pid;
}