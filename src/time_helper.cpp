#include <sys/time.h>

int timespec_diff_ms(struct timespec *a, struct timespec *b) {
  time_t diff_sec = a->tv_sec - b->tv_sec;
  long diff_nsec = a->tv_nsec - b->tv_nsec;
  if (diff_nsec < 0) {
    diff_sec--;
    diff_nsec += 1000000000;
  }
  long diff_msec = (diff_sec * 1000) + (diff_nsec / 1000000);
  return diff_msec;
}

// long timespec_to_ms(struct timespec *t) {
//  return (t->tv_sec * 1000) + (t->tv_nsec / 1000000);
//}

long timeval_to_ms(struct timeval *t) {
  return (t->tv_sec * 1000) + (t->tv_usec / 1000);
}
