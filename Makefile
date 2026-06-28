CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread
TARGET   = order_book_demo
SRCS     = main.cpp order_book.cpp
OBJS     = $(SRCS:.cpp=.o)

# RL Agents Environment (separate translation unit; compiles to an
# object you can link into a training driver or pybind11 module).
RL_SRCS  = rl_env.cpp
RL_OBJS  = $(RL_SRCS:.cpp=.o)

all: $(TARGET) rl_env.o

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

# Build the RL env object without linking it into the demo binary.
rl_env.o: rl_env.cpp rl_env.h order_book.h
	$(CXX) $(CXXFLAGS) -c rl_env.cpp -o rl_env.o

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJS) $(RL_OBJS) $(TARGET)

.PHONY: all run clean
