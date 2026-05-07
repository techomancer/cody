#
# cody IRIX Makefile — MIPSPro 7.4.4  (smake or make)
# Target: IRIX 6.5, n32 ABI
#

CXX    = CC
CFLAGS = -n32 -O2 -DNOTRACE -I../src

OBJS = cody.o http.o json.o session.o tools.o ui.o log.o \
       console_unix.o socket_unix.o tools_unix.o tui_unix.o

all: cody

cody: $(OBJS)
	$(CXX) $(CFLAGS) -o cody $(OBJS) -lcurses

cody.o:         ../src/cody.cpp
	$(CXX) $(CFLAGS) -c -o cody.o ../src/cody.cpp
http.o:         ../src/http.cpp
	$(CXX) $(CFLAGS) -c -o http.o ../src/http.cpp
json.o:         ../src/json.cpp
	$(CXX) $(CFLAGS) -c -o json.o ../src/json.cpp
session.o:      ../src/session.cpp
	$(CXX) $(CFLAGS) -c -o session.o ../src/session.cpp
tools.o:        ../src/tools.cpp
	$(CXX) $(CFLAGS) -c -o tools.o ../src/tools.cpp
ui.o:           ../src/ui.cpp
	$(CXX) $(CFLAGS) -c -o ui.o ../src/ui.cpp
log.o:          ../src/log.cpp
	$(CXX) $(CFLAGS) -c -o log.o ../src/log.cpp
console_unix.o: ../UNIX/console_unix.cpp
	$(CXX) $(CFLAGS) -c -o console_unix.o ../UNIX/console_unix.cpp
socket_unix.o:  ../UNIX/socket_unix.cpp
	$(CXX) $(CFLAGS) -c -o socket_unix.o ../UNIX/socket_unix.cpp
tools_unix.o:   ../UNIX/tools_unix.cpp
	$(CXX) $(CFLAGS) -c -o tools_unix.o ../UNIX/tools_unix.cpp
tui_unix.o:     ../UNIX/tui_unix.cpp
	$(CXX) $(CFLAGS) -c -o tui_unix.o ../UNIX/tui_unix.cpp

clean:
	rm -f *.o cody
