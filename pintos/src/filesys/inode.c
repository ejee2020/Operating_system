#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "threads/fixed-point.h"
#include "filesys/directory.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define NUM_DIRECT 123

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
    block_sector_t direct_pointers[NUM_DIRECT];     /* Address of direct pointers */
    block_sector_t indirect_pointer;                /* Address of indirect pointer that stores of direct pointers */
    block_sector_t doubly_indirect_pointer;         /* Address of doubly direct pointer */

    bool isdir;                                     /* Boolean to indicate if data is directory or file */
    off_t length;                                   /* Total size */
    unsigned magic;                                 /* Magic number */
};

/* cache_block here is served as our buffer cache */
struct cache_block {
  block_sector_t sector_idx;                         /* Tag for Fully Associative Cache */
  uint8_t* buffer;                                   /* Data */
  bool dirty;                                        /* Dirty bit */
  struct lock block_lock;                            /* Lock for synchronization */
  struct list_elem elem;                             /* List element for LRU Cache */
};

struct cache_block* get_cache_block(struct block*, block_sector_t);
void cache_read_write (struct block*, block_sector_t, void*, bool);
void cache_read (struct block*, block_sector_t, void*);
void cache_write (struct block*, block_sector_t, void*);
bool allocate_sectors(struct inode_disk *, size_t);
void deallocate_sectors(struct inode_disk *, size_t, size_t);

int hit_count = 0;
int access_count = 0;

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct lock deny_lock;              /* A lock for updating deny_write_cnt */
    struct lock resize_lock;            /* A lock for resizing the file */
    int readers;                        /* Number of reading threads */
    int writers;                        /* Number of writing threads */
    struct lock inode_lock;             /* A lock for the writer status variables */
    struct condition waiters;           /* A condition variable for the has_writer boolean */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);

  struct inode_disk* data = malloc(sizeof(struct inode_disk));
  cache_read(fs_device, inode->sector, data);
  block_sector_t sector = -1;

  if (pos < data->length) {   /* the pos should not exceed the file's length */ 
    off_t idx = pos / BLOCK_SECTOR_SIZE;    /* convert the offset pos into block index */
    int num_pointers = BLOCK_SECTOR_SIZE / sizeof(block_sector_t);

    if (idx < NUM_DIRECT)     /* the case when idx is lie in direct pointers */
      sector = data->direct_pointers[idx];
    else if (idx < NUM_DIRECT + num_pointers) {   /* the case when idx is lie in indirect pointers */
      block_sector_t *buffer = malloc(BLOCK_SECTOR_SIZE);
      cache_read(fs_device, data->indirect_pointer, buffer);
      size_t indirect_index = idx - NUM_DIRECT;

      sector = buffer[indirect_index];
      free(buffer);
    }
    else { /* the case when idx is lie in doubly-indirect pointers */
      block_sector_t *buffer = malloc(BLOCK_SECTOR_SIZE);
      cache_read(fs_device, data->doubly_indirect_pointer, buffer);
      size_t indirect_index = (idx - NUM_DIRECT - num_pointers) / num_pointers;

      block_sector_t *indirect_buffer = malloc(BLOCK_SECTOR_SIZE);
      cache_read(fs_device, buffer[indirect_index], indirect_buffer);
      size_t doubly_indirect_index = (idx - NUM_DIRECT - num_pointers) % num_pointers;

      sector = indirect_buffer[doubly_indirect_index];
      free(indirect_buffer);
      free(buffer);
    }
  }

  free(data);
  return sector;
}

static const size_t CACHE_CAPACITY = 64;    /* There should be maximum 64 sectors */
static struct list cache_lst;               /* A list we keep track of buffer cache */
static struct lock cache_lst_lock;          /* A lock of synchronization of buffer cache */

/* Flushes all cache to disk */
void
flush_full_cache(void) {
  lock_acquire(&cache_lst_lock);
  while(!list_empty(&cache_lst)) {
    struct list_elem *e = list_begin(&cache_lst);
    struct cache_block* cb = list_entry(e, struct cache_block, elem);

    lock_acquire(&cb->block_lock);
    if (cb->dirty == true)
      block_write (fs_device, cb->sector_idx, cb->buffer);
    lock_release(&cb->block_lock);

    list_remove(e);
  }
  lock_release(&cache_lst_lock);
  access_count = 0;
  hit_count = 0;

}

/* Get a new cache block from sector sector_idx, replacing cache blocks if necessary */
struct cache_block*
get_cache_block(struct block* fs_device, block_sector_t sector_idx)
{
  /* initialize a cache block */
  struct cache_block* cb = malloc(sizeof(struct cache_block));
  cb->sector_idx = sector_idx;
  cb->buffer = malloc(BLOCK_SECTOR_SIZE);
  cb->dirty = false;
  lock_init(&cb->block_lock);
  block_read (fs_device, sector_idx, cb->buffer);

  /* Replacement Policy with LRU */
  lock_acquire(&cache_lst_lock);
  while (list_size(&cache_lst) >= CACHE_CAPACITY) {
    /* The last block is the least recently used, so we pop it out of the list */
    struct cache_block* last = list_entry(list_pop_back(&cache_lst), struct cache_block, elem); 
    lock_release(&cache_lst_lock);

    /* Check the last block is dirty or not. If it is dirty, we write it to the disk */
    lock_acquire(&last->block_lock);
    if (last->dirty == true)
      block_write (fs_device, last->sector_idx, last->buffer);
    lock_release(&last->block_lock);

    free(last->buffer);
    free(last);
    lock_acquire(&cache_lst_lock);
  }

  /* Since the new block is the most recently used/added, we out it in the front */
  list_push_front(&cache_lst, &cb->elem);
  lock_release(&cache_lst_lock);
  return cb;
}

/* Read/Write data on the cache. Might need to bring in a new cache if necessarly */
void
cache_read_write (struct block* fs_device, block_sector_t sector_idx, void* buffer, bool read)
{
  struct cache_block *cb;
  bool success = false;
  
  access_count++;
  /* Find the buffer cache that match the sector_idx */
  lock_acquire(&cache_lst_lock);
  for (struct list_elem *e = list_begin(&cache_lst); e != list_end (&cache_lst); e = list_next(e)) {
     cb = list_entry(e, struct cache_block, elem);
     if (cb->sector_idx == sector_idx) {
       hit_count++;
       success = true;
       break;
     }
  }

  if (success) { /* We found the buffer cache such that we renew it to the most recently used */
    list_remove(&cb->elem);
    list_push_front(&cache_lst, &cb->elem);
    lock_release(&cache_lst_lock);
  }
  else {    /* We did not found it, so we require a new buffer cache */
    lock_release(&cache_lst_lock);
    cb = get_cache_block(fs_device, sector_idx);
  }

  /* Read or write the data to the buffer cahce */
  lock_acquire(&cb->block_lock);
  if(read)
    memcpy(buffer, cb->buffer, BLOCK_SECTOR_SIZE);
  else {
    memcpy(cb->buffer, buffer, BLOCK_SECTOR_SIZE);
    cb->dirty = true;
  }
  lock_release(&cb->block_lock);
}

/* Read data on the cache. Might need to bring in a new cache */
void
cache_read (struct block* fs_device, block_sector_t sector_idx, void* buffer)
{
  cache_read_write (fs_device, sector_idx, buffer, true);
}

/* Write data on the cache. Might need to bring in a new cache */
void
cache_write (struct block* fs_device, block_sector_t sector_idx, void* buffer)
{
  cache_read_write (fs_device, sector_idx, buffer, false);
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
static struct lock inode_lst_lock;

/* Initializes the inode module. */
void
inode_init (void)
{
  list_init (&open_inodes);
  lock_init (&inode_lst_lock);

  list_init (&cache_lst);
  lock_init (&cache_lst_lock);
}

/* Frees the memory specified from start to end */
void
deallocate_sectors(struct inode_disk *disk_inode, size_t start, size_t end) {
  size_t num_pointers = BLOCK_SECTOR_SIZE / sizeof(block_sector_t);

  for(size_t pos_index = end; pos_index > start; --pos_index) {
    size_t index = pos_index - 1;
    /* deallocate direct pointers */
    if (index < NUM_DIRECT) {
      free_map_release(disk_inode->direct_pointers[index], 1);
      disk_inode->direct_pointers[index] = 0;
    }
    /* deallocate direct pointers for the indirect pointer */
    else if (index < NUM_DIRECT + num_pointers) {
      block_sector_t *buffer = malloc(BLOCK_SECTOR_SIZE);
      cache_read(fs_device, disk_inode->indirect_pointer, buffer);
      size_t indirect_index = index - NUM_DIRECT;

      free_map_release(buffer[indirect_index], 1);
      buffer[indirect_index] = 0;
      cache_write(fs_device, disk_inode->indirect_pointer, buffer);
      free(buffer);

      if(index == NUM_DIRECT) {
        free_map_release(disk_inode->indirect_pointer, 1);
        disk_inode->indirect_pointer = 0;
      }
    }
    /* deallocate direct pointers for the doubly indirect pointer */
    else {
      block_sector_t *buffer = malloc(BLOCK_SECTOR_SIZE);
      cache_read(fs_device, disk_inode->doubly_indirect_pointer, buffer);
      size_t indirect_index = (index - NUM_DIRECT - num_pointers) / num_pointers;

      block_sector_t *indirect_buffer = malloc(BLOCK_SECTOR_SIZE);
      cache_read(fs_device, buffer[indirect_index], indirect_buffer);
      size_t doubly_indirect_index = (index - NUM_DIRECT - num_pointers) % num_pointers;

      free_map_release(indirect_buffer[doubly_indirect_index], 1);
      indirect_buffer[doubly_indirect_index] = 0;
      cache_write(fs_device, buffer[indirect_index], indirect_buffer);
      free(indirect_buffer);

      if(index % num_pointers == NUM_DIRECT % num_pointers) {
        free_map_release(buffer[indirect_index], 1);
        buffer[indirect_index] = 0;
        cache_write(fs_device, disk_inode->doubly_indirect_pointer, buffer);
      }
      free(buffer);

      if(index == NUM_DIRECT + num_pointers) {
        free_map_release(disk_inode->doubly_indirect_pointer, 1);
        disk_inode->doubly_indirect_pointer = 0;
      }
    }
  }
}

/* Allocates memory until at least the specified end. Returns true if successful */
bool
allocate_sectors(struct inode_disk *disk_inode, size_t end) {
  bool success = true;
  static char zeros[BLOCK_SECTOR_SIZE];
  size_t num_pointers = BLOCK_SECTOR_SIZE / sizeof(block_sector_t);

  size_t start = bytes_to_sectors(disk_inode->length);
  size_t max_size = NUM_DIRECT + num_pointers + num_pointers*num_pointers;
  if(end >= max_size)
    return false;

  size_t index;
  for(index = start; index < end; ++index) {
    /* allocate direct pointers */
    if (index < NUM_DIRECT) {
      if(free_map_allocate (1, &(disk_inode->direct_pointers[index]))) {
        cache_write(fs_device, disk_inode->direct_pointers[index], zeros);
      }
      else {
        success = false;
        break;
      }
    }
    /* allocate direct pointers for the indirect pointer */
    else if (index < NUM_DIRECT + num_pointers) {
      if(index == NUM_DIRECT) {
        if(free_map_allocate (1, &disk_inode->indirect_pointer)) {
          cache_write(fs_device, disk_inode->indirect_pointer, zeros);
        }
        else {
          success = false;
          break;
        }
      }

      block_sector_t *buffer = malloc(BLOCK_SECTOR_SIZE);
      cache_read(fs_device, disk_inode->indirect_pointer, buffer);
      size_t indirect_index = index - NUM_DIRECT;

      if(free_map_allocate (1, &(buffer[indirect_index]))) {
        cache_write(fs_device, buffer[indirect_index], zeros);
        cache_write(fs_device, disk_inode->indirect_pointer, buffer);
        free(buffer);
      }
      else {
        if(index == NUM_DIRECT) {
          free_map_release(disk_inode->indirect_pointer, 1);
          disk_inode->indirect_pointer = 0;
        }
        success = false;
        free(buffer);
        break;
      }
    }
    /* allocate direct pointers for the doubly indirect pointer */
    else {
      if(index == NUM_DIRECT + num_pointers) {
        if(free_map_allocate(1, &disk_inode->doubly_indirect_pointer)) {
          cache_write(fs_device, disk_inode->doubly_indirect_pointer, zeros);
        }
        else {
          success = false;
          break;
        }
      }

      if(index % num_pointers == NUM_DIRECT % num_pointers) {
        block_sector_t *buffer = malloc(BLOCK_SECTOR_SIZE);
        cache_read(fs_device, disk_inode->doubly_indirect_pointer, buffer);
        size_t indirect_index = (index - NUM_DIRECT - num_pointers) / num_pointers;

        if(free_map_allocate(1, &buffer[indirect_index])) {
          cache_write(fs_device, buffer[indirect_index], zeros);
          cache_write(fs_device, disk_inode->doubly_indirect_pointer, buffer);
          free(buffer);
        }
        else {
          if(index == NUM_DIRECT + num_pointers) {
            free_map_release(disk_inode->doubly_indirect_pointer, 1);
            disk_inode->doubly_indirect_pointer = 0;
          }
          success = false;
          free(buffer);
          break;
        }
      }

      block_sector_t *buffer = malloc(BLOCK_SECTOR_SIZE);
      cache_read(fs_device, disk_inode->doubly_indirect_pointer, buffer);
      size_t indirect_index = (index - NUM_DIRECT - num_pointers) / num_pointers;

      block_sector_t *indirect_buffer = malloc(BLOCK_SECTOR_SIZE);
      cache_read(fs_device, buffer[indirect_index], indirect_buffer);
      size_t doubly_indirect_index = (index - NUM_DIRECT - num_pointers) % num_pointers;

      if(free_map_allocate (1, &indirect_buffer[doubly_indirect_index])) {
        cache_write(fs_device, indirect_buffer[doubly_indirect_index], zeros);
        cache_write(fs_device, buffer[indirect_index], indirect_buffer);
        free(indirect_buffer);
        free(buffer);
      }
      else {
        if(index % num_pointers == NUM_DIRECT % num_pointers) {
          free_map_release(buffer[indirect_index], 1);
          buffer[indirect_index] = 0;
          cache_write(fs_device, disk_inode->doubly_indirect_pointer, buffer);
        }

        if(index == NUM_DIRECT + num_pointers) {
          free_map_release(disk_inode->doubly_indirect_pointer, 1);
          disk_inode->doubly_indirect_pointer = 0;
        }

        free(indirect_buffer);
        free(buffer);
        success = false;
        break;
      }
    }
  }

  if(!success)
    deallocate_sectors(disk_inode, start, index);
  return success;
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool isdir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->magic = INODE_MAGIC;
      disk_inode->length = 0;
      disk_inode->isdir = isdir;

      if(allocate_sectors(disk_inode, sectors)) {
        disk_inode->length = length;
        cache_write (fs_device, sector, disk_inode);
        success = true;
      }
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  lock_acquire(&inode_lst_lock);
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          lock_release(&inode_lst_lock);
          return inode;
        }
    }

  /* The inode has not opened yet */
  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  lock_release(&inode_lst_lock);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  inode->readers = 0;
  inode->writers = 0;
  lock_init (&inode->deny_lock);
  lock_init (&inode->resize_lock);
  lock_init (&inode->inode_lock);
  cond_init (&inode->waiters);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      lock_acquire(&inode_lst_lock);
      list_remove (&inode->elem);
      lock_release(&inode_lst_lock);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          struct inode_disk* data = malloc(sizeof(struct inode_disk));
          cache_read(fs_device, inode->sector, data);
          deallocate_sectors(data, 0, bytes_to_sectors (inode_length(inode)));
          free(data);

          free_map_release (inode->sector, 1);
        }

      free (inode);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  if(inode_length(inode) <= offset) return 0;

  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  /* synchronization */
  lock_acquire(&inode->inode_lock);
  while(inode->writers > 0)
    cond_wait(&inode->waiters, &inode->inode_lock);
  ++inode->readers;
  lock_release(&inode->inode_lock);

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          cache_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          cache_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  lock_acquire(&inode->inode_lock);
  --inode->readers;
  cond_broadcast(&inode->waiters, &inode->inode_lock);
  lock_release(&inode->inode_lock);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset)
{
  /* Check if someone is writing */
  lock_acquire(&inode->deny_lock);
  if(inode->deny_write_cnt) {
    lock_release(&inode->deny_lock);
    return 0;
  }
  lock_release(&inode->deny_lock);

  if(size == 0) return 0;

  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  /* synchronization */
  lock_acquire(&inode->inode_lock);
  while(inode->readers > 0 || inode->writers > 0)
    cond_wait(&inode->waiters, &inode->inode_lock);
  ++inode->writers;
  lock_release(&inode->inode_lock);

  struct inode_disk* data = malloc(sizeof(struct inode_disk));
  cache_read(fs_device, inode->sector, data);

  off_t last_write = offset + size;
  if(!allocate_sectors(data, bytes_to_sectors(last_write)))
    return 0;

  lock_acquire(&inode->resize_lock);
  if(last_write > data->length)
    data->length = last_write;
  lock_release(&inode->resize_lock);

  cache_write(fs_device, inode->sector, data);
  free(data);

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          cache_write (fs_device, sector_idx, (void *)(buffer + bytes_written));
        }
      else
        {
          /* We need a bounce buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left)
            cache_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          cache_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  lock_acquire(&inode->inode_lock);
  --inode->writers;
  cond_broadcast(&inode->waiters, &inode->inode_lock);
  lock_release(&inode->inode_lock);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode)
{
  lock_acquire(&inode->deny_lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release(&inode->deny_lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode)
{
  lock_acquire(&inode->deny_lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release(&inode->deny_lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (struct inode *inode)
{
  lock_acquire(&inode->resize_lock);

  struct inode_disk* data = malloc(sizeof(struct inode_disk));
  cache_read(fs_device, inode->sector, data);
  off_t size = data->length;

  free(data);
  lock_release(&inode->resize_lock);
  return size;
}

/* Returns true if inode is directory type */
bool
inode_isdir(struct inode *inode)
{
  struct inode_disk disk;
  cache_read(fs_device, inode->sector, &disk);
  return disk.isdir;
}

/* Returns open_cnt of inode */
int
get_open_cnt (struct inode* inode)
{
  return inode->open_cnt;
}

/* Extracts a file name part from *SRCP into PART, and updates *SRCP so that the
next call will return the next file name part. Returns 1 if successful, 0 at
end of string, -1 for a too-long file name part. */
static int
get_next_part (char* part, const char **srcp) {
  const char *src = *srcp;
  char *dst = part;

  /* Skip leading slashes. If it’s all slashes, we’re done. */
  while (*src == '/')
    src++;
  if (*src == '\0')
    return 0;

  /* Copy up to NAME_MAX character from SRC to DST. Add null terminator. */
  while (*src != '/' && *src != '\0') {
    *dst++ = *src;
    src++;
  }
  *dst = '\0';

  /* Advance source pointer. */
  *srcp = src;
  return 1;
}

/* Retrieves an inode from the given the path. Returns NULL if there is an error */
struct inode *
get_inode_from_path(const char *path) {
  struct dir *cur;
  /* determine the path is relative or absolute */
  if (path[0] == '/' || thread_current()->curr_dir == NULL)
    cur = dir_open_root();
  else
    cur = dir_reopen(get_current_dir());

  
  char part[strlen(path)+1];
  const char **ptr;
  ptr = &path;

  /* Parse the path and traverse to the desired directory */
  while (get_next_part(part, ptr)) {
    struct inode *inode = NULL;
    if(cur == NULL)
      return NULL;

    if(strcmp(part, ".") == 0)
      inode = inode_reopen(dir_get_inode(cur));
    else if(strcmp(part, "..") == 0)
      inode = get_parent_inode(cur);
    else if (!dir_lookup(cur, part, &inode)) {
      dir_close(cur);
      return NULL;
    }

    dir_close(cur);
    if(inode->removed)
      return NULL;
    cur = dir_open(inode);
  }

  struct inode* inode = dir_get_inode(cur);
  free(cur);
  return inode;
}

fixed_point_t* hit_rate(void) {
  fixed_point_t* rate = malloc(sizeof(fixed_point_t));
  if (rate == NULL) return NULL;
  if (access_count == 0) {
    *rate = fix_int(0);
  } else{ 
    *rate = fix_div(fix_int(hit_count), fix_int(access_count));
  }
  return rate;
}

bool compare_rate (fixed_point_t first, fixed_point_t second) {
  if (first.f < second.f) {
    return true;
  } else {
    return false;
  }
}