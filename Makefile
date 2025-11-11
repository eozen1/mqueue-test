APP:=mq_benchmark
SRC:=src/mq_benchmark.cpp
OBJ:=build/mq_benchmark.o
BIN:=build/$(APP)

UNAME_S:=$(shell uname -s)

CXX?=g++
CXXFLAGS:=-O3 -std=c++17 -Wall -Wextra -Wpedantic -pthread
LDFLAGS:=

ifeq ($(UNAME_S),Linux)
	LDFLAGS+=-lrt
endif

ifeq ($(UNAME_S),Darwin)
GCD_BIN:=build/gcd_benchmark
GCD_SRC:=src/gcd_benchmark.cpp
GCD_OBJ:=build/gcd_benchmark.o
NSOP_BIN:=build/nsop_benchmark
NSOP_SRC:=src/nsop_benchmark.mm
NSOP_OBJ:=build/nsop_benchmark.o

CLANG?=clang++
MAC_CXXFLAGS:=-O3 -std=c++17 -Wall -Wextra -Wpedantic -pthread
MAC_LDFLAGS_GCD:=
MAC_LDFLAGS_NSOP:=-framework Foundation

all: $(GCD_BIN) $(NSOP_BIN)

$(GCD_BIN): $(GCD_OBJ)
	@mkdir -p $(dir $(GCD_BIN))
	$(CLANG) $(MAC_CXXFLAGS) -o $(GCD_BIN) $(GCD_OBJ) $(MAC_LDFLAGS_GCD)

$(GCD_OBJ): $(GCD_SRC)
	@mkdir -p $(dir $(GCD_OBJ))
	$(CLANG) $(MAC_CXXFLAGS) -c $(GCD_SRC) -o $(GCD_OBJ)

$(NSOP_BIN): $(NSOP_OBJ)
	@mkdir -p $(dir $(NSOP_BIN))
	$(CLANG) $(MAC_CXXFLAGS) -o $(NSOP_BIN) $(NSOP_OBJ) $(MAC_LDFLAGS_NSOP)

$(NSOP_OBJ): $(NSOP_SRC)
	@mkdir -p $(dir $(NSOP_OBJ))
	$(CLANG) $(MAC_CXXFLAGS) -c $(NSOP_SRC) -o $(NSOP_OBJ)

run:
	@echo "Run macOS benchmarks:"
	@echo "  $(GCD_BIN) --help"
	@echo "  $(NSOP_BIN) --help"
else
$(BIN): $(OBJ)
	@mkdir -p $(dir $(BIN))
	$(CXX) $(CXXFLAGS) -o $(BIN) $(OBJ) $(LDFLAGS)

$(OBJ): $(SRC)
	@mkdir -p $(dir $(OBJ))
	$(CXX) $(CXXFLAGS) -c $(SRC) -o $(OBJ)
endif

.PHONY: all clean run

clean:
	rm -rf build


