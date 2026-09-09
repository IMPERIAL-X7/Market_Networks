CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -Isrc -pthread

# All source files required for the server
SERVER_SRC = \
    src/exchange_server.cpp \
    src/network/tcp_server.cpp \
    src/protocol/message_parser.cpp \
    src/trading/order_book.cpp

# Executable names
SERVER = src/exchange_server
TRADER = src/trader_client
MARKET = src/market_data_client

all: $(SERVER) $(TRADER) $(MARKET)

$(SERVER): $(SERVER_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Client build targets
$(TRADER): src/trader_client.cpp src/network/tcp_client.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

$(MARKET): src/market_data_client.cpp src/network/tcp_client.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f $(SERVER) $(TRADER) $(MARKET)