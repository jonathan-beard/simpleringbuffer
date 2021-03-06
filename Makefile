CC    ?= gcc
CXX   ?= g++


CFLAGS   =  -O2 -g -Wall -std=c99
CXXFLAGS =  -O2 -g  -Wall -std=c++11  -DRDTSCP=1 #-DLIMITRATE=1

CXXOBJS = main pointer fifo

CFILES = $(addsuffix .c, $(COBJS) )
CXXFILES = $(addsuffix .cpp, $(CXXOBJS) )

OBJS = $(addsuffix .o, $(COBJS) ) $(addsuffix .o, $(CXXOBJS) )

ifneq ($(shell uname -s), Darwin)
RT = -lrt
STATIC = -static -static-libgcc -static-libstdc++
endif

INCS = 
LIBS = -lpthread -lm -lc $(RT) $(STATIC) 

compile: $(CXXFILES) $(CFILES)
	$(MAKE) $(OBJS)
	$(CXX) $(CXXFLAGS) $(INCS) $(OBJS) $(LIBS) -o ringb

%.o: %.cpp
	$(CXX) -c $(CXXFLAGS) $(INCS) -o $@ $<

%.o: %.c
	$(CC) -c $(CFLAGS) $(INCS) -o $@ $<

.PHONY: clean
clean:
	rm -rf ringb $(OBJS)
