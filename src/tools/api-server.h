#ifndef MO_TOOLS_API_SERVER_H
#define MO_TOOLS_API_SERVER_H

#include "tools/tools.h"
#include "vm/vm.h"

#include <pthread.h>

MO_MUST_USE bool mo_serve_maybe_api(const struct mo_cli *cli, FILE *output, FILE *errors,
                                    int *status_out);

/* Compiles a single-file Mo program declaring an api block into an initialized
 * VM image and resolves its generated wire entry point and actor descriptors. */
MO_MUST_USE int mo_api_compile_file(const struct mo_cli *cli, FILE *output, FILE *errors,
                                    struct mo_image **image_out, uint32_t *wire_fn_out,
                                    uint16_t *default_port_out,
                                    uint32_t actor_services[MO_LIMIT_API_ACTORS],
                                    uint32_t *actor_count_out);

/* Serves a single HTTP/1.1 request on client_fd (closing client_fd before
 * returning) under monotonic read_timeout_us and write_timeout_us deadlines. */
void mo_api_serve_fd(const struct mo_image *image, uint32_t wire_fn, int folder_fd, int output_fd,
                     int errors_fd, FILE *errors, pthread_mutex_t *log_mutex,
                     const struct mo_limits *limits, struct mo_live_actor *const *actors,
                     uint32_t actor_count, uint64_t read_timeout_us, uint64_t write_timeout_us,
                     int client_fd);

/* Forces static asset path confinement onto the portable O_NOFOLLOW resolver (for C tests). */
void mo_api_set_force_fallback(bool force);

#endif
