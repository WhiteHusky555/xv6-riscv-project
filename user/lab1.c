#include "kernel/types.h"
#include "user/user.h"

#define ROUND_TRIPS 10000   // Количество циклов обмена

int main() {
    int parent_to_child[2];  // Канал: родитель -> ребёнок
    int child_to_parent[2];  // Канал: ребёнок -> родитель
    int pid;
    char buf = 'x';
    uint64 start, end;
    int status;

    // Создаём два канала
    if (pipe(parent_to_child) < 0 || pipe(child_to_parent) < 0) {
        printf("pipe failed\n");
        exit(1);
    }

    pid = fork();
    if (pid < 0) {
        printf("fork failed\n");
        exit(1);
    }

    if (pid == 0) {  // Дочерний процесс
        // Закрываем ненужные концы каналов
        close(parent_to_child[1]);
        close(child_to_parent[0]);

        // Цикл приёма и отправки
        for (int i = 0; i < ROUND_TRIPS; i++) {
            if (read(parent_to_child[0], &buf, 1) != 1) {
                printf("child read failed\n");
                exit(1);
            }
            if (write(child_to_parent[1], &buf, 1) != 1) {
                printf("child write failed\n");
                exit(1);
            }
        }

        close(parent_to_child[0]);
        close(child_to_parent[1]);
        exit(0);

    } else {  // Родительский процесс
        close(parent_to_child[0]);
        close(child_to_parent[1]);

        start = uptime();

        for (int i = 0; i < ROUND_TRIPS; i++) {
            if (write(parent_to_child[1], &buf, 1) != 1) {
                printf("parent write failed\n");
                exit(1);
            }
            if (read(child_to_parent[0], &buf, 1) != 1) {
                printf("parent read failed\n");
                exit(1);
            }
        }

        end = uptime();

        close(parent_to_child[1]);
        close(child_to_parent[0]);

        wait(&status);

        // Вычисляем время в тиках (1 тик = 10 мс)
        uint64 elapsed_ticks = end - start;
        
        // В xv6 printf не поддерживает %f и %ld, поэтому выводим всё как целые числа
        printf("Completed %d round trips\n", ROUND_TRIPS);
        printf("Time: %d ticks\n", (int)elapsed_ticks);
        printf("One tick = 10 ms\n");
        
        // Выводим время в миллисекундах
        int elapsed_ms = (int)elapsed_ticks * 10;
        printf("Total time: %d ms\n", elapsed_ms);
        
        // Вычисляем количество операций в секунду (целое число)
        if (elapsed_ms > 0) {
            int ops_per_sec = (ROUND_TRIPS * 1000) / elapsed_ms;
            printf("Rate: %d round trips per second\n", ops_per_sec);
            
            // Среднее время одного round-trip в микросекундах
            int avg_us = (elapsed_ms * 1000) / ROUND_TRIPS;
            printf("Average round trip time: %d microseconds\n", avg_us);
        }
    }

    exit(0);
}