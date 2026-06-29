#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// xv6 выделяет 4096 байт на каждый узел. Чтобы не исчерпать 
// всю физическую память (OOM) и не обрушить ядро, держим числа небольшими.
#define NCHILDREN 3
#define ITERATIONS 20

// Функция, которую будет выполнять каждый "поток" (дочерний процесс)
void run_worker(int worker_id) 
{
    printf("Worker %d started\n", worker_id);

    for (int i = 0; i < ITERATIONS; i++) {
        // 1. УНИКАЛЬНАЯ ЗАПИСЬ: Каждый воркер пишет свои ключи (проверка параллелизма корзин)
        int unique_key = worker_id * 1000 + i;
        kht_put(unique_key, worker_id);

        // 2. КОНКУРЕНТНАЯ ЗАПИСЬ: Все воркеры одновременно долбят один и тот же ключ!
        // Это жесткий стресс-тест для нашего Copy-on-Write и спин-блокировки конкретного сегмента.
        kht_put(42, worker_id * 10 + i);

        // 3. КОНКУРЕНТНОЕ ЧТЕНИЕ: Читаем общий ключ сразу после записи.
        // Так как читателей не блокируют глобально, кто-то прочитает старое значение, 
        // а кто-то новое, но главное — ядро не должно упасть (panic).
        kht_get(42);
    }

    printf("Worker %d finished\n", worker_id);
    exit(0);
}

int main(void) 
{
    printf("\n[CONCURRENCY TEST] Starting multithreaded hash table test...\n");

    int pids[NCHILDREN];

    // Инициализируем общий ключ до запуска процессов
    kht_put(42, 0);

    // Порождаем процессы. QEMU раскидает их по разным ядрам CPU.
    for (int i = 0; i < NCHILDREN; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            printf("Fork failed!\n");
            exit(1);
        }
        if (pids[i] == 0) {
            // Дочерний процесс уходит выполнять работу и завершается
            run_worker(i + 1);
        }
    }

    // Родительский процесс ждет завершения всех дочерних
    for (int i = 0; i < NCHILDREN; i++) {
        wait(0);
    }

    printf("\n[CONCURRENCY TEST] All workers completed successfully!\n");
    
    // Проверяем, что таблица жива и отвечает
    int final_val = kht_get(42);
    printf("Final value of shared key 42: %d\n", final_val);
    
    // Проверяем пару уникальных ключей от разных воркеров
    printf("Worker 1 key (1005): %d\n", kht_get(1005));
    printf("Worker 3 key (3019): %d\n", kht_get(3019));

    printf("[CONCURRENCY TEST] PASSED.\n\n");
    kht_clear();
    exit(0);
}