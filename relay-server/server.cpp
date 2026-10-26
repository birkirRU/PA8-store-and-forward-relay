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
#include <optional>
#include <string>
#include <ctime>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "log_event.h"
#include "identity.h"
#include "server.h"

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
        log_event("DROP",
            clients_[fd].ip.c_str(),
            clients_[fd].port,
            clients_[fd].name.empty() ? nullptr : clients_[fd].name.c_str(),
            detail.c_str());
    }

    for (const std::string& p : payloads) {
        if (!clients_.count(fd)) break;  // a failed reply closed the connection
        handle_command(fd, p);
    }
}

void Server::connect_peer(const std::string& ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { log_note("socket failed: %s", strerror(errno)); return; }

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
    c.outgoing = true;
    c.want_write = true;                       // we registered EPOLLOUT; flush() will turn it off
    clients_[fd] = c;

}

void Server::finish_connect(int fd) {
    int err = 0;
    socklen_t len = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);   // did the connect succeed?
    if (err != 0) { close_client(fd, std::string("connect failed: ") + strerror(err)); return; }

    Client& c = clients_[fd];
    c.connecting = false;
    c.sent_helo = true;
    log_event("CONN", c.ip.c_str(), c.port, nullptr, "outgoing connection established");
    reply(fd, "HELO," GROUP_ID);
}

std::string Server::servers_list() const {
    std::string out;
    for (const auto& kv : clients_) {
        const Client& c = kv.second;
        if (c.type.value == ConnType::peer && c.outgoing && !c.connecting && !c.name.empty())
            out += c.name + ',' + c.ip + ',' + std::to_string(c.port) + ';';
    }
    return out;
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
    log_event_bytes("SEND", clients_[fd].ip.c_str(), clients_[fd].port, clients_[fd].name.empty() ? nullptr : clients_[fd].name.c_str(), payload.data(), payload.size());
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
    log_event("CLOSE", c.ip.c_str(), c.port, c.name.empty() ? nullptr : c.name.c_str(), detail.c_str());
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
