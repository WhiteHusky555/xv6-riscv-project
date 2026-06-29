//
// Console input and output, to the uart.
// Reads are line at a time.
// Implements special input characters:
//   newline -- end of line
//   control-h -- backspace
//   control-u -- kill line
//   control-d -- end of file
//   control-p -- print process list
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define BACKSPACE 0x100  // erase the last output character
#define C(x)  ((x)-'@')  // Control-x

//
// send one character to the uart, but don't use
// interrupts or sleep(). safe to be called from
// interrupts, e.g. by printf and to echo input
// characters.
//
void
consputc(int c)
{
  if(c == BACKSPACE){
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

struct {
  struct spinlock lock;
  
  // input circular buffer
#define INPUT_BUF_SIZE 128
  char buf[INPUT_BUF_SIZE];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} cons;

//
// user write() system calls to the console go here.
// uses sleep() and UART interrupts.
//
int
consolewrite(int user_src, uint64 src, int n)
{
  char buf[32]; // move batches from user space to uart.
  int i = 0;

  while(i < n){
    int nn = sizeof(buf);
    if(nn > n - i)
      nn = n - i;
    if(either_copyin(buf, user_src, src+i, nn) == -1)
      break;
    uartwrite(buf, nn);
    i += nn;
  }

  return i;
}


// lab 5
// ИЗМЕНЕНО: Вместо sleep мы выполняем Polling (Опрос)
// ИЗМЕНЕНО: Вместо while(1) мы выполняем Polling (Опрос)
// ИЗМЕНЕНО: Вместо while(tx_busy != 0) мы выполняем Polling (Опрос)
// ИЗМЕНЕНО: Вместо while(ReadReg(LSR) & LSR_TX_ID
int
consoleread(int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char cbuf;

  target = n;
  acquire(&cons.lock);
  while(n > 0){
    // ИЗМЕНЕНО: Вместо sleep мы выполняем Polling (Опрос)
    while(cons.r == cons.w){
      if(killed(myproc())){
        release(&cons.lock);
        return -1;
      }
      
      // Спрашиваем UART напрямую: "Есть символ?"
      int polled_c = uartgetc();
      if (polled_c != -1) {
        // Символ есть! Но consoleintr() сама захватывает cons.lock.
        // Чтобы не получить deadlock (взаимную блокировку), мы должны 
        // отпустить блокировку, вызвать обработчик, а затем взять её снова.
        release(&cons.lock);
        consoleintr(polled_c);
        acquire(&cons.lock);
      } else {
        // Данных нет. 
        // Чтобы не зависнуть в бесконечном цикле (заморозив всё ядро),
        // мы отдаем остаток кванта времени процессора другим процессам.
        release(&cons.lock);
        // ВМЕСТО YIELD():
        // Усыпляем процессор до ближайшего тика таймера (или другого прерывания)
        asm volatile("wfi");
        acquire(&cons.lock);
      }
    }

    c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

    if(c == C('D')){  // end-of-file
      if(n < target){
        cons.r--;
      }
      break;
    }

    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    if(c == '\n'){
      break;
    }
  }
  release(&cons.lock);

  return target - n;
}

//
// the console input interrupt handler.
// uartintr() calls this for each input character.
// do erase/kill processing, append to cons.buf,
// wake up consoleread() if a whole line has arrived.
//
void
consoleintr(int c)
{
  acquire(&cons.lock);

  switch(c){
  case C('P'):  // Print process list.
    procdump();
    break;
  case C('U'):  // Kill line.
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF_SIZE] != '\n'){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  case C('H'): // Backspace
  case '\x7f': // Delete key
    if(cons.e != cons.w){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  default:
    if(c != 0 && cons.e-cons.r < INPUT_BUF_SIZE){
      c = (c == '\r') ? '\n' : c;

      // echo back to the user.
      consputc(c);

      // store for consumption by consoleread().
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

      if(c == '\n' || c == C('D') || cons.e-cons.r == INPUT_BUF_SIZE){
        // wake up consoleread() if a whole line (or end-of-file)
        // has arrived.
        cons.w = cons.e;
        wakeup(&cons.r);
      }
    }
    break;
  }
  
  release(&cons.lock);
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");

  uartinit();

  // connect read and write system calls
  // to consoleread and consolewrite.
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
}
