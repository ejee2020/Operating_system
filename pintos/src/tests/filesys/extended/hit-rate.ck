# -*- perl -*-
use strict;
use warnings;
use tests::tests;
use tests::random;
check_expected (IGNORE_EXIT_CODES => 1, [<<'EOF']);
(hit-rate) begin
(hit-rate) create test_rate
(hit-rate) open test_rate
(hit-rate) write test_rate
(hit-rate) flush buffer cache
(hit-rate) read test_rate
(hit-rate) calc first hit_rate
(hit-rate) close test_rate
(hit-rate) open test_rate
(hit-rate) calc second hit_rate
(hit-rate) compare
(hit-rate) we get result
(hit-rate) first < second
(hit-rate) close test_rate
(hit-rate) end
EOF
pass;