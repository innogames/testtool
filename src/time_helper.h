//
// Testtool - Helpers for dealing with various types of time variables
//
// Copyright (c) 2021 InnoGames GmbH

#ifndef _TIME_HELPER_H_
#define _TIME_HELPER_H_

#include <sys/time.h>

int timespec_diff_ms(struct timespec *a, struct timespec *b);
// int timespec_to_ms(struct timespec *t);
int timeval_to_ms(struct timeval *t);

#endif
