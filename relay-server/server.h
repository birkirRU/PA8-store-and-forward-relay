#pragma once

#include <cstdint>
#include <cstddef>
#include <ctime>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

const uint16_t INSTRUCTOR_PORT = 5001;
const std::string  INSTRUCTOR_IP = "130.208.246.98";

const size_t TARGET_PEERS = 7;      // open new server connections while we have fewer than this
const size_t MAX_PEERS = 8;         // close incoming servers beyond this
const time_t RETRY_AFTER = 60;      // seconds before we dial the same server again

const size_t MAX_PER_GROUP = 200;    // messages held for one destination
const size_t MAX_STORED    = 2000;   // messages held in total

const size_t MAX_KNOWN_SERVERS = 500;

class Server {
public:
    explicit Server(uint16_t port);
    void run();  // returns when SIGINT/SIGTERM arrives

private:
    struct ConnType {
        enum Value { client, peer, unknown };
        Value value;

        std::string to_string() const {
            switch (value) {
                case client:  return "Client";
                case peer:    return "Peer";
                case unknown: return "Unknown";
            }
            return "Unknown";
        }
    };

    // One per connection, our own client and other servers alike (keyed by fd in clients_).
    struct Client {
        uint64_t id;
        std::string in;               // received bytes not yet forming a complete frame
        std::string out;              // framed bytes the socket has not taken yet
        bool want_write = false;      // EPOLLOUT registered (only while `out` is non-empty)
        std::string ip;
        uint16_t port = 0;            // for incoming connections: their source port, not their listening port
        ConnType type = {ConnType::unknown};
        bool connecting = false;      // our non-blocking connect() has not finished yet
        bool outgoing = false;        // we opened this connection
        bool sent_helo = false;
        std::string name = "";        // group ID from their HELO (empty until then)
        time_t last_keepalive = 0;
        time_t last_statusreq = 0;
        std::string target = "";      // group ID we dialled (empty for the instructor fallback)
    };
    struct Endpoint {
        std::string ip;
        uint16_t port;
        time_t next_try = 0;          // don't dial before this time
    };
    using Answer = std::optional<std::string>;   // see the top of server_cmds.cpp
    struct Message {
        std::string from;
        std::string to;
        time_t timestamp;
        std::string text;
    };

    void accept_client();
    void read_client(int fd);
    void connect_peer(const std::string& ip, uint16_t port, const std::string& target = "");
    void finish_connect(int fd);
    void handle_command(int fd, const std::string& payload);

    // Client commands.
    std::string c_handle(const std::string& payload, const std::string& cmd);
    std::string c_sendmsg(const std::vector<std::string>& f);
    std::string c_getmsg(const std::vector<std::string>& f);
    std::string c_statusreq();

    // Peer commands.
    Answer p_handle(int fd, Client& c, const std::string& payload, const std::string& cmd);
    Answer p_helo(Client& c, const std::vector<std::string>& f);
    Answer p_servers(const std::string& payload);
    Answer p_keepalive(const std::vector<std::string>& f);
    Answer p_getmsgs(int fd, const std::vector<std::string>& f);
    Answer p_sendmsg(const std::vector<std::string>& f);
    Answer p_statusresp(int fd, const std::vector<std::string>& f);
    Answer p_statusreq();

    std::string servers_list() const;
    void queue_send(int fd, const std::string& text);
    bool flush(int fd);
    void close_client(int fd, const std::string& reason);
    std::string tag(int fd) { return clients_[fd].type.to_string() + std::to_string(clients_[fd].id); }

    std::string find_own_ip(const char* via_ip, uint16_t via_port);

    // KEEPALIVE
    void on_timer();
    int timer_fd_ = -1;

    // Staying connected: aim for TARGET_PEERS servers, accept at most MAX_PEERS
    void maintain_peers();
    size_t count_peers() const;
    std::string pick_candidate() const;
    time_t next_fallback_ = 0;

    bool is_connected_peer(const std::string& id) const;

    uint16_t listening_port_;
    std::string own_ip_;
    int ep_ = -1;
    int listen_fd_ = -1;
    int spare_fd_ = -1;
    uint64_t next_id_ = 1;
    std::unordered_map<int, Client> clients_;
    std::map<std::string, std::deque<Message>> mailboxes_;   // held messages per recipient, oldest first
    size_t stored_ = 0;
    std::map<std::string, Endpoint> known_;   // servers learned from SERVERS replies: who we can dial
};
