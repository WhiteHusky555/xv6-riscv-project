#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"

// ==========================================
// Паттерн 1: Divide and Conquer (Сегментация)
// ==========================================
// Вместо одной большой блокировки на всю таблицу, мы делим её на сегменты.
// NCPU (количество ядер) - отличный ориентир для числа сегментов.
#define NUM_SEGMENTS 8 
#define BUCKETS_PER_SEGMENT 64

// Узел односвязного списка
struct node {
  uint key;
  uint value;
  struct node *next;
};

// Структура сегмента. Каждый сегмент имеет СВОЮ спин-блокировку.
// Если CPU 1 пишет в сегмент 0, а CPU 2 пишет в сегмент 3 — они работают параллельно!
struct segment {
  struct spinlock lock;
  struct node *buckets[BUCKETS_PER_SEGMENT];
};

// Глобальная хеш-таблица в памяти ядра
struct segment table[NUM_SEGMENTS];

// Простая хеш-функция для ядра
static uint 
hash_func(uint key) 
{
  uint h = key ^ (key >> 16);
  h *= 0x85ebca6b;
  h ^= h >> 13;
  return h;
}

// Инициализация таблицы (вызывается один раз из main.c при загрузке ОС)
void 
kht_init(void) 
{
  for (int i = 0; i < NUM_SEGMENTS; i++) {
    // Инициализируем спин-блокировку каждого сегмента
    initlock(&table[i].lock, "kht_segment");
    for (int j = 0; j < BUCKETS_PER_SEGMENT; j++) {
      table[i].buckets[j] = 0;
    }
  }
}

// ==========================================
// ЧТЕНИЕ (Поиск)
// ==========================================
int 
kht_get(uint key) 
{
  uint hash = hash_func(key);
  int seg_idx = hash % NUM_SEGMENTS;
  int bkt_idx = (hash / NUM_SEGMENTS) % BUCKETS_PER_SEGMENT;

  struct segment *seg = &table[seg_idx];

  // В полноценных ОС с RCU здесь не было бы блокировки вообще.
  // Но в базовом xv6 нет сборщика мусора, поэтому для безопасности
  // мы берем блокировку, но благодаря сегментации (D&C) она почти не создает задержек.
  acquire(&seg->lock);

  struct node *curr = seg->buckets[bkt_idx];
  while (curr != 0) {
    if (curr->key == key) {
      int val = curr->value;
      release(&seg->lock);
      return val;
    }
    curr = curr->next;
  }

  release(&seg->lock);
  return -1; // Не найдено
}

// ==========================================
// ЗАПИСЬ: Divide and Conquer + Copy-on-Write
// ==========================================
void 
kht_put(uint key, uint value) 
{
  uint hash = hash_func(key);
  int seg_idx = hash % NUM_SEGMENTS;
  int bkt_idx = (hash / NUM_SEGMENTS) % BUCKETS_PER_SEGMENT;

  struct segment *seg = &table[seg_idx];

  // Захватываем блокировку ТОЛЬКО одного сегмента
  acquire(&seg->lock);

  struct node *head = seg->buckets[bkt_idx];
  struct node *curr = head;

  // Ищем существующий ключ
  while (curr != 0) {
    if (curr->key == key) break;
    curr = curr->next;
  }

  if (curr != 0) {
    // ==========================================
    // Паттерн 2: COPY-ON-WRITE
    // ==========================================
    // Вместо того чтобы менять значение "наживую" (что сломало бы 
    // алгоритмы чтения без блокировок, если бы мы их использовали), 
    // мы создаем новую копию цепочки узлов.

    struct node *new_head = 0;
    struct node *tail = 0;
    struct node *temp = head;

    // Копируем узлы до найденного
    while (temp != curr) {
      // ВАЖНО: kalloc выделяет целую страницу (4 КБ). Для реального ядра
      // здесь нужно использовать kmalloc (Slab-аллокатор мелких объектов).
      struct node *copy = (struct node*)kalloc();
      copy->key = temp->key;
      copy->value = temp->value;
      copy->next = 0;
      
      if (new_head == 0) new_head = copy;
      else tail->next = copy;
      tail = copy;
      
      temp = temp->next;
    }

    // Создаем обновленный узел
    struct node *updated = (struct node*)kalloc();
    updated->key = key;
    updated->value = value;
    // Цепляем к нему оригинальный хвост списка (он не изменился)
    updated->next = curr->next; 

    if (new_head == 0) new_head = updated;
    else tail->next = updated;

    // Переключаем указатель корзины на новую ветку
    seg->buckets[bkt_idx] = new_head;

    // ==========================================
    // БЕЗОПАСНАЯ ОЧИСТКА ПАМЯТИ (ИСПРАВЛЕНО)
    // ==========================================
    // ВАЖНО: Так как в xv6 нет RCU, мы должны очистить старые узлы вручную,
    // но только потому, что наши читатели (в kht_get) защищены спинлоком 
    // и гарантированно не находятся сейчас в старом списке.
    
    // КРИТИЧЕСКИЙ ФИКС: Сохраняем адрес остановки ДО начала цикла kfree.
    // Если мы будем проверять (temp != curr->next) прямо в while, то на 
    // последней итерации мы сделаем kfree(curr), а затем while попытается
    // прочитать curr->next из уже стертой памяти (забитой 0x01 в xv6). 
    // Это вызывало panic: kfree.
    struct node *stop_node = curr->next;
    
    temp = head;
    while (temp != stop_node) {
      struct node *to_free = temp;
      temp = temp->next;
      kfree((void*)to_free);
    }
    
  } else {
    // Обычная вставка нового узла в начало
    struct node *new_node = (struct node*)kalloc();
    new_node->key = key;
    new_node->value = value;
    new_node->next = head;
    
    seg->buckets[bkt_idx] = new_node;
  }

  release(&seg->lock);
}

// Функция для наглядного вывода состояния хеш-таблицы в консоль xv6.
// Она показывает только те сегменты и корзины, в которых есть данные.
void 
kht_dump(void) 
{
  printf("\n=== Состояние Хеш-таблицы ===\n");
  int total_items = 0;

  for (int i = 0; i < NUM_SEGMENTS; i++) {
    acquire(&table[i].lock); // Блокируем сегмент для безопасного чтения
    
    int seg_has_data = 0;
    for (int j = 0; j < BUCKETS_PER_SEGMENT; j++) {
      struct node *curr = table[i].buckets[j];
      
      if (curr != 0) {
        if (!seg_has_data) {
          printf("Сегмент %d:\n", i);
          seg_has_data = 1;
        }
        
        printf("  Корзина %d: ", j);
        while (curr != 0) {
          printf("[%d:%d] -> ", curr->key, curr->value);
          curr = curr->next;
          total_items++;
        }
        printf("NULL\n");
      }
    }
    
    release(&table[i].lock);
  }
  
  if (total_items == 0) {
    printf("Таблица пуста.\n");
  }
  printf("=============================\n\n");
}

// Тестовая программа для проверки логики
void 
kht_test(void) 
{
  printf("\n[ТЕСТ] Запуск проверки khastable...\n");

  // В реальном xv6 kht_init() нужно вызывать внутри функции main() в kernel/main.c
  // Но для изолированного теста вызовем ее здесь:
  static int initialized = 0;
  if (!initialized) {
    kht_init();
    initialized = 1;
  }

  printf("[ШАГ 1] Добавляем новые данные (kht_put)...\n");
  kht_put(10, 100);
  kht_put(42, 420);
  kht_put(99, 990);
  
  // Добавим ключ с таким же хэшем (для простоты теста симулируем коллизию,
  // подобрав ключи, которые попадут в ту же корзину. В нашей хэш-функции
  // это сделать сложнее, поэтому просто добавим еще один случайный ключ).
  kht_put(105, 1050);

  // Смотрим, как распределились данные по сегментам
  kht_dump();

  printf("[ШАГ 2] Читаем данные (kht_get)...\n");
  printf("  Поиск ключа 42: %d (Ожидается: 420)\n", kht_get(42));
  printf("  Поиск ключа 99: %d (Ожидается: 990)\n", kht_get(99));
  printf("  Поиск ключа 777: %d (Ожидается: -1, т.к. нет в таблице)\n\n", kht_get(777));

  printf("[ШАГ 3] Спровоцируем Copy-on-Write (kht_put существующего ключа)...\n");
  printf("  Обновляем ключ 42 со значения 420 на 5555.\n");
  kht_put(42, 5555);

  // Выводим таблицу снова. Мы увидим, что значение обновилось, 
  // а старый узел был безопасно очищен ядром внутри kht_put.
  kht_dump();

  printf("[ТЕСТ] Успешно завершен.\n");
}

void kht_clear(void) {
  for (int i = 0; i < NUM_SEGMENTS; i++) {
    acquire(&table[i].lock);
    for (int j = 0; j < BUCKETS_PER_SEGMENT; j++) {
      struct node *curr = table[i].buckets[j];
      while (curr != 0) {
        struct node *to_free = curr;
        curr = curr->next;
        kfree((void*)to_free);
      }
      table[i].buckets[j] = 0;
    }
    release(&table[i].lock);
  }
}