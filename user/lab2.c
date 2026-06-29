#include "kernel/types.h"
#include "user/user.h"

int main() {
    uint64 free_bytes = getfreemem();   // количество свободных байт

    // Вывод в байтах, килобайтах, мегабайтах (целые числа)
    printf("Free memory: %d bytes\n", (int)free_bytes);
    printf("Free memory: %d KB\n", (int)(free_bytes / 1024));
    printf("Free memory: %d MB\n", (int)(free_bytes / (1024 * 1024)));

    // Вывод в гигабайтах с дробной частью (3 знака после запятой)
    uint64 gb_int = free_bytes / (1024ULL * 1024 * 1024);
    uint64 gb_frac = (free_bytes % (1024ULL * 1024 * 1024)) * 1000 / (1024ULL * 1024 * 1024);

    printf("Free memory: %d.", (int)gb_int);
    // Дополняем дробную часть ведущими нулями до трёх цифр
    if (gb_frac < 10)
        printf("00%d", (int)gb_frac);
    else if (gb_frac < 100)
        printf("0%d", (int)gb_frac);
    else
        printf("%d", (int)gb_frac);
    printf(" GB\n");

    exit(0);
}