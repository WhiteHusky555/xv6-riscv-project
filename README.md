# Enhanced xv6-riscv Kernel: Multithreading, Parallel Allocator & Polling I/O

> Глубоко модернизированная версия учебной ОС **xv6-riscv**.  
> Оригинальное ядро переработано для полноценной и безопасной работы в многоядерной среде (`CPUS=4`) с поддержкой лёгковесных потоков, оптимизированным параллельным выделением памяти, неблокирующими структурами данных и подсистемой ввода-вывода на основе опроса.

---

## Содержание

- [Основные изменения и фичи ядра](#основные-изменения-и-фичи-ядра)
- [Борьба с критическими багами](#борьба-с-критическими-багами-ядра)
- [Тестовый пул](#тестовый-пул)
- [Инструкция по запуску](#инструкция-по-запуску)

---

## Основные изменения и фичи ядра

### 1. Поддержка многопоточности (Ядерный pthreads)

Реализована поддержка многопоточности внутри одного процесса с полноценным разделением адресного пространства.

| Компонент | Описание |
|---|---|
| `clone` | Создаёт новый поток, разделяющий с родителем `pagetable` и `sz`, с выделением уникального стека в user-space |
| `join` | Блокирует родительский процесс до завершения потока; возвращает адрес стека для освобождения через `free()` |
| `uthread.c` | Пользовательская библиотека-обёртка с интерфейсом `pthread_create` / `pthread_join` |

---

### 2. Параллельный аллокатор физической памяти (Lock Contention Fix)

В оригинальном xv6 глобальная блокировка `kmem.lock` вынуждала ядра простаивать в очереди при одновременном выделении памяти.

**Решение:**

- Структура памяти превращена в массив `kmem[NCPU]` — каждое ядро получает индивидуальный `freelist` под локальным спинлоком.
- **Page Stealing («воровство страниц»):** при нехватке страниц аллокатор атомарно забирает страницу у соседнего CPU.
- **`getfreemem`:** системный вызов для сквозного подсчёта свободной памяти по всем ядрам.

---

### 3. Ввод-вывод на основе опроса (Polling I/O)

Ядро переведено с событийной модели прерываний UART на модель постоянного опроса устройства.

- **UART без прерываний:** в `uartinit` прерывания отключены (`IER = 0x00`); вывод в `uartwrite` работает в режиме active polling.
- **Оптимизация консоли:** `consoleread` опрашивает UART напрямую. При отсутствии ввода интегрирована инструкция `wfi` (Wait For Interrupt), усыпляющая ядро до ближайшего тика таймера — без 100% холостой нагрузки.

---

### 4. Конкурентная хеш-таблица в ядре

Потокобезопасная хеш-таблица типа `map<uint, uint>` для работы в пространстве ядра.

- **Сегментированные блокировки:** таблица разделена на 8 независимых сегментов с индивидуальными спинлоками. Потоки пишут параллельно, если их ключи хэшируются в разные сегменты.
- **Copy-on-Write (CoW):** обновление элементов происходит путём создания новой копии цепочки с атомарным переключением указателя — без модификации «вживую», минимизируя время удержания блокировки.

---

## Борьба с критическими багами ядра

### Коллизия `TRAPFRAME` (`scause 0x2` — Illegal Instruction)

При запуске параллельных потоков контексты регистров затирали друг друга. Решение: адрес сохранения контекста сделан уникальным для каждого потока (`p->trapframe_va`) и динамически передаётся через `sscratch` при входе/выходе из user-mode; соответствующие изменения внесены в `trampoline.S`.

### Гонка сигналов Kill / Sleep (Lost Wakeup)

Устранено состязание, при котором процесс мог получить сигнал `kill` между проверкой флага и уходом в сон. Проверка `p->killed` перенесена внутрь функции `sleep` под защиту `p->lock`.

### Мёртвая блокировка в `growproc`

Исправлена уязвимость: аварийный выход из `sbrk` при нехватке памяти оставлял `wait_lock` захваченным,  намертво вешая планировщик.

### Симбиоз потоков и Strict Allocation

Отказ от Lazy Allocation в пользу строгой (eager) аллокации в `sys_sbrk`. Это гарантирует мгновенную синхронизацию таблиц страниц всех родственных потоков через `u2kvmcopy` при выделении кучи.

---

## Тестовый пул

| Утилита | Описание |
|---|---|
| `pthread_test` | Стресс-тест библиотеки потоков: 4 потока инкрементируют общую переменную по 10 000 раз через `__sync_fetch_and_add` |
| `kht_test` | Проверка конкурентности хеш-таблицы: параллельная запись с разных ядер, коллизии, CoW |
| `kill_slp_test` | 100 форков на чтении; родитель убивает их в момент перехода в сон — проверка на зависания |
| `kalloctest` | Нагрузочный тест аллокатора: все ядра одновременно выделяют и освобождают страницы |
| `polltest` | Стабильность ввода-вывода и эхо-вывода в консоли без аппаратных прерываний |
| `usertests` | Стандартный пул тестов xv6 (модифицирован `countfree()` для работы с `getfreemem`). |
✅ **ALL TESTS PASSED**

---

## Инструкция по запуску

### Требования (Prerequisites)

Необходим тулчейн для кросс-компиляции под RISC-V и эмулятор QEMU.

```bash
sudo apt-get install git build-essential gdb-multiarch \
    qemu-system-riscv64 gcc-riscv64-unknown-elf python3
```

### Сборка и запуск

```bash
# 1. Склонировать репозиторий
git clone https://github.com/WhiteHusky555/xv6-riscv-project.git
cd xv6-riscv

# 2. Сборка и запуск в QEMU (4 ядра по умолчанию)
make clean
make qemu
```

### Запуск тестов внутри xv6

После загрузки откроется консоль xv6 (`$`):

```bash
# Многопоточность
$ pthread_test

# Параллельная хеш-таблица
$ kht_test

# Глобальный стресс-тест ядра (~1-2 минуты)
$ usertests
```

### Автоматическое тестирование

```bash
# Все тесты через скрипт
python3 test-xv6.py '.*'

# Или через Makefile
make grade
```

> Выход из QEMU: `Ctrl+A`, затем `X`

# Original README

xv6 is a re-implementation of Dennis Ritchie's and Ken Thompson's Unix
Version 6 (v6).  xv6 loosely follows the structure and style of v6,
but is implemented for a modern RISC-V multiprocessor using ANSI C.

ACKNOWLEDGMENTS

xv6 is inspired by John Lions's Commentary on UNIX 6th Edition (Peer
to Peer Communications; ISBN: 1-57398-013-7; 1st edition (June 14,
2000)).  See also https://pdos.csail.mit.edu/6.1810/, which provides
pointers to on-line resources for v6.

The following people have made contributions: Russ Cox (context switching,
locking), Cliff Frey (MP), Xiao Yu (MP), Nickolai Zeldovich, and Austin
Clements.

We are also grateful for the bug reports and patches contributed by
Abhinavpatel00, Takahiro Aoyagi, Marcelo Arroyo, Hirbod Behnam, Silas
Boyd-Wickizer, Anton Burtsev, carlclone, Ian Chen, clivezeng, Dan
Cross, Cody Cutler, Mike CAT, Tej Chajed, Asami Doi,Wenyang Duan,
echtwerner, eyalz800, Nelson Elhage, Saar Ettinger, Alice Ferrazzi,
Nathaniel Filardo, flespark, Peter Froehlich, Yakir Goaron, Shivam
Handa, Matt Harvey, Bryan Henry, jaichenhengjie, Jim Huang, Matúš
Jókay, John Jolly, Alexander Kapshuk, Anders Kaseorg, kehao95,
Wolfgang Keller, Jungwoo Kim, Jonathan Kimmitt, Eddie Kohler, Vadim
Kolontsov, Austin Liew, l0stman, Pavan Maddamsetti, Imbar Marinescu,
Yandong Mao, Matan Shabtay, Hitoshi Mitake, Carmi Merimovich,
mes900903, Mark Morrissey, mtasm, Joel Nider, Hayato Ohhashi,
OptimisticSide, papparapa, phosphagos, Harry Porter, Greg Price, Zheng
qhuo, Quancheng, RayAndrew, Jude Rich, segfault, Ayan Shafqat, Eldar
Sehayek, Yongming Shen, Fumiya Shigemitsu, snoire, Taojie, Cam Tenny,
tyfkda, Warren Toomey, Stephen Tu, Alissa Tung, Rafael Ubal, unicornx,
Amane Uehara, Pablo Ventura, Luc Videau, Xi Wang, WaheedHafez, Keiichi
Watanabe, Lucas Wolf, Nicolas Wolovick, wxdao, Grant Wu, x653, Andy
Zhang, Jindong Zhang, Icenowy Zheng, ZhUyU1997, and Zou Chang Wei.

ERROR REPORTS

Please send errors and suggestions to Frans Kaashoek and Robert Morris
(kaashoek,rtm@mit.edu).  The main purpose of xv6 is as a teaching
operating system for MIT's 6.1810, so we are more interested in
simplifications and clarifications than new features.

BUILDING AND RUNNING XV6

You will need a RISC-V "newlib" tool chain from
https://github.com/riscv/riscv-gnu-toolchain, and qemu compiled for
riscv64-softmmu.  Once they are installed, and in your shell
search path, you can run "make qemu".
