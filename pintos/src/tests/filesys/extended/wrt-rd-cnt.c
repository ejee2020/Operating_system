#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"
#include "../../../threads/fixed-point.h"

void
test_main (void)
{
  const char *file_name = "test_wrt_rd_cnt";
  int fd;
  msg ("create test_wrt_rd_cnt");
  create (file_name, 10*512);

  unsigned long long prev_wrt_cnt = write_count();
  unsigned long long prev_rd_cnt = read_count();

  msg ("flush the buffer cache.");
  flush_bc();

  msg ("open test_wrt_rd_cnt");
  fd = open (file_name);

  msg ("write 100 KB to test_wrt_rd_cnt");

  char zero[512];
  memset(zero, 'a', 512);
  for (int k = 0; k < 10; k++) {
      write(fd, &zero, 512);
  }

  msg ("flush the buffer cache.");
  flush_bc();

  msg ("get write count and read count");
  unsigned long long wrt_cnt = write_count();
  unsigned long long rd_cnt = read_count();

  msg ("write count : %d", wrt_cnt - prev_wrt_cnt);
  msg ("read count : %d (should be zero)", rd_cnt - prev_rd_cnt);


  msg ("close test_wrt_rd_cnt");
  close(fd);
}
