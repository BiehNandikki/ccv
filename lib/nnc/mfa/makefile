include ../../config.mk

CFLAGS := -std=c++17 -O3 -Wall -I"../../" $(CFLAGS)

SRCS := Metal.cpp ccv_nnc_mfa.cpp ccv_nnc_mfa_attention.cpp ccv_nnc_mfa_error.cpp ccv_nnc_mfa_gemm.cpp ccv_nnc_mfa_normalization.cpp ccv_nnc_mfa_depalettize.cpp ccv_nnc_mfa_adam.cpp ccv_nnc_mfa_cmul.cpp ccv_nnc_mfa_gemv.cpp ccv_nnc_mfa_cast.cpp ccv_nnc_mfa_add.cpp 3rdparty/metal-cpp/Dispatch.cpp v2/CodeWriter.cpp v2/GEMMDescriptor.cpp v2/GEMMKernelDescriptor.cpp v2/GEMMHeaders.cpp v2/GEMMKernel.cpp v2/AttentionDescriptor.cpp v2/AttentionKernelDescriptor.cpp v2/AttentionKernel.cpp

SRC_OBJS := $(patsubst %.c,%.o,$(patsubst %.cpp,%.o,$(SRCS)))

.PHONY: release all lib clean

release: all

include ../../scheme.mk

all: lib

lib: libnnc-compat-mfa.o

clean:
	rm -f *.o

libnnc-compat-mfa.o: $(SRC_OBJS)
	ld -r $^ -o $@

%.o: %.c
	$(CC) $< -o $@ -c $(CFLAGS)

%.o: %.cpp
	$(CC) $< -o $@ -c $(CFLAGS)

dep: .dep.mk
.dep.mk: $(SRCS)
	echo '' > .dep.mk
	for SRC in $(patsubst %.c,,$^) ; do \
		$(CC) $(CFLAGS) -M $$SRC | sed -e 's/^.*\://g' | (echo "$${SRC%%.*}.o: \\" && cat) >> .dep.mk ; \
	done

-include .dep.mk

