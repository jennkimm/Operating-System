#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"


struct {
  struct spinlock lock;

  // max 64 process
  struct proc proc[NPROC]; // processes
  int MLFQ_time_quantum[NMLFQ]; // to store the time slice of each queue 
  int MLFQ_tick; // count tick to check priority_boost() time
  int MLFQ_time_allotment[NMLFQ-1]; // to store the time allotment of each queue

  //this refers to a range of MLFQ level that would be run. from min(=high priority) to max(=low priority).
  int max_level; 
  int min_level; 

} ptable;

struct {
  struct spinlock lock;
  struct proc *stride_proc[NPROC]; // process in stride scheduling

  int total_cpu_share; 
  int stride_time_quantum;
  int stride_queue_size;
  int stride_tick;

} htable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

static void add_parent_in_stride(int i);
static void add_child_in_stride(int i);
static unsigned int rand(void);

// called in proc.c & trap.c
int
get_time_quantum(){
  struct proc *proc = myproc();

  if(proc && proc->cpu_share != 0){
     return htable.stride_time_quantum;
  }else{
    return ptable.MLFQ_time_quantum[proc->qLevel];
  }
}

int
get_time_allotment(){
   struct proc *proc = myproc();

   if(proc && proc->cpu_share !=0 && proc->qLevel != NMLFQ-1)
     return ptable.MLFQ_time_allotment[proc->qLevel];
   else{
     return 100; 
   }
}

int
get_MLFQ_tick(void){
    return ptable.MLFQ_tick;
}

int
get_stride_tick(void){
    return htable.stride_tick;
}

void
increase_MLFQ_tick(void){
  ptable.MLFQ_tick++;
  //cprintf("MLFQ_tick: %d ", ptable.MLFQ_tick);
}

void
increase_stride_tick(void){
  htable.stride_tick++;
 // cprintf("stride_tick: %d ", htable.stride_tick);
}


void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  
  // Initialize ptable
  ptable.MLFQ_time_quantum[0]=1;
  ptable.MLFQ_time_quantum[1]=2;
  ptable.MLFQ_time_quantum[2]=4;

  ptable.MLFQ_time_allotment[0]=5;
  ptable.MLFQ_time_allotment[1]=10;

  ptable.MLFQ_tick = 0;

  memset(htable.stride_proc, 0, sizeof(htable.stride_proc));

  ptable.min_level = NMLFQ - 1;
  ptable.max_level = NMLFQ - 1;

  htable.stride_tick=0;
  htable.stride_queue_size=0;
  htable.stride_time_quantum = 1;
  htable.total_cpu_share=0;
  //cprintf("(pinit) ptable initialized\n");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
     panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  
  //Initialize the variables in proc
  p->ticks_tq = 0;
  p->ticks_ta = 0;
  p->ticks = 0;
  p->qLevel = 0; // when a job enters, place at the highest priority

  p->cpu_share = 0;
  p->stride = 0;
  p->pass = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;
  int idx; // index for stride

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // should delete a process in stride queue
  if(curproc->cpu_share !=0){
    int add_parent;

   // cprintf("(exit) process in stride queue delete\n");

    htable.total_cpu_share -= curproc->cpu_share;

    for(idx=1; idx < NPROC; ++idx){
      if(htable.stride_proc[idx] == curproc){
        break;
      }
    }

    if(idx == NPROC){
       panic("not a valid process of stride scheduler");
    }

    // delete the process from the heap
    htable.stride_proc[idx] = htable.stride_proc[htable.stride_queue_size];
    htable.stride_proc[htable.stride_queue_size--]=0;

    if(idx==1){
      add_parent=0;
    }else{
      add_parent  = htable.stride_proc[idx]->pass < htable.stride_proc[parent(idx)] -> pass ? 1: 0;  
    }

    if(htable.stride_queue_size==0 || htable.stride_queue_size==1){
    
    }else{
       if(add_parent){
        add_child_in_stride(idx);
       }else{
        add_parent_in_stride(idx);
       }
    }
  }
 

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;

      // reinitialize when children finished & wait for parent to wake me up
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;

        p->ticks=0;
        p->ticks_tq=0;
        p->ticks_ta=0;
        p->qLevel=0;
        p->cpu_share=0;
        p->stride=0;
        p->pass=0;

        p->state = UNUSED;
        release(&ptable.lock);

        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

// control minimum heap for process of stride scheduling
void
add_parent_in_stride(int i){
 struct proc* new_proc = htable.stride_proc[i];

 acquire(&htable.lock);
 while(i !=1){ // if it is not root
    // if the parent is greater than the child
    if(htable.stride_proc[i] < htable.stride_proc[parent(i)]){ 
       htable.stride_proc[i] = htable.stride_proc[parent(i)];
       i=parent(i);
       //i/=2;
    }else{
       return;
    }
 }

 htable.stride_proc[i] = new_proc;
 release(&htable.lock);
}

void
add_child_in_stride(int i){
 struct proc* p = htable.stride_proc[i];

 acquire(&htable.lock);
 while( leftchild(i) < NPROC && i <= htable.stride_queue_size){
    if(htable.stride_proc[leftchild(i)] && htable.stride_proc[rightchild(i)]){
      i = htable.stride_proc[leftchild(i)]->pass < htable.stride_proc[rightchild(i)] -> pass ? leftchild(i) : rightchild(i);

      if(htable.stride_proc[i]->pass < p->pass){
        htable.stride_proc[parent(i)] = htable.stride_proc[i];
      }else{
        i /= 2;
        break;
      }
    }else if(htable.stride_proc[leftchild(i)]){
      i *= 2;
      if(htable.stride_proc[i]->pass < p->pass){
        htable.stride_proc[parent(i)] = htable.stride_proc[i];
      }else{
        i /= 2;
      }
      break;
    }else{
      break;
    }
 }

 htable.stride_proc[i]=p;
 release(&htable.lock);
}

int
find_stride_idx_to_run(void){
  struct proc* ready_proc;
  int i;

  int index=1;

  acquire(&htable.lock);
  while(index <= htable.stride_queue_size){
    
    // if there is runnable process, just run it
    if(htable.stride_proc[index]->state == RUNNABLE){
      ready_proc = htable.stride_proc[index];
      ready_proc -> pass += ready_proc->stride;
      break;
    }else{
     // else, find process that has smaller pass value among children
      if( rightchild(index) <= htable.stride_queue_size){
        if(htable.stride_proc[leftchild(index)]->pass < htable.stride_proc[rightchild(index)]->pass){
            index = leftchild(index);
        }else{
            index = rightchild(index);
        }
      }else if( leftchild(index) > htable.stride_queue_size){
        index=htable.stride_queue_size+1;
      }else{
        index=leftchild(index);
      }

    }
  }


  if(index >= htable.stride_queue_size+1){
    for(i = 1 ; i <= htable.stride_queue_size; ++i){
      if(htable.stride_proc[i]->state == RUNNABLE){
        index = i;
        break;
      }
    }
  }else{

  }

  release(&htable.lock);

  return index;
}


int
is_MLFQ_or_stride(){ 
 //generate random number
 int random = rand() % 100;
 
 if(random < htable.total_cpu_share){
    return 1; // stride
 }else{
    return 0; // MLFQ
 }
}

unsigned long randstate=1;
unsigned int
rand()
{
    randstate = randstate * 1664525 + 1013904223;
    return randstate;
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  int MLFQ_or_stride;
  int is_stride=0; // if 0, it's for stride scheduling, else MLFQ

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

    MLFQ_or_stride=1;
    
    ptable.max_level=NMLFQ-1;

    // traverse proc array
    while(ptable.max_level < NMLFQ){
        //cprintf("ptable.max_level=%d\n",ptable.max_level);
        ptable.min_level=NMLFQ-1; // to store minimum queue level in one traversing
        
        // Is this process MLFQ or stride?
        for(p=ptable.proc; p<&ptable.proc[NPROC]; p++){
            if(htable.total_cpu_share==0){ // MLFQ
                is_stride=0;
                MLFQ_or_stride=0;
            }else if(MLFQ_or_stride){ // stride
                is_stride=1;
                MLFQ_or_stride=is_MLFQ_or_stride();
            }else{
                continue;
            }
           
            // stride scheduling
            if(is_stride){
                int stride_idx;
                stride_idx = find_stride_idx_to_run();
                
                // exists
                if(stride_idx <= htable.stride_queue_size){
                  p = htable.stride_proc[stride_idx];

                }
                else{ // not exists
                  is_stride=0;
                  MLFQ_or_stride=0;
                  continue;
                }
            }

            // MLFQ scheduling
            else{
                // proceses who have equal or lower priority than max_level is found in MLFQ
                if(p->state == RUNNABLE && p->qLevel <= ptable.max_level){
                    ptable.max_level = p->qLevel < ptable.max_level ? p->qLevel : ptable.max_level; 
                    ptable.min_level = ptable.max_level;
                }else{ // not exists
                    MLFQ_or_stride=0;
                    continue;
                }
            }

            //struct proc* mlfq = p;
            int tick_before_mlfq = ptable.MLFQ_tick; // to check MLFQ used more than one tick
            
           // cprintf("(scheduler)Right before context change\n");
           // if(MLFQ_or_stride)
           //       cprintf("stride\n");
           // else
           //       cprintf("MLFQ\n");
           // cprintf("qlevel = %d\n", p->qLevel);
           // cprintf("cpu_share = %d\n", p->cpu_share);

            c->proc=p;
            switchuvm(p);
            p->state=RUNNING;
            swtch(&(c->scheduler), p->context);
            switchkvm();
            
            if(!is_stride && htable.total_cpu_share!=0 && p->state == RUNNABLE && p->cpu_share == 0 && ptable.MLFQ_tick == tick_before_mlfq){
                MLFQ_or_stride=0; // MLFQ
                p--; // run this process again. make this process use at least one tick
            }else{
                MLFQ_or_stride=1; // stride
            }

            c->proc = 0;
         }
        
        
         if(ptable.min_level == NMLFQ-1){
            // no process to run btw min ~ max
            // increase max_level
            ptable.max_level++;
         }
       }
      
       release(&ptable.lock);  
    }
    /*
 
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);
    */

}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);

  mycpu()->intena = intena;

}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int
getppid(void)
{
    return myproc()->parent->pid;
}

void
priority_boost(void){
    struct proc* p;
    //cprintf("priority boosting");
    
    for(p=ptable.proc; p<&ptable.proc[NPROC];p++){
        // boost up priority
        p->qLevel = 0;
        p->ticks=0;
        p->ticks_tq=0;
        p->ticks_ta=0;
        ptable.MLFQ_tick=0;
    }

}

int
set_cpu_share(int share){
  int is_new;
  int idx;
  struct proc *curproc = myproc();

  // check arguments
  if(share<MIN_CPU_SHARE || share>MAX_CPU_SHARE){
     // exception
     cprintf("cpu share is not valid!\n");
     return -1;
  }

  if(curproc->cpu_share == 0)
      is_new=1;
  else
      is_new=0; // cpu share already set
  
  if(htable.total_cpu_share + share > MAX_CPU_SHARE){
     // exception
     return -1;   
  }

   // set cpu share
   htable.total_cpu_share += share; 
   curproc->cpu_share=share;
   curproc->stride= CONST / share;
  
   // make current process point the idx-th stride process when the system call is invoked
   if(is_new){
     
     if(htable.stride_proc[1]){
        curproc->pass = htable.stride_proc[1]->pass;
     }

     idx=++htable.stride_queue_size;

     while(idx!=1){
       htable.stride_proc[idx]=htable.stride_proc[parent(idx)];
       idx/=2;
     }

     htable.stride_proc[idx]=curproc;

   }else{

   }
  return 0;
}
