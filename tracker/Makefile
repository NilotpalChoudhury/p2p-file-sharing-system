CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -I../common

SRCS = tracker.cpp tracker_state.cpp ../common/network.cpp ../common/file_pieces.cpp ../common/sha1.cpp
TARGET = tracker

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
