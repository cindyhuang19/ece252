CC = gcc
CFLAGS = -Wall -std=gnu99 -g # "curl-config --cflags" output is empty  
LD = gcc
LDFLAGS = -std=gnu99 -g
LDLIBS = -lz -lcurl -pthread       # "curl-config --libs" output 

SRCS   = paster2.c zutil.c crc.c
OBJS   = paster2.o zutil.o crc.o
TARGETS= paster2

all: ${TARGETS}

paster2: $(OBJS)
    $(LD) -o $@ $^ $(LDLIBS) $(LDFLAGS)  

%.o: %.c
    $(CC) $(CFLAGS) -c $< 

%.d: %.c
    gcc -MM -MF $@ $<

-include $(SRCS:.c=.d)

.PHONY: clean
clean:
    rm -f *~ *.d *.o *~ $(TARGETS) *.png
~                     