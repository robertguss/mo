#ifndef MO_TEST_LISTENER_FAULTS_H
#define MO_TEST_LISTENER_FAULTS_H
#define _GNU_SOURCE
#include <poll.h>
#include <sys/socket.h>
#if defined(__linux__)
int mo_listener_fault_accept4(int fd, struct sockaddr *address, socklen_t *length, int flags);
#endif
int mo_listener_fault_accept(int fd, struct sockaddr *address, socklen_t *length);
int mo_listener_fault_poll(struct pollfd *descriptors, nfds_t count, int timeout);
int mo_listener_fault_bind(int fd, const struct sockaddr *address, socklen_t length);
#if defined(__linux__)
#define accept4 mo_listener_fault_accept4
#endif
#define accept mo_listener_fault_accept
#define poll mo_listener_fault_poll
#define bind mo_listener_fault_bind
#endif
