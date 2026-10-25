CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++20
CXXDEBUGFLAGS = -g

COMMON = common.cpp common.h identity.h log_event.h

all: tsamserver tsamclient

debug: tsamserver_debug tsamclient_debug

tsamserver: server.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) server.cpp common.cpp -o $@

tsamclient: client.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) client.cpp common.cpp -o $@

tsamserver_debug: server.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) $(CXXDEBUGFLAGS) server.cpp common.cpp -o $@

tsamclient_debug: client.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) $(CXXDEBUGFLAGS) client.cpp common.cpp -o $@

clean:
	rm -f tsamserver tsamclient tsamserver_debug tsamclient_debug

.PHONY: all debug clean
