#ifndef MO_TEST_LISTENER_FAULTS_H
#define MO_TEST_LISTENER_FAULTS_H
#define _GNU_SOURCE
#include <sys/socket.h>
#if defined(__linux__)
int mo_listener_fault_accept4(int fd, struct sockaddr *address, socklen_t *length, int flags);
#endif
int mo_listener_fault_accept(int fd, struct sockaddr *address, socklen_t *length);
#if defined(__linux__)
#define accept4 mo_listener_fault_accept4
#endif
#define accept mo_listener_fault_accept
#endif
