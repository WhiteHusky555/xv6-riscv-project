// user/shebang_test.c
#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int main() {
    printf("\n========== Shebang Test Suite ==========\n\n");
    
    // ===== ТЕСТ 1: Простой shebang без аргументов =====
    // Проверяем базовую способность exec запускать интерпретатор для скрипта.
    printf("Test 1: Creating script with simple shebang\n");
    int fd = open("test1.sh", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("Failed to create test1.sh\n");
        exit(1);
    }
    write(fd, "#!/cat\n", 7);
    write(fd, "Hello from script 1!\n", 21);
    write(fd, "This is line 2\n", 15);
    close(fd);
    printf("Created test1.sh with '#!/cat'\n");
    
    // ===== ТЕСТ 2: Shebang с аргументом (используем grep) =====
    // Т.к. cat в xv6 не понимает -n, используем grep.
    // Передаем слово "Special" как аргумент в shebang.
    // Grep воспримет его как паттерн для поиска в файле test2.sh.
    printf("\nTest 2: Creating script with shebang argument (grep Pattern)\n");
    fd = open("test2.sh", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("Failed to create test2.sh\n");
        exit(1);
    }
    write(fd, "#!/grep Special\n", 16); 
    write(fd, "Common line 1\n", 14);
    write(fd, "Special line 2\n", 15);
    write(fd, "Common line 3\n", 14);
    close(fd);
    printf("Created test2.sh with '#!/grep Special'\n");
    
    // ===== ТЕСТ 3: Shebang с echo и передачей аргументов пользователя =====
    // Проверяем, что аргументы из командной строки (argv[1...]) добавляются ПОСЛЕ пути к скрипту.
    printf("\nTest 3: Creating script with echo\n");
    fd = open("test3.sh", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("Failed to create test3.sh\n");
        exit(1);
    }
    write(fd, "#!/echo\n", 8);
    close(fd);
    printf("Created test3.sh with '#!/echo'\n");
    
    // ===== ТЕСТ 4: Проверка сложной комбинации =====
    // Интерпретатор: echo, аргумент shebang: Header, аргумент пользователя: Footer.
    printf("\nTest 4: Creating script for multi-argument test\n");
    fd = open("test4.sh", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("Failed to create test4.sh\n");
        exit(1);
    }
    write(fd, "#!/echo Header\n", 15);
    close(fd);
    printf("Created test4.sh with '#!/echo Header'\n");
    
    // ===== ВЫПОЛНЕНИЕ ТЕСТОВ =====
    
    printf("\n========================================\n");
    printf("Running Tests\n");
    printf("========================================\n\n");
    
    // Тест 1
    printf(">>> Test 1: ./test1.sh\n");
    printf("Expected: Show file content (cat script)\n");
    if (fork() == 0) {
        exec("test1.sh", (char*[]){"test1.sh", 0});
        printf("ERROR: exec failed!\n");
        exit(1);
    }
    wait(0);
    
    // Тест 2
    printf("\n>>> Test 2: ./test2.sh\n");
    printf("Expected: Show only 'Special line 2' (grep correctly got pattern from shebang)\n");
    if (fork() == 0) {
        exec("test2.sh", (char*[]){"test2.sh", 0});
        printf("ERROR: exec failed!\n");
        exit(1);
    }
    wait(0);
    
    // Тест 3
    printf("\n>>> Test 3: ./test3.sh hello world\n");
    printf("Expected: test3.sh hello world\n");
    if (fork() == 0) {
        exec("test3.sh", (char*[]){"test3.sh", "hello", "world", 0});
        printf("ERROR: exec failed!\n");
        exit(1);
    }
    wait(0);
    
    // Тест 4
    printf("\n>>> Test 4: ./test4.sh Footer\n");
    printf("Expected: Header test4.sh Footer\n");
    if (fork() == 0) {
        exec("test4.sh", (char*[]){"test4.sh", "Footer", 0});
        printf("ERROR: exec failed!\n");
        exit(1);
    }
    wait(0);
    
    printf("\n========================================\n");
    printf("All tests completed successfully!\n");
    printf("========================================\n");
    
    exit(0);
}