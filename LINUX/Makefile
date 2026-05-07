CXX    = g++
CFLAGS = -std=c++98 -O2 -Wall -Wextra -DNOTRACE -I../src

SRC  = ../src
UNIX = ../UNIX

SRCS_MAIN = \
	$(SRC)/cody.cpp \
	$(SRC)/http.cpp \
	$(SRC)/json.cpp \
	$(SRC)/session.cpp \
	$(SRC)/tools.cpp \
	$(UNIX)/console_unix.cpp \
	$(UNIX)/socket_unix.cpp \
	$(UNIX)/tools_unix.cpp \
	$(UNIX)/tui_unix.cpp \
	$(SRC)/ui.cpp \
	$(SRC)/log.cpp

OBJS_MAIN = $(patsubst %.cpp,%.o,$(notdir $(SRCS_MAIN)))

VPATH = $(SRC):$(UNIX)

all: cody

cody: $(OBJS_MAIN)
	$(CXX) $(CFLAGS) -o cody $(OBJS_MAIN) -ltinfo

%.o: %.cpp
	$(CXX) $(CFLAGS) -c -o $@ $<

# Unit tests
test: test_json

test_json: json_test
	./json_test

json_test: $(SRC)/json.cpp $(SRC)/json_test.cpp
	$(CXX) $(CFLAGS) -o json_test $(SRC)/json.cpp $(SRC)/json_test.cpp

clean:
	rm -f *.o cody json_test

.PHONY: all test test_json clean
