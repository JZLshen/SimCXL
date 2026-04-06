#ifndef RPC_FIRST_ROUND_BARRIER_H
#define RPC_FIRST_ROUND_BARRIER_H

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define RPC_FIRST_ROUND_BARRIER_ENV "CXL_RPC_FIRST_ROUND_BARRIER_FILE"

typedef struct {
    uint64_t arrived_clients;
    uint64_t release_flag;
} rpc_first_round_barrier_state_t;

static inline void
rpc_first_round_barrier_pause(int pause_iters)
{
    int effective_pause_iters = (pause_iters > 0) ? pause_iters : 64;

    for (int i = 0; i < effective_pause_iters; i++)
        __asm__ __volatile__("pause" ::: "memory");
}

static inline int
rpc_wait_for_first_round_barrier(int participant_count,
                                 int node_id,
                                 volatile int *keep_running,
                                 int pause_iters)
{
    const char *path = NULL;
    rpc_first_round_barrier_state_t *state = NULL;
    struct stat st;
    int fd = -1;

    if (participant_count <= 1)
        return 0;

    path = getenv(RPC_FIRST_ROUND_BARRIER_ENV);
    if (!path || path[0] == '\0') {
        fprintf(stderr,
                "client %d: missing %s for first-round barrier\n",
                node_id,
                RPC_FIRST_ROUND_BARRIER_ENV);
        return -1;
    }

    fd = open(path, O_RDWR);
    if (fd < 0) {
        perror("client: open first-round barrier");
        return -1;
    }

    if (fstat(fd, &st) != 0) {
        perror("client: stat first-round barrier");
        close(fd);
        return -1;
    }

    if (st.st_size < (off_t)sizeof(*state)) {
        fprintf(stderr,
                "client %d: first-round barrier file too small: %s\n",
                node_id,
                path);
        close(fd);
        return -1;
    }

    state = (rpc_first_round_barrier_state_t *)mmap(NULL,
                                                    sizeof(*state),
                                                    PROT_READ | PROT_WRITE,
                                                    MAP_SHARED,
                                                    fd,
                                                    0);
    if (state == MAP_FAILED) {
        perror("client: mmap first-round barrier");
        close(fd);
        return -1;
    }

    if (__atomic_add_fetch(&state->arrived_clients, 1, __ATOMIC_ACQ_REL) ==
        (uint64_t)participant_count) {
        __atomic_store_n(&state->release_flag, 1, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&state->release_flag, __ATOMIC_ACQUIRE) == 0) {
            if (keep_running && !(*keep_running)) {
                munmap(state, sizeof(*state));
                close(fd);
                return -1;
            }
            rpc_first_round_barrier_pause(pause_iters);
        }
    }

    munmap(state, sizeof(*state));
    close(fd);
    return 0;
}

#endif
