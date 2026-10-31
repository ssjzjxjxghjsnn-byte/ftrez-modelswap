#pragma once
#include <stddef.h>
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define mprotect dobby_test_mprotect
int dobby_test_mprotect(void* address, size_t size, int protection);
