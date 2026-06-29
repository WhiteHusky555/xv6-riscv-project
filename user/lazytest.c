#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main() {
    printf("--- Starting Lazy Allocation Test ---\n");
    
    // Смотрим текущий размер памяти
    uint64 old_sz = (uint64)sbrk(0);
    
    // Запрашиваем 10 страниц (40960 байт)
    int pages = 10;
    int size = pages * 4096;
    char *p = sbrk(size);
    
    if (p == (char*)-1) {
        printf("Error: sbrk failed!\n");
        exit(1);
    }
    
    uint64 new_sz = (uint64)sbrk(0);
    printf("sbrk(40960) success! Old sz: %d, New sz: %d\n", (int)old_sz, (int)new_sz);
    printf("Notice that sbrk returned instantly. No physical memory was actually allocated yet.\n\n");
    
    printf("Now writing to the allocated pages to trigger Page Faults...\n");
    
    // Касаемся каждой страницы, чтобы заставить ядро (trap.c) выделить физическую память
    for (int i = 0; i < pages; i++) {
        p[i * 4096] = 'A' + i; // Пишем один байт в начало каждой страницы
        printf("  Touched page %d at address %p\n", i, &p[i * 4096]);
    }
    
    printf("\nReading back the values to verify:\n");
    for (int i = 0; i < pages; i++) {
        if (p[i * 4096] != 'A' + i) {
            printf("Error: Data mismatch at page %d\n", i);
            exit(1);
        }
    }
    
    printf("--- Test Passed! System did not crash. ---\n");
    exit(0);
}