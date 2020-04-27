#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
unsigned int ticks;

// to access variables in ptable which is in proc.c
extern int get_time_quantum();
extern int get_time_allotment();
extern int priority_boost(void);
extern int get_MLFQ_tick(void);
extern int increase_MLFQ_tick(void);
extern int increase_stride_tick(void);

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;

  }

  if(myproc() && myproc()->killed && (tf->cs&3)==DPL_USER)
      exit();

  if(get_MLFQ_tick() >= 100){
     priority_boost();
  }


  if(myproc() && myproc()->state == RUNNING && tf->trapno == T_IRQ0+IRQ_TIMER){
    // cprintf("timer interrupt occurs\n");
    myproc()->ticks++;
    myproc()->ticks_tq++;
    myproc()->ticks_ta++;
    
    //increase MLFQ tick & stride tick in ptable as well
    if(myproc()->cpu_share == 0){
        increase_MLFQ_tick();
    }else{
        increase_stride_tick();
    }

   // cprintf("timer interrupts occured, increase one tick\n pid: %d\n tf->trapno:%d\n MLFQ_tick:%d\n tick:%d\n ticks_tq:%d\n \n", myproc()->pid, tf->trapno, get_MLFQ_tick(), myproc()->ticks, myproc()->ticks_tq);

    if(myproc()->ticks_tq >= get_time_quantum() && myproc()->ticks_ta < get_time_allotment()){
        if(myproc()->qLevel < NMLFQ-1){
            // downgrade priority
            myproc()->qLevel++; 
        }

        myproc()->ticks_tq=0;

        // cprintf("yield cpu. time quantum satisfied\n");
        yield();
    }
    
    // time allotment satisfied
    // downgrade priority queue & yield CPU
    if(myproc()->qLevel < NMLFQ-1 && myproc()->ticks_ta >= get_time_allotment()){
       myproc()->qLevel+=1;

       myproc()->ticks_ta=0;

       yield();
    }
  }
}
