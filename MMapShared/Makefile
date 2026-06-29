KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

obj-m = misc-mmap.o

misc-mmap-objs=main.o file-funcs.o mmap-funcs.o

all:
	make -C $(KERNEL_DIR) M=`pwd` modules

clean:
	make -C $(KERNEL_DIR) M=`pwd` clean
