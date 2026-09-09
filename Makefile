# The Socket Exchange - build for FreeBSD (clang++) and Linux (g++).
#
#   make               build the server, both clients and the bonus tool
#   make -j4           the same, in parallel
#   make test          build, then run the protocol conformance tests
#   make clean         remove build products
#   make submission ROLLS="ROLL1 ROLL2"    package the submission ZIP

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
CPPFLAGS += -Isrc
LDFLAGS  ?=

BIN_DIR   := bin
BUILD_DIR := build

SERVER  := $(BIN_DIR)/exchange_server
TRADER  := $(BIN_DIR)/trader_client
MARKET  := $(BIN_DIR)/market_data_client
CONNGEN := $(BIN_DIR)/conn_gen

# Sources shared by more than one binary; each is compiled once.
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

TRADER_SRC  := src/trader_client.cpp $(CLIENT_SRC)
MARKET_SRC  := src/market_data_client.cpp $(CLIENT_SRC)
CONNGEN_SRC := src/tools/conn_gen.cpp src/network/socket_utils.cpp

ALL_SRC := $(sort $(SERVER_SRC) $(TRADER_SRC) $(MARKET_SRC) $(CONNGEN_SRC))

obj = $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(1))

SERVER_OBJ  := $(call obj,$(SERVER_SRC))
TRADER_OBJ  := $(call obj,$(TRADER_SRC))
MARKET_OBJ  := $(call obj,$(MARKET_SRC))
CONNGEN_OBJ := $(call obj,$(CONNGEN_SRC))
ALL_OBJ     := $(call obj,$(ALL_SRC))

.PHONY: all clean test check-kqueue submission
.SUFFIXES:

all: $(SERVER) $(TRADER) $(MARKET) $(CONNGEN)

# Per-translation-unit objects, so that `make -j` parallelises the build and
# rebuilds stay incremental. Header dependencies are tracked with -MMD.
$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

$(SERVER): $(SERVER_OBJ)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TRADER): $(TRADER_OBJ)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(MARKET): $(MARKET_OBJ)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(CONNGEN): $(CONNGEN_OBJ)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

-include $(ALL_OBJ:.o=.d)

test: all check-kqueue
	python3 tests/test_protocol.py

# The kqueue backend is the one used on FreeBSD but cannot be built on Linux.
# This compiles it against a stand-in for <sys/event.h> that matches FreeBSD's
# declarations, so its syntax and API usage are checked during development.
check-kqueue:
	@if [ "$$(uname -s)" = "FreeBSD" ]; then \
	    echo "check-kqueue: native FreeBSD build already covers this"; \
	else \
	    $(CXX) $(CXXFLAGS) $(CPPFLAGS) -Itests/freebsd-stub \
	        -DEXCHANGE_FORCE_KQUEUE -fsyntax-only src/network/event_loop.cpp && \
	    echo "check-kqueue: kqueue backend compiles"; \
	fi

# make submission ROLLS="2024CS10057 2024CS10093"
submission:
	@test -n "$(ROLLS)" || { echo 'usage: make submission ROLLS="ROLL1 ROLL2"'; exit 2; }
	tools/make_submission.sh $(ROLLS)

clean:
	rm -rf $(BIN_DIR) $(BUILD_DIR)
