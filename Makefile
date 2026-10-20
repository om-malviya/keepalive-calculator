# HTTP/1.1 keep-alive calculator.
#   make         build ./calc
#   make test    build, then run the integration tests (starts its own server)
#   make mark    run the handout's marking script against a server already
#                running on PORT (default 8080)
#   make clean   remove build outputs

# make predefines CXX as "c++"; prefer clang++ unless the user chose a compiler.
ifeq ($(origin CXX),default)
CXX := clang++
endif
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
LDFLAGS ?= -pthread
PORT ?= 8080

SRCS := $(wildcard src/*.cpp)
OBJS := $(SRCS:.cpp=.o)
HDRS := $(wildcard src/*.hpp)

.PHONY: all test mark clean

all: calc

calc: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

src/%.o: src/%.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: calc
	python3 -I tests/test_calc.py -v

mark: calc
	python3 -I tests/marker.py $(PORT)

clean:
	rm -f calc $(OBJS)
