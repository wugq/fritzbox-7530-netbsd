/* stand-in for the kernel header, for ubi-harness.c on the PC */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define roundup(x,y) ((((x)+((y)-1))/(y))*(y))
