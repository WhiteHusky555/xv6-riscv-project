#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"


// Объявляем наши функции из библиотеки pthreads (uthread.c), 
// если они еще не добавлены в user.h
extern int pthread_create(void (*)(void*), void*);
extern int pthread_join(void);

#define NTHREADS 4
#define ITERATIONS 10000

// ==========================================
// Глобальная переменная (общая память)
// ==========================================
int shared_counter = 0;

// Простой спинлок для пользовательского пространства (чтобы принты не смешивались)
struct lock {
    uint locked;
};

void acquire_lock(struct lock *lk) {
    // Используем встроенную атомарную инструкцию GCC
    while(__sync_lock_test_and_set(&lk->locked, 1) != 0);
    __sync_synchronize();
}

void release_lock(struct lock *lk) {
    __sync_synchronize();
    __sync_lock_release(&lk->locked);
}

struct lock print_lock;

// ==========================================
// Функция, которую выполняет каждый поток
// ==========================================
void worker(void *arg) {
    int tid = (int)(uint64)arg; // Получаем ID потока из аргумента

    // Блокировка вывода, чтобы символы не перемешивались
    acquire_lock(&print_lock);
    printf("Поток %d запущен...\n", tid);
    release_lock(&print_lock);

    // Увеличиваем общий счетчик ITERATIONS раз
    for(int i = 0; i < ITERATIONS; i++) {
        // Используем атомарное сложение, чтобы потоки не затерли значения друг друга
        // при одновременной записи в shared_counter
        __sync_fetch_and_add(&shared_counter, 1);
    }

    acquire_lock(&print_lock);
    printf("Поток %d завершил работу.\n", tid);
    release_lock(&print_lock);

    // ВАЖНО: В нашей простой реализации поток ОБЯЗАН вызвать exit(0) в конце.
    // Если он просто сделает return, он попытается вернуться по неверному 
    // адресу в стеке и вызовет ошибку (segfault).
    exit(0);
}

int main(void) {
    // Если вы реализовывали системный вызов для проверки свободной памяти
    printf("Свободно памяти: %d страниц\n", getfreemem());
    print_lock.locked = 0;
    
    printf("\n[TEST] Запуск теста pthreads (Потоков: %d, Итераций: %d)...\n", NTHREADS, ITERATIONS);

    // 1. Создаем потоки
    for(int i = 0; i < NTHREADS; i++) {
        // Передаем номер потока в качестве аргумента (кастуем int -> void*)
        if(pthread_create(worker, (void*)(uint64)i) < 0) {
            printf("Ошибка: не удалось создать поток %d\n", i);
            exit(1);
        }
    }

    // 2. Родительский процесс ждет завершения всех потоков
    for(int i = 0; i < NTHREADS; i++) {
        int pid = pthread_join();
        if(pid > 0) {
            // Здесь в реальной библиотеке мы бы делали free() для стека завершившегося потока
            // printf("Поток с PID %d успешно присоединен (joined).\n", pid);
        }
    }

    printf("\n[TEST] Все потоки завершились.\n");
    
    // 3. Проверка результата
    int expected = NTHREADS * ITERATIONS;
    printf("Ожидаемое значение shared_counter: %d\n", expected);
    printf("Реальное значение shared_counter:  %d\n", shared_counter);

    if(shared_counter == expected) {
        printf("[TEST] УСПЕХ! Общая память и многопоточность работают идеально.\n\n");
    } else {
        printf("[TEST] ПРОВАЛ! Обнаружена гонка данных или память не общая.\n\n");
    }

    exit(0);
}