#ifndef KVS_DEBUG_H
#define KVS_DEBUG_H

#ifndef KVS_STRESS_TEST
#define KVS_STRESS_TEST 0
#endif

#if KVS_STRESS_TEST == 1
#define SCHED_POINT() kvs_sched_point()
#define SET_THREAD_STREAM(stream) kvs_set_thread_stream(stream)
#else
#define SCHED_POINT() (void)0
#define SET_THREAD_STREAM(stream) (void)0
#endif

void kvs_sched_point(void);
void kvs_set_thread_stream(unsigned long stream);

#endif
