#ifndef CXL_RPC_LAYOUT_H
#define CXL_RPC_LAYOUT_H

#include <stdint.h>

/*
 * Public RPC shared-memory layout.
 *
 * Region 0 is the server region. Regions [1, TOTAL_REGIONS) are per-client
 * regions. The doorbell area at the front of the server region is sized to
 * cover one reserved slot0 plus one 64B slot for every logical client region.
 *
 * Keep this file in sync with the board-side RPC layout in x86_board.py.
 */

#define CXL_BASE 0x100000000ULL
#define CXL_SIZE 0x200000000ULL

#define CLIENT_REGION_SIZE 0x02000000ULL
#define TOTAL_REGIONS (CXL_SIZE / CLIENT_REGION_SIZE)
#define SERVER_REGION_INDEX 0ULL
#define SERVER_REGION_BASE (CXL_BASE + SERVER_REGION_INDEX * CLIENT_REGION_SIZE)
#define MAX_CLIENTS ((int)(TOTAL_REGIONS - 1ULL))

#define DOORBELL_OFFSET 0x00000000ULL
#define DOORBELL_STRIDE 0x00000040ULL
#define RESERVED_DOORBELL_SLOTS 1ULL

#define CXL_RPC_ALIGN_UP(value, align) \
    ((((uint64_t)(value)) + ((uint64_t)(align) - 1ULL)) & \
     ~((uint64_t)(align) - 1ULL))

#define DOORBELL_REGION_BYTES \
    CXL_RPC_ALIGN_UP( \
        (((uint64_t)MAX_CLIENTS + RESERVED_DOORBELL_SLOTS) * DOORBELL_STRIDE), \
        0x1000ULL)

#define METADATA_Q_OFFSET DOORBELL_REGION_BYTES
#define METADATA_Q_ENTRIES 1024U
#define METADATA_Q_SIZE_BYTES ((uint64_t)METADATA_Q_ENTRIES * 16ULL)

#define REQUEST_DATA_BYTES (10ULL * 1024ULL * 1024ULL)
#define RESPONSE_DATA_BYTES (10ULL * 1024ULL * 1024ULL)

#define REQUEST_DATA_OFFSET \
    CXL_RPC_ALIGN_UP(METADATA_Q_OFFSET + METADATA_Q_SIZE_BYTES, 0x1000ULL)
#define RESPONSE_DATA_OFFSET \
    CXL_RPC_ALIGN_UP(REQUEST_DATA_OFFSET + REQUEST_DATA_BYTES, 0x1000ULL)
#define FLAG_OFFSET \
    CXL_RPC_ALIGN_UP(RESPONSE_DATA_OFFSET + RESPONSE_DATA_BYTES, 0x1000ULL)

_Static_assert(DOORBELL_REGION_BYTES >=
                   (((uint64_t)MAX_CLIENTS + RESERVED_DOORBELL_SLOTS) *
                    DOORBELL_STRIDE),
               "Doorbell region must cover slot0 plus every logical client");
_Static_assert(FLAG_OFFSET + 64ULL <= CLIENT_REGION_SIZE,
               "RPC shared-memory layout must fit inside one client region");

#endif
