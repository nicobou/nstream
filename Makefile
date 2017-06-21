NAME= nstream~ nsreceive~ 


OBJS = nstream~.o\
	nsreceive~.o 


AS_CFLAGS += -DPD 

CFLAGS += -fPIC -O2 -Wall -Wimplicit -Wshadow -Wstrict-prototypes \
          -Wno-unused -Wno-parentheses -Wno-switch

ifndef CC
 CC  = gcc
endif

%.o: %.c
	$(CC) $(CFLAGS) $(AS_CFLAGS) $(AS_INCLUDE) -c $< -o $@

all: $(OBJS)
	@for i in $(NAME); do \
	echo $(NAME) ;\
	($(CC) -export_dynamic -shared -o $$i.pd_linux $$i.o -lc -lm);\
	done

clean:
	-rm -f *.o *.pd_* so_locations
