CC = gcc
# Static linking to avoid GLIBC version mismatch between host and guest
# Add -g for GDB debugging symbols
CFLAGS = -O2 -Wall -static -g -I../lib/libcxlrpc/include -I../../../include
LIBCXLRPC = ../lib/libcxlrpc/libcxlrpc.a
M5_LIB = ../../../util/m5/build/x86/out/libm5.a
LIBS = -lnuma $(M5_LIB)
# Keep libcxlrpc's own include path/flags local to its directory so top-level
# CFLAGS overrides for this Makefile don't leak into the sub-build.
LIBCXLRPC_CFLAGS ?= -O2 -Wall -Wextra -I./include -I../../../../include
CE_SANITY_LIBCXLRPC_CFLAGS = $(LIBCXLRPC_CFLAGS) -DCXL_RPC_TEST_FORCE_COPYENGINE=1

TARGETS = rpc_client_example \
          rpc_server_example \
          cxl_mem_ldst_latency \
          cxl_mem_paper_latency \
          cxl_mem_copy_cmp \
          cxl_copyengine_bw \
          cpu_memmove_bw

ifneq ($(CXL_RPC_INCLUDE_CE_SANITY),)
TARGETS += cxl_copyengine_sanity
endif

LEGACY_CLEAN_PATTERNS = cxl_rpc_benchmark \
	                        cxl_rpc_send_poll_threads \
	                        cxl_rpc_poll_* \
	                        cxl_rpc_send_response_* \
	                        cxl_rpc_store_pair

all: libcxlrpc $(TARGETS)

libcxlrpc:
	$(MAKE) -C ../lib/libcxlrpc all CFLAGS="$(LIBCXLRPC_CFLAGS)"

rpc_client_example: src/rpc_client_example.c libcxlrpc
	$(CC) $(CFLAGS) -o $@ $< $(LIBCXLRPC) $(LIBS)
rpc_server_example: src/rpc_server_example.c libcxlrpc
	$(CC) $(CFLAGS) -o $@ $< $(LIBCXLRPC) $(LIBS)

cxl_mem_ldst_latency: src/cxl_mem_ldst_latency.c libcxlrpc
	$(CC) $(CFLAGS) -o $@ $< $(LIBCXLRPC) $(LIBS)
cxl_mem_paper_latency: src/cxl_mem_paper_latency.c libcxlrpc
	$(CC) $(CFLAGS) -o $@ $< $(LIBCXLRPC) $(LIBS)
cxl_mem_copy_cmp: src/cxl_mem_copy_cmp.c libcxlrpc
	$(CC) $(CFLAGS) -o $@ $< $(LIBCXLRPC) $(LIBS)
cxl_copyengine_bw: src/cxl_copyengine_bw.c libcxlrpc
	$(CC) $(CFLAGS) -o $@ $< $(LIBCXLRPC) $(LIBS)
cpu_memmove_bw: src/cpu_memmove_bw.c libcxlrpc
	$(CC) $(CFLAGS) -pthread -o $@ $< $(LIBCXLRPC) $(LIBS)

cxl_copyengine_sanity: src/cxl_copyengine_sanity.c
	$(MAKE) -C ../lib/libcxlrpc clean all CFLAGS="$(CE_SANITY_LIBCXLRPC_CFLAGS)"
	$(CC) $(CFLAGS) -o $@ $< $(LIBCXLRPC) $(LIBS)
clean:
	rm -f $(TARGETS) $(LEGACY_CLEAN_PATTERNS)

.PHONY: all clean libcxlrpc
