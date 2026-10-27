####### Compiler, tools and options

CC            = arm-none-linux-gnueabi-gcc
CXX           = arm-none-linux-gnueabi-g++
CFLAGS        = -pipe -fno-exceptions -O3 -march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp -Wall -W 
CXXFLAGS      = -pipe -fno-exceptions -O3 -march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp -Wall -W 
INCPATH       = -I/home/abyss/back_up/alsa_dm8168/alsa/include
LINK          = arm-none-linux-gnueabi-g++
LFLAGS        = -Wl,-rpath-link,-O1,-rpath
LIBS          = -L /home/abyss/back_up/alsa_dm8168/alsa/lib  -lasound 
MKDIR 		  = mkdir -p

ifeq (,$(EXEC_DIR))
EXEC_DIR = ./install
endif

all: alsa02

alsa02.o: alsa02.c
	$(CC) -c alsa02.c $(CFLAGS) $(INCPATH)
alsa02: alsa02.o
	@$(MKDIR) bin
	$(CC) -o bin/alsa02 alsa02.o $(LIBS)
clean:
	rm -f *.o bin/*
install:
	@echo "#"
	@echo "# Making $@ ..."
	@$(MKDIR) $(EXEC_DIR)/debug
	cp bin/alsa02 $(EXEC_DIR)/debug
	@$(MKDIR) $(EXEC_DIR)/release
	cp bin/alsa02 $(EXEC_DIR)/release

