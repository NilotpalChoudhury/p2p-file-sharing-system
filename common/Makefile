CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2

# This only builds the standalone Day-1 verification harness (common/test_day1.cpp).
# sha1.cpp and file_pieces.cpp are ALSO compiled directly into tracker/ and client/ (see their
# own Makefiles) from Day 2 onwards, since upload_file/download_file need them there too. This
# harness stays useful on its own for independently re-checking SHA1 + chunking in isolation.

TARGET = test_day1
SRCS = test_day1.cpp sha1.cpp file_pieces.cpp

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
