#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(void) {
    char buf[128];
    int n;

    printf("=== Тестирование синхронного ввода-вывода (Polling) ===\n");
    printf("Если ты видишь этот текст, значит uartwrite() успешно\n");
    printf("отправляет символы через цикл ожидания LSR_TX_IDLE.\n\n");
    
    printf("Введи любое сообщение и нажми Enter:\n> ");

    // Читаем из стандартного потока ввода (fd 0 - консоль)
    n = read(0, buf, sizeof(buf) - 1);
    
    if (n > 0) {
        buf[n] = '\0'; // Завершаем строку
        printf("\nУспех! Ядро прочитало твое сообщение:\n[%s]\n", buf);
        printf("consoleread() корректно опрашивает регистры UART без прерываний!\n");
    } else {
        printf("\nОшибка чтения!\n");
    }

    exit(0);
}