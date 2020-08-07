#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);
void split_parent_child (const char*, char*, char*);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format)
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format)
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void)
{
  flush_full_cache();                /* Since File system is done, we flush all buffer cache to the disk */
  free_map_close ();
}

 /* A helper function that parse the name into path (parent) and filename (child) */
void
split_parent_child (const char* name, char* parent, char* child)
{
  int parent_end = 0;
  int child_start = strlen(name) - 1;

  if(strlen(name) > 0 && name[0] == '/')
    ++parent_end;

  while(child_start > parent_end && name[child_start] == '/') --child_start;
  while(child_start > parent_end && name[child_start] != '/') --child_start;
  if(child_start > parent_end) ++child_start;
  parent_end = child_start;

  for(int i=0; i<parent_end; ++i) parent[i] = name[i];
  parent[parent_end] = '\0';

  for(int i=child_start; i<(int)strlen(name); ++i) child[i-child_start] = name[i];
  child[strlen(name) - child_start] = '\0';
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size, bool isdir)
{
  /* check edge case that name is equal to "" */
  if(strcmp(name, "") == 0) return false;
  block_sector_t inode_sector = 0;

  char parent[strlen(name)+2];
  char child[strlen(name)+2];
  split_parent_child(name, parent, child);

  struct dir *dir;

  /* if the parent path is "", then we just create a file at current directory */
  if(strcmp(parent, "") == 0) dir = dir_reopen(get_current_dir());
  /* else we open  the directory where the parent path is */
  else dir = dir_open(get_inode_from_path(parent));

  /* In order to success, we need to if the dir is NULL, the free_map is able to allocate, 
     inode is creatable, and directory is addable.
  */
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, isdir)
                  && dir_add (dir, child, inode_sector, isdir));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  /* check edge case that name is equal to "" */
  if(strcmp(name, "") == 0) return false;
  struct inode *inode = get_inode_from_path(name);
  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name)
{
  /* check edge case that name is equal to "", ".", and ".." that they cannot be removed */
  if(strcmp(name, "") == 0) return false;
  if(strcmp(name, ".") == 0) return false;
  if(strcmp(name, "..") == 0) return false;

  struct inode *inode = get_inode_from_path(name);
  if(inode == NULL || get_open_cnt(inode) > 2) return false;

  char parent[strlen(name)+2];
  char child[strlen(name)+2];
  split_parent_child(name, parent, child);

  struct dir *dir;
  if(strcmp(parent, "") == 0) dir = dir_reopen(get_current_dir());
  else dir = dir_open(get_inode_from_path(parent));

  bool success = dir != NULL && dir_remove (dir, child);
  dir_close (dir);

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
