CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2

all: exchange_server trader_client market_data_client

exchange_server: src/server.cpp src/net_utils.hpp
	$(CXX) $(CXXFLAGS) src/server.cpp -o exchange_server

trader_client: src/trader.cpp src/net_utils.hpp
	$(CXX) $(CXXFLAGS) src/trader.cpp -o trader_client

market_data_client: src/market_data.cpp src/net_utils.hpp
	$(CXX) $(CXXFLAGS) src/market_data.cpp -o market_data_client

clean:
	rm -f exchange_server trader_client market_data_client server_test

.PHONY: all clean
