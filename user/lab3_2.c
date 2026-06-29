#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

// Функция для вывода информации о памяти процесса
void print_memory_info(char *stage) {
    printf("\n=== %s ===\n", stage);
    
    // Получаем текущую позицию program break
    char *current_brk = sbrk(0);
    printf("Current program break: %p\n", current_brk);
}

int main() {
    printf("\n========== sbrk(1) Test Program ==========\n");
    printf("PID of this process: %d\n", getpid());
    
    // 1. Информация ДО вызова sbrk
    print_memory_info("BEFORE sbrk(1)");
    
    // Вывод таблицы страниц ДО (если есть системный вызов)
    #ifdef HAS_PRINTPAGETABLE
    printf("\n--- Page table BEFORE sbrk(1) ---\n");
    printpagetable();
    #endif
    
    // 2. Запрос 1 байта
    printf("\n--- Calling sbrk(1) ---\n");
    char *new_brk = sbrk(1);
    if (new_brk == (char*)-1) {
        printf("sbrk(1) failed!\n");
        exit(1);
    }
    printf("sbrk(1) returned: %p\n", new_brk);
    printf("Expected new brk: %p\n", sbrk(0));
    
    // 3. Информация ПОСЛЕ вызова sbrk
    print_memory_info("AFTER sbrk(1)");
    
    // Вывод таблицы страниц ПОСЛЕ
    #ifdef HAS_PRINTPAGETABLE
    printf("\n--- Page table AFTER sbrk(1) ---\n");
    printpagetable();
    #endif
    
    // 4. Проверяем, можем ли мы записать данные по новому адресу
    printf("\n--- Testing write to new memory ---\n");
    *new_brk = 'A';
    printf("Wrote 'A' at address %p, value: %c\n", new_brk, *new_brk);
    
    // 5. Добавляем еще памяти для наглядности
    printf("\n--- Allocating more memory (4096 bytes) ---\n");
    char *more_mem = sbrk(4096);
    if (more_mem != (char*)-1) {
        printf("Allocated 4096 bytes at %p\n", more_mem);
        print_memory_info("AFTER sbrk(4096)");
        
        // Проверяем доступность новой памяти
        for (int i = 0; i < 10; i++) {
            more_mem[i] = 'A' + i;
        }
        printf("Wrote test values to new memory\n");
        
        #ifdef HAS_PRINTPAGETABLE
        printf("\n--- Page table AFTER sbrk(4096) ---\n");
        printpagetable();
        #endif
    }
    
    // 6. Анализ размера выделенной памяти
    printf("\n========== ANALYSIS ==========\n");
    printf("Key observations:\n");
    printf("- sbrk(1) increased program break by 1 byte\n");
    printf("- BUT physical memory is allocated in 4096-byte pages\n");
    printf("- The new page table entry (PTE) contains:\n");
    printf("  * Physical page number (PPN)\n");
    printf("  * Flags: PTE_V (valid), PTE_R (read), PTE_W (write), PTE_U (user)\n");
    printf("  * No PTE_X (execute) - data page\n");
    printf("- Each PTE maps 4096 bytes (one page)\n");
    
    printf("\n========== Test Complete ==========\n");
    
    exit(0);
}