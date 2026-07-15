CC ?= cc
CFLAGS ?= -O2 -Wall
LDLIBS ?= -lm

all: sender receiver

sender: sender.c
	$(CC) $(CFLAGS) -o sender sender.c $(LDLIBS)

receiver: receiver.c
	$(CC) $(CFLAGS) -o receiver receiver.c $(LDLIBS)

clean:
	rm -f sender receiver
