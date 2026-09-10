CXX ?= c++
# Use += so -std=c++17 is always added, even when the base make (FreeBSD bmake)
# has already predefined CXXFLAGS to "-O2 -pipe". With ?= the standard flag
# would be silently dropped on FreeBSD.
CXXFLAGS += -std=c++17 -Wall -Wextra -O2

POLLER ?=
POLLERFLAG_epoll = -DUSE_EPOLL
POLLERFLAG_kqueue = -DUSE_KQUEUE
CXXFLAGS += $(POLLERFLAG_$(POLLER))

all: exchange_server trader_client market_data_client

exchange_server: src/server.cpp src/net_utils.hpp src/event_poller.hpp
	$(CXX) $(CXXFLAGS) src/server.cpp -o exchange_server

trader_client: src/trader.cpp src/net_utils.hpp
	$(CXX) $(CXXFLAGS) src/trader.cpp -o trader_client

market_data_client: src/market_data.cpp src/net_utils.hpp
	$(CXX) $(CXXFLAGS) src/market_data.cpp -o market_data_client

clean:
	rm -f exchange_server trader_client market_data_client server_test

.PHONY: all clean