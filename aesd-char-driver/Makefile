# See example Makefile from scull project
# Comment/uncomment the following line to disable/enable debugging
# DEBUG = y

ifeq ($(DEBUG),y)
DEBFLAGS = -O -g -DAESD_DEBUG
else
DEBFLAGS = -O2
endif

EXTRA_CFLAGS += $(DEBFLAGS)

ifneq ($(KERNELRELEASE),)

# Called from kernel build system
obj-m := aesdchar.o
aesdchar-y := aesd-circular-buffer.o main.o

else

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_SRC ?= $(KERNELDIR)
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

modules:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean

endif
