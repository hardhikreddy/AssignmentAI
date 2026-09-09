CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2

# Event-loop back end for the Exchange Server:
#   (unset)        -> poll(2), portable default
#   POLLER=epoll   -> Linux epoll   (local benchmarking)
#   POLLER=kqueue  -> FreeBSD kqueue (submission-target fast path)
POLLER ?=
CXXFLAGS_epoll = -DUSE_EPOLL
CXXFLAGS_kqueue = -DUSE_KQUEUE
CXXFLAGS += $(CXXFLAGS_$(POLLER))

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
