#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"
#include "../../../threads/fixed-point.h"

void
test_main (void)
{
  const char *file_name = "test_rate";
  char zero = 0;
  int fd;
  msg ("create test_rate");
  create (file_name, 0);
  msg ("open test_rate");
  fd = open (file_name);
  
  msg ("write test_rate");
  write (fd, &zero, 1);
  msg ("flush buffer cache");
  flush_bc();
  msg ("read test_rate");
  read (fd, &zero, 1);
  msg ("calc first hit_rate");
  fixed_point_t* first_hit_rate = hit_rate();
  msg ("close test_rate");
  close (fd);
  msg ("open test_rate");
  fd = open (file_name);
  msg ("calc second hit_rate");
  fixed_point_t* second_hit_rate = hit_rate();

  msg ("compare");
  bool result = cmphitrate(first_hit_rate, second_hit_rate);
  msg ("we get result");
  if (result) {
      msg ("first < second");
  } else {
      msg ("second <= first");
  }
  msg ("close test_rate");
  close(fd);
}