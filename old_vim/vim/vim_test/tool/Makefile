ARCH := $(shell uname -m)

ifeq ($(ARCH), sparc64)
	DMA_ADDR_LENGTH := 4
	BE_ARCH := 1
	EXT_CFLAGS := -m64
endif

ifeq ($(ARCH), x86_64)
	DMA_ADDR_LENGTH := 8
	BE_ARCH := 0
	EXT_CFLAGS := -m64
endif

ifeq ($(ARCH), ppc64)
	DMA_ADDR_LENGTH := 8
	BE_ARCH := 1
	EXT_CFLAGS := -m64
endif

ifeq ($(ARCH), mips64el)
	DMA_ADDR_LENGTH := 8
	BE_ARCH := 0
endif

ifeq ($(ARCH), ia64)
	DMA_ADDR_LENGTH := 8
	BE_ARCH := 0
endif

ifeq ($(ARCH), aarch64)
	DMA_ADDR_LENGTH := 8
	BE_ARCH := 0
	EXT_CFLAGS := -D__AARCH64__
endif


CFLAGS		:= -Wall -std=gnu99 -DDMA_ADDR_LENGTH=$(DMA_ADDR_LENGTH) -DBE_ARCH=$(BE_ARCH)
CFLAGS		+= $(EXT_CFLAGS)

TARGET		= ztool
RELEASE 	= shtool
SRC		= main.c init.c parse.c utils.c api.c super.c req.c bbt.c ecc.c ifmode.c mpt.c bufwrite.c dio.c nor.c help.c microcode.c graphics.c dev-type.c
RELEASE_SRC	= main.c init.c parse.c utils.c api.c super.c req.c bbt.c mpt.c help.c microcode.c graphics.c dev-type.c
HEADER		= tool.h list.h both.h shannon-mbr.h graphics.h dev-type.h

PHONY := ckarch

all: ckarch $(TARGET) $(RELEASE)

ckarch:
	@if [ -z "$(DMA_ADDR_LENGTH)" ]; then				\
		echo "This tool just support x86_64 and sparc64."	\
		     "But this platform is $(ARCH)";			\
		false;							\
	fi

$(TARGET): $(SRC) $(HEADER)
	gcc $(CFLAGS) -g -o $@ $(SRC)
	cp -a ./$(TARGET) ../bin/

$(RELEASE): $(SRC) $(HEADER)
	gcc $(CFLAGS) -D__RELEASE__ -s -o $@ $(SRC)
	cp -a ./$(RELEASE) ../release/

clean:
	rm -f $(TARGET) $(RELEASE) ../bin/$(TARGET) ../release/$(RELEASE)

c:
	rm -f $(TARGET) $(RELEASE)
