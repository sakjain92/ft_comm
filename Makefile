IDIR =./include
CC=gcc
CFLAGS= -Wall -Wextra -I$(IDIR)

ODIR=obj
SDIR=src

LIBS=-levent -lpthread

_DEPS = list.h comm.h
DEPS = $(patsubst %,$(IDIR)/%,$(_DEPS))

_SRC=$(wildcard $(SDIR)/*.c)
SRC=$(notdir $(_SRC))
_OBJ=$(SRC:.c=.o)
OBJ = $(patsubst %,$(ODIR)/%,$(_OBJ))


$(ODIR)/%.o: $(SDIR)/%.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

comm.a: $(OBJ)
	ar rcs $@ $^


.PHONY: clean all


clean:
	rm -f $(ODIR)/*.o *~ core $(INCDIR)/*~ 
