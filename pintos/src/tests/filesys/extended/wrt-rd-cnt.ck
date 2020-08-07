# -*- perl -*-
use strict;
use warnings;
use tests::tests;
use tests::random;
check_expected (IGNORE_EXIT_CODES => 1, [<<'EOF']);
(wrt-rd-cnt) begin
(wrt-rd-cnt) create test_wrt_rd_cnt
(wrt-rd-cnt) open test_wrt_rd_cnt
(wrt-rd-cnt) write 100 KB to test_wrt_rd_cnt
(wrt-rd-cnt) write count : 20
(wrt-rd-cnt) read count : 0 (should be zero)
(wrt-rd-cnt) close test_wrt_rd_cnt
(wrt-rd-cnt) end
EOF
pass;