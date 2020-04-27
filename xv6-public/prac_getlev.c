#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"

int
getlev(void)
{
    return myproc()->qLevel;
}

int
sys_getlev(void)
{
   return getlev();
}
