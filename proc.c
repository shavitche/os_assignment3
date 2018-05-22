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

extern int system_max_free_pages; // used to calculate free system-wide pages state
extern int system_used_pages;   // used to calculate free system-wide pages state
static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
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

#ifndef NONE
  // MINE:
  int page_index;
  p->page_fault_counter = 0;
  p->pageout_counter = 0;
  p->next_free_place_in_swapfile = 0;
  p->head = (struct page_description*) 0;
  p->tail = (struct page_description*) 0;
  for(page_index = 0; page_index < MAX_TOTAL_PAGES; page_index = page_index + 1){
    p->page_descriptions[page_index].pte = 0;
    p->page_descriptions[page_index].virtual_address = 0;
    p->page_descriptions[page_index].status = PG_UNSUED;
    p->page_descriptions[page_index].access_counter = 0xFFFFFFFF;
    p->page_descriptions[page_index].page_queue_counter = -1;
    p->page_descriptions[page_index].aging_bol_counter = 0;
    p->page_descriptions[page_index].swap_file_offset = -1;
    p->page_descriptions[page_index].next = (struct page_description*) 0;
    p->page_descriptions[page_index].prev = (struct page_description*) 0;
  }
#endif

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
#ifndef NONE
  // create swapfile more pid higher than 2 (not init, not sh)
  if (np->pid > 2){
    createSwapFile(np);
  }
#endif
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


#ifndef NONE
  if(curproc->pid > 2){
    // MINE:
    cprintf("forking\n");
    np->page_fault_counter = 0;
    np->pageout_counter = 0;
    np->next_free_place_in_swapfile = curproc->next_free_place_in_swapfile;

    // Deep copy page descriptions:
    for(i = 0; i < MAX_TOTAL_PAGES; i++){
      np->page_descriptions[i].swap_file_offset = curproc->page_descriptions[i].swap_file_offset;
      np->page_descriptions[i].access_counter = 0xFFFFFFFF;
      np->page_descriptions[i].page_queue_counter = curproc->page_descriptions[i].page_queue_counter;
      np->page_descriptions[i].status = curproc->page_descriptions[i].status;
      np->page_descriptions[i].virtual_address = curproc->page_descriptions[i].virtual_address;
      np->page_descriptions[i].pte = walkpgdir(np->pgdir, (char *)PGROUNDDOWN(np->page_descriptions[i].virtual_address), 0);
    }

    // Properly set next and prev of each page
    int j;
    for (i = 0; i < MAX_PSYC_PAGES; i++) {
      if(np->page_descriptions[i].status != PG_MEMORY && np->page_descriptions[i].status != PG_MUST_MEM)
        continue;

      for (j = 0; j < MAX_PSYC_PAGES; ++j) {
        if(np->page_descriptions[j].status != PG_MEMORY && np->page_descriptions[j].status != PG_MUST_MEM)
          continue;

        if (np->page_descriptions[j].virtual_address == curproc->page_descriptions[i].next->virtual_address)
          np->page_descriptions[i].next = &np->page_descriptions[j];
        if (np->page_descriptions[j].virtual_address == curproc->page_descriptions[i].prev->virtual_address)
          np->page_descriptions[i].prev = &np->page_descriptions[j];
      }
    }

    // Deep copy head and tail
    for(i = 0; i < MAX_TOTAL_PAGES; i++){
      if(curproc->head->virtual_address == curproc->page_descriptions[i].virtual_address){
        np->head = &np->page_descriptions[i];
      }
      if(curproc->tail->virtual_address == curproc->page_descriptions[i].virtual_address){
        np->tail = &np->page_descriptions[i];
      }
    }

    // Copy swap file from father:
    int bytes_read, written_bytes;
    uint offset = 0;
    char swap_buffer[PGSIZE / 2] = "";
    while ((bytes_read = readFromSwapFile(curproc, swap_buffer, offset, PGSIZE / 2)) != 0) {
      written_bytes = writeToSwapFile(np, swap_buffer, offset, bytes_read);
      cprintf("Coping swap file, bytes read: %d, bytes written: %d to offset: %d\n", bytes_read, written_bytes, offset);
      offset += bytes_read;
    }
  }
#endif

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}


int get_number_of_pages_in_memory(struct proc *p) {
  int i;
  int pages = 0;
  for (i = 0; i < MAX_TOTAL_PAGES; i++) {
    if(p->page_descriptions[i].status == PG_MEMORY || p->page_descriptions[i].status == PG_MUST_MEM){
      pages++;
    }
  }
  return pages;
}

int get_number_of_pages_in_swapfile(struct proc *p) {
  int i;
  int pages = 0;
  for (i = 0; i < MAX_TOTAL_PAGES; i++) {
    if(p->page_descriptions[i].status == PG_SWAPFILE){
      pages++;
    }
  }
  return pages;
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

  if(curproc == initproc)
    panic("init exiting");

  removeSwapFile(curproc);
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



#ifdef TRUE
  #if defined (LIFO) || defined (SCFIFO) || defined (LAP) || defined(AQ) || defined(LAPA)
    int number_of_pages_in_memory = get_number_of_pages_in_memory(curproc);
    int number_of_pages_in_swapfile = get_number_of_pages_in_swapfile(curproc);
    cprintf("%d %s %d %d %d %d %s \n",
            curproc->pid,
            "ZOMBIE",
            number_of_pages_in_memory,
            number_of_pages_in_swapfile,
            curproc->page_fault_counter,
            curproc->pageout_counter,
            curproc->name);
    #endif
    cprintf("%d / %d   free pages in the system\n", (system_max_free_pages - system_used_pages), system_max_free_pages);
#endif

  acquire(&ptable.lock);

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

  }
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
    int number_of_pages_in_memory = 0;
    int number_of_pages_in_swapfile = 0;
    for(i = 0; i < MAX_TOTAL_PAGES; i++){
      if(p->page_descriptions[i].status == PG_MEMORY || p->page_descriptions[i].status == PG_MUST_MEM){
        number_of_pages_in_memory++;
      }
      else if(p->page_descriptions[i].status == PG_SWAPFILE){
        number_of_pages_in_swapfile++;
      }
    }
    cprintf("%d %s %d %d %d %d %s",
            p->pid,
            state,
            number_of_pages_in_memory,
            number_of_pages_in_swapfile,
            p->page_fault_counter,
            p->pageout_counter,
            p->name);
    // up to here
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
  cprintf("%d / %d   free pages in the system\n", (system_max_free_pages - system_used_pages), system_max_free_pages);

}


// remove given link from the linked list of memory pages
void remove_page_from_linked_list(struct proc *p, int index_to_remove){

  // Update linked list by cutting the swapped out page from it.
  if (p->page_descriptions[index_to_remove].prev != 0 && p->page_descriptions[index_to_remove].next != 0) {

    // We took a link from the middle
    p->page_descriptions[index_to_remove].prev->next = p->page_descriptions[index_to_remove].next;
    p->page_descriptions[index_to_remove].next->prev = p->page_descriptions[index_to_remove].prev;
  } else if (p->page_descriptions[index_to_remove].prev == 0) {

    // We took the first link
    p->head = p->page_descriptions[index_to_remove].next;
    p->head->prev = (struct page_description *) 0;
  } else {

    // We took the last link
    p->tail = p->page_descriptions[index_to_remove].prev;
    p->tail->next = (struct page_description *) 0;
  }

  p->page_descriptions[index_to_remove].prev = (struct page_description *) 0;
  p->page_descriptions[index_to_remove].next = (struct page_description *) 0;

}

// add given link to the linked list of memory pages
void insert_page_to_linked_list(struct proc *p, int index_to_insert){
  struct proc *proc = myproc();

  // Insert new page to linked list
  // We insert page to the start of the list (Last page that was added = head. First page that was added = tail)
  if (p->tail == 0) {

    // If this is the first page to be allocated it should be the tail.
    p->tail = &proc->page_descriptions[index_to_insert];
  }
  p->page_descriptions[index_to_insert].next = proc->head;
  if (p->head != 0) {

    // if we had head before we need to update it's prev to the now new head (to the new link).
    p->head->prev = &p->page_descriptions[index_to_insert];
  }
  p->head = &p->page_descriptions[index_to_insert];
}

// This function adds a page to proc page descriptions
int
insert_page_description(struct proc *p, uint *pte, uint va) {
  int i;
  for(i = 0; i < MAX_TOTAL_PAGES; i++){
    if(p->page_descriptions[i].status == PG_UNSUED){

      p->page_descriptions[i].status = PG_MEMORY;
      p->page_descriptions[i].pte = pte;
      p->page_descriptions[i].virtual_address = va;
      p->page_descriptions[i].swap_file_offset = -1;
      p->page_descriptions[i].access_counter = 0xFFFFFFFF;
      p->page_descriptions[i].page_queue_counter = i;
      p->page_descriptions[i].aging_bol_counter = 0;
      insert_page_to_linked_list(p, i);
//      cprintf("i is: %d\n", i);
      if(i>15) {
        // Add +1 to all other in memory pages except the current index which will be the first
        for (int j = 0; j < MAX_TOTAL_PAGES; j++) {
          if (p->page_descriptions[j].status == PG_MUST_MEM) {
            continue;
          }
          if (p->page_descriptions[j].status == PG_MEMORY) {
            if (j != i) {
              p->page_descriptions[j].page_queue_counter++;
            } else {
              p->page_descriptions[j].page_queue_counter = 0;
            }
          }
        }
      }
      return 0;
    }
  }
  return 1;
}

// This functions calculate and update proc next_free_place_in_swapfile
void update_next_free_place_in_swapfile(struct proc *p){
  struct page_description *pg;
  int offset = 0;
  int free;
  for (offset = 0; offset <= PGSIZE * 15; offset = offset + PGSIZE){
    free = 1;
    for (pg = p->page_descriptions; pg < &p->page_descriptions[MAX_TOTAL_PAGES]; pg++) {
      if (pg->status == PG_SWAPFILE && pg->swap_file_offset == offset) {
        free = 0;
      }
    }
    if (free == 1){
      break;
    }
  }
  p->next_free_place_in_swapfile = offset;
}


