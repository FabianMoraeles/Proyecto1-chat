CXX      = g++
CXXFLAGS = -std=c++17 -Wall -O2 -pthread $(shell pkg-config --cflags protobuf)
INCLUDES = -Iinclude -Iprotos

PROTO_DIR = protos
PROTO_CC  = $(wildcard $(PROTO_DIR)/*.pb.cc)
PROTO_OBJ = $(PROTO_CC:.cc=.o)

LIBS_SERVER = $(shell pkg-config --libs protobuf) -lpthread
LIBS_CLIENT = $(shell pkg-config --libs protobuf) -lpthread -lncursesw

.PHONY: all clean server client protos

all: server client

# Compile protos
$(PROTO_DIR)/%.o: $(PROTO_DIR)/%.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Server
server: src/server.cpp $(PROTO_OBJ)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o server $(LIBS_SERVER)
	@echo "✓  server built"

# Client
client: src/client.cpp $(PROTO_OBJ)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o client $(LIBS_CLIENT)
	@echo "✓  client built"

clean:
	rm -f $(PROTO_OBJ) server client
	@echo "✓  cleaned"
