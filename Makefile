IDIR =./include
ODIR=obj
SDIR=src
TDIR=test

CC=gcc
CFLAGS= -Wall -Wextra -I$(IDIR)
LDFLAGS=-L./ -L./libevent-release-1.4.15-stable/.libs

COMM_LIB_NAME =comm
COMM_LIB = lib$(COMM_LIB_NAME).a

LIBS = -l$(COMM_LIB_NAME) -Wl,-Bstatic -levent -Wl,-Bdynamic -lrt -pthread 
_DEPS = list.h comm.h
DEPS = $(patsubst %,$(IDIR)/%,$(_DEPS))

_SRC = $(wildcard $(SDIR)/*.c)
SRC = $(notdir $(_SRC))

_OBJ = $(SRC:.c=.o)
OBJ = $(patsubst %,$(ODIR)/%,$(_OBJ))

_TEST_SRC = $(wildcard $(TDIR)/*.c)
TEST_SRC = $(notdir $(_TEST_SRC))
TESTS = $(TEST_SRC:.c=.elf)

$(ODIR)/%.o: $(SDIR)/%.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

%.elf: $(TDIR)/%.c $(COMM_LIB)
	@echo $(LIBEVENT)
	$(CC) -o $@ $< $(CFLAGS) $(LDFLAGS) $(LIBS)

all: $(COMM_LIB) $(TESTS)

$(COMM_LIB): $(OBJ)
	ar rcs $@ $^


.PHONY: clean all


clean:
	rm -f $(ODIR)/*.o *~ core $(INCDIR)/*~  *.a *.elf
