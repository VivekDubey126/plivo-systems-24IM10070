CXX      = g++
CXXFLAGS = -O3 -std=c++17 -Wall -Wextra

ifeq ($(OS),Windows_NT)
LDFLAGS = -lws2_32
EXT     = .exe
else
LDFLAGS =
EXT     =
endif

all: sender$(EXT) receiver$(EXT)

sender$(EXT): sender.cpp
	$(CXX) $(CXXFLAGS) -o sender$(EXT) sender.cpp $(LDFLAGS)

receiver$(EXT): receiver.cpp
	$(CXX) $(CXXFLAGS) -o receiver$(EXT) receiver.cpp $(LDFLAGS)

clean:
	rm -f sender$(EXT) receiver$(EXT)

.PHONY: all clean
