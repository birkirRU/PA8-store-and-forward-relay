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

    struct Client {
        uint64_t id;
        std::string in;
        std::string out;
        bool want_write = false;
        std::string ip;
        uint16_t port = 0;
        ConnType type = {ConnType::unknown};
        bool connecting = false;
        bool outgoing = false;
        bool sent_helo = false;
        std::string name = "";
    };
    struct Endpoint { std::string ip; uint16_t port; };
    using Answer = std::optional<std::string>;
    struct Message {
        std::string from;
        std::string to;
        time_t timestamp;
        std::string text;
    };

    void accept_client();
    void read_client(int fd);
    void connect_peer(const std::string& ip, uint16_t port);
    void finish_connect(int fd);
    void handle_command(int fd, const std::string& payload);

    // Client commands.
    std::string c_handle(const std::string& payload, const std::string& cmd);
    std::string c_sendmsg(const std::vector<std::string>& f);
    std::string c_getmsg(const std::vector<std::string>& f);
    std::string c_statusreq();

    // Peer commands.
    Answer p_handle(Client& c, const std::string& payload, const std::string& cmd);
    Answer p_helo(Client& c, const std::vector<std::string>& f);
    Answer p_servers(const std::string& payload);
    Answer p_keepalive(const std::vector<std::string>& f);
    Answer p_getmsgs(const std::vector<std::string>& f);
    Answer p_sendmsg(const std::vector<std::string>& f);
    Answer p_statusresp(const std::vector<std::string>& f);
    Answer p_statusreq();

    std::string servers_list() const;
    void reply(int fd, const std::string& text);
    bool flush(int fd);
    void close_client(int fd, const std::string& reason);
    std::string tag(int fd) { return clients_[fd].type.to_string() + std::to_string(clients_[fd].id); }

    int ep_ = -1;
    int listen_fd_ = -1;
    int spare_fd_ = -1;
    uint64_t next_id_ = 1;
    std::unordered_map<int, Client> clients_;
    std::map<std::string, std::deque<Message>> mailboxes_;
    size_t stored_ = 0;
    std::map<std::string, Endpoint> known_;
};
