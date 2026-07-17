# Compiler and flags
CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread -O3 $(shell pkg-config --cflags absl_flags absl_flags_parse)
# Some distro abseil .pc files emit malformed bare "-Wl" tokens in --libs; keep
# only the -l/-L flags and wrap them in a link group (abseil has circular deps).
ABSL_RAW := $(shell pkg-config --libs absl_flags absl_flags_parse)
ABSL_LIBS := $(filter -l%,$(ABSL_RAW))
ABSL_LDIRS := $(filter -L%,$(ABSL_RAW))
LDFLAGS = $(ABSL_LDIRS) -Wl,--start-group $(ABSL_LIBS) -Wl,--end-group -lpthread

# Source and object files
SRCS = main.cpp network_emulator.cpp ../../logger/Logger.cpp
OBJS = $(SRCS:.cpp=.o)

# Target executable
TARGET = network_emulator

# Default rule to build the target
all: $(TARGET)

# Rule to build the target executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) $(LDFLAGS) -o $(TARGET)

# Rule to compile .cpp files into .o files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean up build artifacts
clean:
	rm -f $(OBJS) $(TARGET)