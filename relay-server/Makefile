CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++20
CXXDEBUGFLAGS = -g

COMMON = common.cpp common.h identity.h log_event.h
SERVER = server.cpp server_cmds.cpp server.h $(COMMON)

all: tsamserver tsamclient

debug: tsamserver_debug tsamclient_debug

tsamserver: $(SERVER)
	$(CXX) $(CXXFLAGS) server.cpp server_cmds.cpp common.cpp -o $@

tsamclient: client.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) client.cpp common.cpp -o $@

tsamserver_debug: $(SERVER)
	$(CXX) $(CXXFLAGS) $(CXXDEBUGFLAGS) server.cpp server_cmds.cpp common.cpp -o $@

tsamclient_debug: client.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) $(CXXDEBUGFLAGS) client.cpp common.cpp -o $@

clean:
	rm -f tsamserver tsamclient tsamserver_debug tsamclient_debug

.PHONY: all debug clean
