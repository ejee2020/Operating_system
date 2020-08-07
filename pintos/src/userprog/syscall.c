#include "userprog/syscall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/inode.h"
#include "devices/shutdown.h"
#include "filesys/directory.h"
#include "filesys/file.h"

/* The following definition is used to exit the thread. */
#define exit() {\
  printf ("%s: exit(%d)\n", (char *)(&thread_current()->name), f->eax);\
  close_all_fd(thread_current());\
  thread_exit();\
  return;\
}

/* The following definition is used to exit the thread that causes an error. */
#define exit_bad() {\
  f->eax = -1;\
  exit();\
}

/* Checks validity of args[num1], ..., args[num2] */
#define check_args(num1, num2) {\
  if(!is_valid_void_ptr(args+num1, (num2-num1+1)*sizeof(uint32_t)))\
    exit_bad();\
}

static bool is_valid_void_ptr(void*, size_t);
static bool is_valid_char_ptr(char*);
static void syscall_handler (struct intr_frame *);
struct dir* readdir(int);

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* This function checks whether the passing void pointer with the given size is valid */
bool
is_valid_void_ptr(void* ptr, size_t size)
{
  if(ptr == NULL) return false;

  uint32_t* pd = thread_current()->pagedir;
  for(size_t i=0; i<size; ++i) {
    if(!is_user_vaddr(ptr) || pagedir_get_page(pd, ptr) == NULL) return false;
    ++ptr;
  }
  return true;
}

/* This function checks whether the passing char pointer is valid. */
bool
is_valid_char_ptr(char* ptr)
{
  if(ptr == NULL) return false;

  uint32_t* pd = thread_current()->pagedir;
  /* Repeatedly checks the ptr values until we reach a \0 or invalid ptrs. */
  while(true) {
    if(!is_user_vaddr(ptr)) return false;
    char* value = (char*) pagedir_get_page(pd, ptr);
    if(value == NULL) return false;
    if(*value == '\0') break;
    ++ptr;
  }
  return true;
}

/* This function takes in an fd and returns the dir pointer if possible */
struct dir*
readdir(int fd) {
  struct file_wrapper *ptr = get_file_wrapper(thread_current(), fd);
  if (ptr == NULL)
    return NULL;
  if(!ptr->isdir)
    return NULL;
  return ptr->dir;
}

static void
syscall_handler (struct intr_frame *f UNUSED)
{
  uint32_t* args = ((uint32_t*) f->esp);
  check_args(0, 0);

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  /* printf("System call number: %d\n", args[0]); */

  /* syscall for practice */
  if (args[0] == SYS_PRACTICE) {
    check_args(1, 1);
    f->eax = args[1]+1;
  }

  /* syscall for halt */
  if (args[0] == SYS_HALT) {
    shutdown_power_off();
  }

  /* syscall for exit */
  if (args[0] == SYS_EXIT) {
    check_args(1, 1);
    close_all_fd(thread_current());
    f->eax = args[1];
    update_ws_exit(args[1]);
    exit();
  }

  /* syscall for create */
  if (args[0] == SYS_CREATE) {
    check_args(1, 2);
    char* filename = (char*) args[1];
    /* If the char pointer is not valid, call exit_bad */
    if(!is_valid_char_ptr(filename)) {
      exit_bad();
    }
    /* If the char pointer is valid, we call filesys_create function with proper arguments */
    else
      f->eax = filesys_create (filename, args[2], false);
  }

  /* syscall for inumber */
  if (args[0] == SYS_INUMBER) {
    check_args(1, 1);
    unsigned int fd = args[1];
    struct file_wrapper* ptr = get_file_wrapper(thread_current(), fd);
    if(ptr == NULL)
      f->eax = -1;
    else {
      if(ptr->isdir)
        f->eax = inode_get_inumber(dir_get_inode(ptr->dir));
      else
        f->eax = inode_get_inumber(file_get_inode(ptr->file));
    }
  }

  /* syscall for removing a file */
  if (args[0] == SYS_REMOVE) {
    check_args(1, 1);
    char* filename = (char*) args[1];
    /* If the char pointer is not valid call exit_bad */
    if(!is_valid_char_ptr(filename)) {
      exit_bad();
    }
    /* If the char pointer is valid remove a file */
     else
      f->eax = filesys_remove (filename);
  }

  /* syscall for opening a file */
  if (args[0] == SYS_OPEN) {
    check_args(1, 1);
    char* filename = (char*) args[1];

    /* If the char pointer is not valid call exit_bad */
    if(!is_valid_char_ptr(filename)) {
      exit_bad();
    }
    /* If the char pointer is valid open a file */
    else {
      struct file* file = filesys_open(filename);
      if (file == NULL)
        f->eax = -1;
      else {
        struct inode* inode = file_get_inode(file);
        if(inode == NULL)
          f->eax = -1;
        else if(inode_isdir(inode)) {
          struct dir *dir = dir_open(inode_reopen(inode));
          if(dir == NULL)
            f->eax = -1;
          else {
            unsigned int fd = get_next_fd(thread_current());
            add_wrapper(thread_current(), fd, dir, true);
            f->eax = fd;
          }
        }
        else {
          struct file *file = file_open(inode_reopen(inode));
          if(file == NULL)
            f->eax = -1;
          else {
            unsigned int fd = get_next_fd(thread_current());
            add_wrapper(thread_current(), fd, file, false);
            f->eax = fd;
          }
        }
        file_close(file);
      }
    }
  }

  /* syscall for getting the size of the file */
  if (args[0] == SYS_FILESIZE) {
    check_args(1, 1);
    struct file_wrapper* ptr = get_file_wrapper(thread_current(), (int) args[1]);
    /* If get_file fails, return 0 */
    if(ptr == NULL)
      f->eax = 0;
    /* If get_file succeeds, return the size of the file */
    else {
      if(ptr->isdir)
        f->eax = inode_length(dir_get_inode(ptr->dir));
      else
        f->eax = inode_length(file_get_inode(ptr->file));
    }
  }

  /* syscall for reading a file */
  if (args[0] == SYS_READ) {
    check_args(1, 3);
    /* If the pointer is not valid, call exit_bad */
    if(!is_valid_void_ptr((void *) args[2], (size_t) args[3])) {
      exit_bad();
    }

    /* If the args[1] is stdin, do the following */
    if(args[1] == 0) {
      char* buffer = (char*) args[2];
      unsigned int length = 0;
      char ch = input_getc();
      while(length < args[3] && ch != -1) {
        buffer[length] = ch;
        ++length;
        ch = input_getc();
      }
      f->eax = length;
    }
    /* Reading a file not stdin */
    else {
      struct file_wrapper* ptr = get_file_wrapper(thread_current(), (unsigned int) args[1]);
      /* If get_file fails, return -1 */
      if(ptr == NULL)
        f->eax = -1;
      /* If get_file succeeds, call file_read */
      else {
        if (ptr->isdir)
          f->eax = -1;
        else
          f->eax = file_read(ptr->file, (void *)args[2], (off_t) args[3]);
      }
    }
  }

  /* syscall for writing */
  if (args[0] == SYS_WRITE) {
    check_args(1, 3);
    /* If args[1] is stdout call putbuf */
    if(args[1] == 1)
      putbuf((char *)args[2], (size_t)args[3]);
    /* Writing a file not to stdout */
    else {
      struct file_wrapper* ptr = get_file_wrapper(thread_current(), (unsigned int) args[1]);
      /* If the pointer is not valid, call exit_bad */
      if(!is_valid_void_ptr((void *) args[2], (size_t) args[3])) {
        exit_bad();
      }
      /* If the pointer does not exist then return 0 */
      else if(ptr == NULL)
        f->eax = 0;
      /* Otherwise, call file_write appopriately */
      else {
        if (ptr->isdir)
          f->eax = -1;
        else
          f->eax = file_write(ptr->file, (void *)args[2], (off_t) args[3]);
      }
    }
  }

  /* syscall for seek */
  if (args[0] == SYS_SEEK) {
    check_args(1, 2);
    struct file_wrapper* ptr = get_file_wrapper(thread_current(), (unsigned int) args[1]);
    /* If get_file succeeds, call file_seek */
    if(ptr != NULL) {
      if (ptr->isdir)
        f->eax = -1;
      else
        file_seek(ptr->file, (off_t) args[2]);
    }
  }

  /* syscall for tell */
  if (args[0] == SYS_TELL) {
    check_args(1, 1);
    struct file_wrapper* ptr = get_file_wrapper(thread_current(), (unsigned int) args[1]);
    /* If get_file fails, return 0 */
    if(ptr == NULL)
      f->eax = 0;
    /* If get_file succeeds, call file_tell */
    else {
      if (ptr->isdir)
        f->eax = -1;
      else
        f->eax = file_tell(ptr->file);
    }
  }

  /* syscall for close */
  if (args[0] == SYS_CLOSE) {
    check_args(1, 1);
    struct file_wrapper* wrapper = get_file_wrapper(thread_current(), args[1]);
    if (wrapper !=  NULL) {
      if(wrapper->isdir)
        dir_close(wrapper->dir);
      else
        file_close(wrapper->file);
      remove_wrapper(thread_current(), args[1]);
    }
  }

  /* syscall for wait */
  if (args[0] == SYS_WAIT) {
    check_args(1, 1);
    tid_t pid = args[1];
    /* call process_wait with associated pid */
    f->eax = process_wait(pid);
  }

  /* syscall for exec */
  if (args[0] == SYS_EXEC) {
    check_args(1, 1);
    char* filename = (char *) args[1];
    /* If the pointer is not valid, call exit_bad */
    if(!is_valid_char_ptr(filename)) {
      exit_bad();
    }
    /* If the pointer is valid, call process_execute to get the pid and return it */
    else {
      tid_t pid = process_execute(filename);
      f->eax = pid;
    }
  }

  /* syscall for chdir */
  if (args[0] == SYS_CHDIR)
  {
    check_args(1, 1);
    char* path = (char *) args[1];
    /* Check the path is a valid char pointer */
    if(!is_valid_char_ptr(path)) {
      exit_bad();
    }
    /* Get the inode from the path */
    struct inode *inode = get_inode_from_path(path);
    /* If the indoe is not found or is not directory, then return false */
    if (inode == NULL || !inode_isdir(inode)) 
      f->eax = false;
    else {
      /* Open the desired directory from the node and set current directory to it */
      struct dir *dir = dir_open(inode);
      dir_close(thread_current()->curr_dir);
      thread_current()->curr_dir = dir;
      f->eax =true;
    }
  }

  /* syscall for mkdir */
  if (args[0] == SYS_MKDIR)
  {
    check_args(1, 1);
    char* path = (char *) args[1];
    /* Check the path is a valid char pointer */
    if(!is_valid_char_ptr(path)) {
      exit_bad();
    }

    if(strlen(path) == 0)
      f->eax = false;
    else {
      /* If the path is available, then create the directory. 
         Note that the third argument is a boolean for checking  
         The file system is creating a directory or a file
      */
      if (get_inode_from_path(path) == NULL)
        f->eax = filesys_create(path, 64, true);
      else
        f->eax = false;
    }
  }

  /* syscall for readdir */
  if (args[0] == SYS_READDIR)
  {
    check_args(1, 2);
    void* buffer = (void *)args[2];
    /* Check the path is a valid char pointer */
    if(!is_valid_void_ptr(buffer, NAME_MAX+1)) {
      exit_bad();
    }
    
    /* Call the helper function readdir to get the directory */
    struct dir* new_dir = readdir((int) args[1]);
    if (new_dir == NULL)
      f->eax = false;
    else
      f->eax = dir_readdir(new_dir, (char*)args[2]);
  }

  /* syscall for isdir */
  if (args[0] == SYS_ISDIR)
  {
    check_args(1, 1);
    unsigned int fd = args[1];
    struct file_wrapper* ptr = get_file_wrapper(thread_current(), fd);
    if(ptr == NULL)
      f->eax = -1;
    else
      f->eax = ptr->isdir;
  }

  if (args[0] == SYS_HITRATE) {
    f->eax = hit_rate();
  }

  if (args[0] == SYS_FLUSHBC) {
    flush_full_cache();
  }

  if (args[0] == SYS_CMPHIT) {
    check_args(1, 2);
    fixed_point_t* first = args[1];
    fixed_point_t* second = args[2];
    if (first == NULL || second == NULL) {
      exit_bad();
    }
    f->eax = compare_rate(*first, *second);
  }

  if (args[0] == SYS_WRCNT) {
    check_args(1, 1);
    if (!fs_device) {
      exit_bad();
    }
    f->eax = get_write_cnt(fs_device);
  }

  if (args[0] == SYS_RDCNT) {
    check_args(1, 1);
    if (!fs_device) {
      exit_bad();
    }
    f->eax = get_read_cnt(fs_device);
  }
}
