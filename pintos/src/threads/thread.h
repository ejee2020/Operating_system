#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h"
#include "threads/fixed-point.h"
#include "filesys/file.h"

/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */

/* The structure wait_status is for storing information of synchronization between parent and child. */
struct wait_status {
  struct lock ws_lock;                  /* Lock for locking the ref_cnt */
  struct semaphore dead;                /* Semaphore to communicate whether the child is dead */
  struct semaphore loaded;              /* Semaphore to communicate whether the child finish loading
                                           but unsure if it load it successful or not */
  bool load_success;                    /* A boolean variable to return if the child loaded successfully */
  tid_t tid;                            /* Thread identifier */
  int ref_cnt;                          /* A reference count to indicate the state of the processes.
                                           ref_cnt == 2 if parent and child both alive,
                                           ref_cnt == 1 if one of them is alive
                                           ref_cnt == 0 if both are dead */
  int exit_code;                        /* The exit code of the thread */
  struct dir* curr_dir;                 /* Thread's cwd to be passed to child */
  struct list_elem elem;                /* List element for all children's wait_status list */
};

/* The structure file_wrapper is for storing file information for file_syscall */
struct file_wrapper {
  unsigned int fd;                      /* File descriptor of the file */
  bool isdir;                           /* Indicates whether it wraps a directory or file */
  struct file *file;                    /* A pointer points to the file */
  struct dir *dir;                      /* A pointer points to the directory */
  struct list_elem elem;                /* List element for all files list in the current thread */
};

struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    uint8_t *stack;                     /* Saved stack pointer. */
    int priority;                       /* Priority. */
    struct list_elem allelem;           /* List element for all threads list. */

    struct file *exec_file;             /* The file* that the thread is currently executing */
    /* Shared between parent and child threads. */
    struct wait_status *parent_ws;      /* The wait_status that passed from parent process */
    struct list children_ws_lst;        /* The list of the wait_status of current thread's children processes */
    struct list file_wrapper_lst;       /* The list of the files that current thread contains */
    unsigned int fd_running;            /* The helper variable that keeps track of numbers of fd we used */
    struct dir *curr_dir;               /* The pointer to the current directory. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /* List element. */

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /* Page directory. */
#endif

    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
  };

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

unsigned int get_next_fd(struct thread*);
void add_wrapper(struct thread* , unsigned int , void* , bool);
void remove_wrapper(struct thread*, unsigned int);
void close_all_fd(struct thread*);
struct file_wrapper* get_file_wrapper(struct thread*, unsigned int);

struct dir* get_current_dir(void);

#endif /* threads/thread.h */
