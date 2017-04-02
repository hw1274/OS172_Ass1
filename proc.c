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
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);
static unsigned long int next = 1;


enum policy { UNIFORM, PRIORITY, DYNAMIC };
enum policy pol = UNIFORM;

void pinit(void) {
  initlock(&ptable.lock, "ptable");
}

int systemUptime(void) {
  return ticks;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc* allocproc(void) {
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
  switch(pol) {
  	case UNIFORM:
		p->ntickets = 1;
		break;
	case PRIORITY:
		p->ntickets = 10;
		break;
	case DYNAMIC:
		p->ntickets = 20;
		break;
  }
  release(&ptable.lock);

  p->ctime = systemUptime();

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
void userinit(void) {
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
  p->stateTickChanged = systemUptime();
  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n) {
  uint sz;

  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void) {
  int i, pid;
  struct proc *np;

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);

  safestrcpy(np->name, proc->name, sizeof(proc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  np->stateTickChanged = systemUptime();

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(int status) {
  struct proc *p;
  int fd;

  proc->status = status;

  if(proc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(proc->cwd);
  end_op();
  proc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(proc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  int uptime = systemUptime();
  p->rutime += (uptime - p->stateTickChanged);
  p->stateTickChanged = uptime;
  proc->ttime = uptime;

  // Jump into the scheduler, never to return.

  proc->state = ZOMBIE;

  sched();
  panic("zombie exit");
}


// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait_stat(int * status, struct perf * perfPtr) {
  struct proc *p;
  int havekids, pid;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        if (status != 0) {
          *status = p-> status;
        }
        perfPtr->ctime = p->ctime;
        perfPtr->ttime = p->ttime;
        perfPtr->stime = p->stime;
        perfPtr->retime = p->retime;
        perfPtr->rutime = p->rutime;

        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->ntickets = 0;
        p->ctime = 0;
        p->ttime = 0;
        p->stime = 0;
        p->retime = 0;
        p->rutime = 0;

        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}



// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(int * status) {
  struct proc *p;
  int havekids, pid;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        if (status != 0) {
          *status = p-> status;
         }
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->ntickets = 0;
        p->ctime = 0;
        p->ttime = 0;
        p->stime = 0;
        p->retime = 0;
        p->rutime = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}

void distributeTickets(int ticketCount) {
  acquire(&ptable.lock);
  struct proc *p;
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
		if(p->state != UNUSED)
			p->ntickets = ticketCount;
	}
  	release(&ptable.lock);	
}


void priority(int priority) { 
  if (pol == PRIORITY)
  	proc->ntickets = priority;
}


void policy(int policy) {
  pol = policy;
  switch(pol) {
  	case UNIFORM:
  		distributeTickets(1);
  		break;
  	case PRIORITY:
  	  	distributeTickets(10);
  	  	break;
  	case DYNAMIC:
  		distributeTickets(20);
  		break;
  }
}

int getTicketSum(void) {
	int sum = 0;
  	acquire(&ptable.lock);
  struct proc *p;
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
		if(p->state != UNUSED)
			sum += p->ntickets;
	}
  	release(&ptable.lock);
	return sum;
}

static int first = 0;
int getRandomTicket(void) {

  if (first == 0 && ticks != 0){   
      first = 1;
      next = ticks;
    }

	int ticketSum = getTicketSum();
	if (ticketSum == 0)
		return 0;
    next = next * 1103515245 + 12341;
    return (unsigned int)(next/65536) % ticketSum;
}

struct proc* getSelectedProc(int ticketNum) {
  	acquire(&ptable.lock);
    struct proc *p;
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
		if(p->state == UNUSED)
			continue;
		ticketNum -= p->ntickets;
		if (ticketNum < 0) {
			release(&ptable.lock);
			return p;
		}
	}
  	release(&ptable.lock);
	return 0;
}


void redistributeTickets() {
	int state = proc->state;
	int ticketCount = proc->ntickets;
	if (state == RUNNABLE && ticketCount > 1)
		proc->ntickets--;
	if (state == SLEEPING) {
		if (ticketCount > 90)
			proc->ntickets = 100;
		else
			proc->ntickets += 10;
	}
}


//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
//IMPLEMENTED SKEDULAR
void scheduler(void) {
	struct proc *p;
	int ticketNum;

	for(;;) {
		sti();	// Enable interrupts on this processor.
		ticketNum = getRandomTicket();
		p = getSelectedProc(ticketNum);
		acquire(&ptable.lock);
		if(p != 0 && p->state == RUNNABLE) {

			// Switch to chosen process.  It is the process's job
			// to release ptable.lock and then reacquire it
			// before jumping back to us.
			proc = p;
			switchuvm(p);
      int uptime = systemUptime();
      p->retime += (uptime - p->stateTickChanged);
      p->stateTickChanged = uptime;
			p->state = RUNNING;
			swtch(&cpu->scheduler, p->context);
			switchkvm();

			if (pol == DYNAMIC)
				redistributeTickets();
			// Process is done running for now.
			// It should have changed its p->state before coming back.


			proc = 0;
		}
		release(&ptable.lock);

	}
}



//ORIGINAL SKEDULAR
void orig_scheduler(void) {
	struct proc *p;

	for(;;){
		// Enable interrupts on this processor.
		sti();

		// Loop over process table looking for process to run.
		acquire(&ptable.lock);
		for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
			if(p->state != RUNNABLE)
				continue;

			// Switch to chosen process.  It is the process's job
			// to release ptable.lock and then reacquire it
			// before jumping back to us.
			proc = p;
			switchuvm(p);
			p->state = RUNNING;
			swtch(&cpu->scheduler, p->context);
			switchkvm();

			// Process is done running for now.
			// It should have changed its p->state before coming back.
			proc = 0;
		}
		release(&ptable.lock);

	}
}



// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void) {
  int intena;

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void) {
  acquire(&ptable.lock);  //DOC: yieldlock
  

  int uptime = systemUptime();
  proc->rutime += (uptime - proc->stateTickChanged);
  proc->stateTickChanged = uptime;
  proc->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void) {
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
void sleep(void *chan, struct spinlock *lk) {
  if(proc == 0)
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
  proc->chan = chan;
  proc->state = SLEEPING;
  int uptime = systemUptime();
  proc->rutime += (uptime - proc->stateTickChanged);
  proc->stateTickChanged = uptime;

  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void wakeup1(void *chan) {
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan) {
      int uptime = systemUptime();
      p->stime += (uptime - p->stateTickChanged);
      p->stateTickChanged = uptime;
      p->state = RUNNABLE;
    }
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan) {
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid) {
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING) {
        int uptime = systemUptime();
        p->stime += (uptime - p->stateTickChanged);
        p->stateTickChanged = uptime;
        p->state = RUNNABLE;
      }
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
void procdump(void) {
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
