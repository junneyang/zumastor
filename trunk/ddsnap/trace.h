#include <string.h>
#include <stdio.h>

#define BREAK asm("int3")
#define warn(string, args...) do { fprintf(stderr, "[%u] %s: " string "\n", getpid(), __func__, ##args); } while (0)
#define error(string, args...) do { warn(string, ##args); BREAK; } while (0) 
#define assert(expr) do { if (!(expr)) error("Failed assertion \"%s\"\n", #expr); } while (0)

#define trace_on(args) do { args } while (0)
#define trace_off(args) do { /* nothing */ } while (0)
