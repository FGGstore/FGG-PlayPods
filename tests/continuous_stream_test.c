/* Actual unchanged stream loop with accelerated clock and synthetic capture. */
#define A2DP_TEST_MAX_LIMIT_MS 600000
#define main prior_deadline_fixture
#include "stream_deadline_test.c"
#undef main
int main(void)
{
    reset(0);read_delay=100;link_drop_at=650000;
    assert(a2dp_stream(1));
    assert(clock_ms>=650000 && clock_ms<=650100 && send_calls>1000 && finish_calls==1 && !duration_logs);
    reset(0);credit=0;read_delay=100;link_drop_at=650000;
    assert(!a2dp_stream(1));
    assert(clock_ms>=650000 && !send_calls && finish_calls==1 && !duration_logs);
    reset(0);continuous_capture=1;read_delay=100;link_drop_at=650000;
    assert(a2dp_stream(1));
    assert(clock_ms>=650000 && clock_ms<=650100 && read_calls>0 && poll_calls>0 &&
           send_calls>0 && finish_calls==1 && !duration_logs);
    puts("Continuous stream passes ten minutes and still stops on loss during media, credit stall and endless capture (mocked)");
    return 0;
}
