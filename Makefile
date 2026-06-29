KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

obj-m := shmipc.o

shmipc-objs := main.o ipc_dev.o

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean