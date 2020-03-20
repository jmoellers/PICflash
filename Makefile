CFLAGS += -g
LIBS += -lexpat
BINDIR=~/bin

PICflash:	PICflash.o config.o
	$(CC) $(CFLAGS) PICflash.o config.o -o $@ $(LIBS)

PICflash.o:	PICflash.c
	$(CC) $(CFLAGS) -c $< -o $@

config.o:	config.c
	$(CC) $(CFLAGS) -c $< -o $@

install:	 PICflash
	cp $< $(BINDIR)
