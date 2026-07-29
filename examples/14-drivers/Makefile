# Kbuild Makefile for the hellochar kernel module. NOT the course's top-level
# make -- kernel modules must build against the running kernel's headers.
#
#   make        build hellochar.ko against the running kernel
#   make clean  remove the build artifacts
#
# Requires: linux-headers-$(uname -r)   (e.g. apt install linux-headers-$(uname -r))
obj-m += hellochar.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
