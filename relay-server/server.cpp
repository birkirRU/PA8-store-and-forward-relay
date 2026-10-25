#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "log_event.h"
#include "identity.h"

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
namespace {


std::string errstr(const std::string& what) { return what + ": " + strerror(errno); }

// Startup or unrecoverable error: report on stderr, then exit.
[[noreturn]] void fatal(const std::string& why) {
    fprintf(stderr, "tsamserver: %s\n", why.c_str());
    log_note("FATAL %s", why.c_str());
    exit(1);
}

volatile sig_atomic_t stop_signal = 0;
void on_stop_signal(int sig) { stop_signal = sig; }

}  // namespace

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
// TODO: extend data model, must manage and differentiate the types
//      "client" (the user), "peer" (connected server on botnet) and "unknown" ()
// 
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
                case client:  return "Client";  // users talking to server
                case peer:    return "Peer";    // connected server in the botnet
                case unknown: return "Unknown";
            }
            return "Unknown";
        }
    };
    
    // TODO: Maybe rename this struct to host/node/endpoint
    struct Client {
        uint64_t id;                   // shown in the log; fds get reused, ids do not
        std::string in;           // received bytes not yet forming a complete frame
        std::string out;          // reply bytes the socket has not accepted yet
        bool want_write = false;  // watching EPOLLOUT because `out` is not empty
        std::string ip;
        uint16_t port;
        ConnType type = {ConnType::unknown};
        bool connecting = false;
        char* name = nullptr;     // GroupID for peer or name for client
    };
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
    std::string sendmsg(const std::vector<std::string>& f);
    std::string getmsg(const std::vector<std::string>& f);
    std::string statusreq();
    void reply(int fd, const std::string& text);
    bool flush(int fd);
    void close_client(int fd, const std::string& reason);
    std::string tag(int fd) { return clients_[fd].type.to_string() + std::to_string(clients_[fd].id); }

    int ep_ = -1;
    int listen_fd_ = -1;
    int spare_fd_ = -1;  // kept free so we can still accept-and-close when out of fds
    uint64_t next_id_ = 1;
    std::unordered_map<int, Client> clients_;                   // fd -> client
    std::map<std::string, std::deque<Message>> mailboxes_;      // user -> messages, oldest first
    size_t stored_ = 0;                                         // messages in all mailboxes
};

Server::Server(uint16_t port) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0) fatal(errstr("socket"));

    // Lets the server restart on the same port right away (no TIME_WAIT delay).
    int yes = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0)
        fatal(errstr("setsockopt(SO_REUSEADDR)"));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // all interfaces
    addr.sin_port = htons(port);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0)
        fatal(errstr("bind to port " + std::to_string(port)));
    if (listen(listen_fd_, SOMAXCONN) < 0) fatal(errstr("listen"));

    const std::string detail = "listening on port: " + std::to_string(port);
    log_event("START", NULL, port, GROUP_ID, detail.c_str());

    ep_ = epoll_create1(0);
    if (ep_ < 0) fatal(errstr("epoll_create1"));
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    if (epoll_ctl(ep_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) fatal(errstr("epoll_ctl(ADD listen)"));

    spare_fd_ = open("/dev/null", O_RDONLY);

}

void Server::run() {
    connect_peer("130.208.246.98", 5001); // Instructor

    epoll_event events[64];
    while (stop_signal == 0) {
        int n = epoll_wait(ep_, events, 64, -1);  // sleep until something is ready
        if (n < 0) {
            if (errno == EINTR) continue;  // a signal; the loop condition checks for stop
            fatal(errstr("epoll_wait"));
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == listen_fd_) { accept_client(); continue; }
            if (!clients_.count(fd)) continue;  // closed earlier in this batch
            if (clients_[fd].connecting) {
                if (events[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) finish_connect(fd);
                continue;
            }

            if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) read_client(fd);
            if (clients_.count(fd) && (events[i].events & EPOLLOUT)) flush(fd);
        }
    }
    
    log_note("shutting down on signal %d: %zu connected, %zu stored message(s) discarded", stop_signal, clients_.size(), stored_);
}

// Level-triggered epoll: if more clients are waiting, we are told again.
void Server::accept_client() {
    sockaddr_in addr{};
    socklen_t addr_length = sizeof(addr);
    int fd = accept4(listen_fd_, (sockaddr *)&addr, &addr_length, SOCK_NONBLOCK);
    
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED)
            return;  // nothing to accept after all, or the client already left
        if ((errno == EMFILE || errno == ENFILE) && spare_fd_ >= 0) {
            // Out of descriptors: the waiting connection would keep the listening socket
            // readable forever (busy loop). Free the spare fd, accept and close it, re-reserve.
            close(spare_fd_);
            int shed = accept(listen_fd_, nullptr, nullptr); 
            if (shed >= 0) close(shed);
            spare_fd_ = open("/dev/null", O_RDONLY);
            log_note("out of file descriptors, rejected a connection");
            return;
        }
        log_note("unknown error accepting connection");
        return;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        log_note("epoll_ctl(ADD client), connection refused");
        close(fd);
        return;
    }
    clients_[fd] = Client{next_id_++, "", "", false, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port)};
   
    std::string detail = tag(fd) + " connected (" + std::to_string(clients_.size()) + " connected)";
    log_event("ACCEPT", clients_[fd].ip.c_str(), clients_[fd].port, NULL, detail.c_str());
}

void Server::read_client(int fd) {
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n == 0) { close_client(fd, "closed the connection"); return; }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        close_client(fd, errstr("recv"));
        return;
    }

    std::vector<std::string> payloads, problems;
    clients_[fd].in.append(buf, n);
    extract_frames(clients_[fd].in, payloads, problems);
    for (const std::string& why : problems) {
        std::string detail = tag(fd) + ": discarded bytes: " + why;
        log_event("DROP", clients_[fd].ip.c_str(), clients_[fd].port, clients_[fd].name, detail.c_str());
    }

    for (const std::string& p : payloads) {
        if (!clients_.count(fd)) break;  // a failed reply closed the connection
        handle_command(fd, p);
    }
}

void Server::connect_peer(const std::string& ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { log_note(errstr("socket failed").c_str()); return; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        log_note("bad peer address %s", ip.c_str());
        close(fd);
        return;
    }
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    if (rc < 0 && errno != EINPROGRESS) {      // EINPROGRESS (handshake in progress) is normal for non-blocking
        
        log_note("connect to %s:%u failed: %s", ip.c_str(), port, strerror(errno));
        close(fd);
        return;
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT;            // EPOLLOUT = "connect finished"
    ev.data.fd = fd;
    if (epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) < 0) { close(fd); return; }

    Client c;
    c.id = next_id_++;
    c.ip = ip;
    c.port = port;
    c.type.value = ConnType::peer;
    c.connecting = true;
    c.want_write = true;                       // we registered EPOLLOUT; flush() will turn it off
    clients_[fd] = c;

}

void Server::finish_connect(int fd) {
    int err = 0;
    socklen_t len = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);   // did the connect succeed?
    if (err != 0) { close_client(fd, errstr("connect failed:")); return; }

    Client& c = clients_[fd];
    c.connecting = false;
    log_event("CONN", c.ip.c_str(), c.port, nullptr, "outgoing connection established");
    reply(fd, "HELO," GROUP_ID);
}

void Server::handle_command(int fd, const std::string& payload) {

    std::string com_name = payload.substr(0, payload.find(','));
    std::string answer;
    if (com_name == "SENDMSG")        answer = sendmsg(split_fields(payload, 4));
    else if (com_name == "GETMSG")    answer = getmsg(split_fields(payload, 2));
    else if (com_name == "STATUSREQ") answer = (payload == "STATUSREQ") ? statusreq() : "";

    if (answer.empty()) {  // unknown command, or wrong fields for a known one

        std::string detail = tag(fd) + ": rejected unknown or malformed command " + com_name;
        log_event("DROP", clients_[fd].ip.c_str(), clients_[fd].port, clients_[fd].name, detail.c_str());
        answer = "ERROR,command does not exist or is malformed";
    }
    log_event_bytes("RECV", clients_[fd].ip.c_str(), clients_[fd].port, clients_[fd].name, payload.data(), payload.size());

    reply(fd, answer);
}

// SENDMSG,<from>,<to>,<message>   -> "OK"
std::string Server::sendmsg(const std::vector<std::string>& f) {
    if (f.size() != 4 || !is_valid_name(f[1]) || !is_valid_name(f[2])) return "";
    mailboxes_[f[2]].push_back({f[1], f[3]});
    stored_++;
    log_note("stored message from %s for %s (%zu waiting for them, %zu stored in total)",
         f[1].c_str(), f[2].c_str(), mailboxes_[f[2]].size(), stored_);
    return "OK";
}

// GETMSG,<user>   -> "<from>,<message>", or "NO_MESSAGE"
std::string Server::getmsg(const std::vector<std::string>& f) {
    if (f.size() != 2 || !is_valid_name(f[1])) return "";
    auto it = mailboxes_.find(f[1]);
    if (it == mailboxes_.end()) return "NO_MESSAGE";

    Message m = it->second.front();
    it->second.pop_front();
    stored_--;
    size_t left = it->second.size();
    if (left == 0) mailboxes_.erase(it);  // so STATUSREQ only lists users with messages
    log_note("delivered message from %s to %s (%zu left for them, %zu stored in total)",
         m.from.c_str(), f[1].c_str(), left, stored_);
    return m.from + "," + m.text;
}

// STATUSREQ   -> "<user1>:<count1>,<user2>:<count2>,...", or "EMPTY"
std::string Server::statusreq() {
    std::string out;
    for (const auto& [user, box] : mailboxes_) {
        if (!out.empty()) out += ',';
        out += user + ":" + std::to_string(box.size());
    }
    return out.empty() ? "EMPTY" : out;
}

// Queues a framed reply and sends what the socket takes now.
void Server::reply(int fd, const std::string& payload) {
    std::string frame = encode_frame(payload);
    if (frame.empty()) {

        // Not using "DROP" because this is a self generated message that is to long to be sent
        // "DROP" is designed for incoming traffic
        log_note("%s: reply longer than %zu bytes, not sent", tag(fd).c_str(), MAX_PAYLOAD);
        return;
    }
    log_event_bytes("SEND", clients_[fd].ip.c_str(), clients_[fd].port, clients_[fd].name, payload.data(), payload.size());
    clients_[fd].out += frame;
    flush(fd);
}

// Sends queued output. While some is left (the client is slow to read) we watch
// EPOLLOUT, so the rest goes out later without blocking other clients.
// false = the connection failed and was closed.
bool Server::flush(int fd) {
    Client& c = clients_[fd];
    while (!c.out.empty()) {
        ssize_t n = send(fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (n > 0) { c.out.erase(0, n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // socket buffer full
        close_client(fd, errstr("send"));
        return false;
    }

    bool want = !c.out.empty();
    if (want != c.want_write) {
        epoll_event ev{};
        ev.events = EPOLLIN | (want ? EPOLLOUT : 0u);
        ev.data.fd = fd;
        if (epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev) < 0) {
            close_client(fd, errstr("epoll_ctl(MOD)"));
            return false;
        }
        c.want_write = want;
        log_note("%s: %s", tag(fd).c_str(), want ? "output queued, peer is slow to read" : "queued output sent");
    }
    return true;
}

// Stored messages are kept: users do not have to be connected to receive mail.
void Server::close_client(int fd, const std::string& reason) {
    Client c = clients_[fd];
    std::string who = tag(fd);
    if (!clients_[fd].in.empty()) // halve finished frame
        log_note("%s: dropped %zu bytes of an incomplete frame", who.c_str(), clients_[fd].in.size());
    epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);  // close() removes it anyway; failure is harmless
    if (close(fd) < 0 && errno != EINTR) log_note("close failed: %s", errstr("close").c_str());  // never retried on Linux
    clients_.erase(fd);

    std::string detail = "disconnected: " + reason + " (" + std::to_string(clients_.size()) + " connected)";
    log_event("CLOSE", c.ip.c_str(), c.port, c.name, detail.c_str());
}

// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }
    unsigned int port = parse_port(argv[1]);

    // Ctrl-C / kill end the server with a final log line. No SA_RESTART, so the
    // signal interrupts epoll_wait and the loop sees it immediately.
    struct sigaction sa{};
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    Server server(static_cast<uint16_t>(port));
    server.run();
    return 0;
}
