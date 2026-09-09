# The Socket Exchange - build for FreeBSD (clang++) and Linux (g++).
#
#   make            build server, both clients and the bonus tool
#   make clean      remove build products
#   make test       build, then run the protocol conformance tests

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
CPPFLAGS += -Isrc
LDFLAGS  ?=

BIN_DIR := bin

SERVER := $(BIN_DIR)/exchange_server
TRADER := $(BIN_DIR)/trader_client
MARKET := $(BIN_DIR)/market_data_client
CONNGEN := $(BIN_DIR)/conn_gen

COMMON_SRC := \
    src/network/socket_utils.cpp \
    src/protocol/message_parser.cpp

SERVER_SRC := \
    src/exchange_server.cpp \
    src/network/event_loop.cpp \
    src/network/tcp_server.cpp \
    src/trading/order_book.cpp \
    $(COMMON_SRC)

CLIENT_SRC := \
    src/client_shell.cpp \
    src/network/tcp_client.cpp \
    $(COMMON_SRC)

.PHONY: all clean test

all: $(SERVER) $(TRADER) $(MARKET) $(CONNGEN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(SERVER): $(SERVER_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $(SERVER_SRC) $(LDFLAGS)

$(TRADER): src/trader_client.cpp $(CLIENT_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ src/trader_client.cpp $(CLIENT_SRC) $(LDFLAGS)

$(MARKET): src/market_data_client.cpp $(CLIENT_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ src/market_data_client.cpp $(CLIENT_SRC) $(LDFLAGS)

$(CONNGEN): src/tools/conn_gen.cpp src/network/socket_utils.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ src/tools/conn_gen.cpp src/network/socket_utils.cpp $(LDFLAGS)

test: all
	python3 tests/test_protocol.py

clean:
	rm -rf $(BIN_DIR)
