#define SBRK_ERROR ((char *)-1)

struct stat;

// system calls
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int*);
int pipe(int*);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int);
int exec(const char*, char**);
int open(const char*, int);
int mknod(const char*, short, short);
int unlink(const char*);
int fstat(int fd, struct stat*);
int link(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
char* sys_sbrk(int,int);
int pause(int);
int uptime(void);
// custom free memory size syscall
int getfreemem(void);   // возвращает количество свободных байт

// ulib.c
int stat(const char*, struct stat*);
char* strcpy(char*, const char*);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
char* gets(char*, int max);
uint strlen(const char*);
void* memset(void*, int, uint);
int atoi(const char*);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);
char* sbrk(int);
char* sbrklazy(int);

// printf.c
void fprintf(int, const char*, ...) __attribute__ ((format (printf, 2, 3)));
void printf(const char*, ...) __attribute__ ((format (printf, 1, 2)));

// umalloc.c
void* malloc(uint);
void free(void*);

// lab3_2
int printpagetable(void);

// lab 6
int kht_put(int, int);
int kht_get(int);
void kht_clear(void);
int clone(uint64 fcn, uint64 arg, uint64 stack);
int join(uint64 stack_addr);

// ethernet driver (kernel/virtio_net.c), raw link-layer access: a
// "frame" is exactly what goes on the wire starting at the
// destination MAC (struct eth_hdr in kernel/net.h), no IP stack.
int netsend(char *frame, int len);      // send a frame; len on success, -1 on error
int netrecv(char *buf, int maxlen);     // non-blocking; 0 = nothing yet, -1 = error
int netmac(char mac[6]);                // fetch our own MAC address