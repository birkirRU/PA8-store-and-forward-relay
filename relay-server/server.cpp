#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
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

Server::Server(uint16_t port): listening_port_(port) {
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


    // get listening address of local server
    own_ip_ = find_own_ip(INSTRUCTOR_IP.c_str(), INSTRUCTOR_PORT);   // the constants already used for the first connection
    log_note("own address: %s", own_ip_.empty() ? "unknown" : own_ip_.c_str());

    const std::string detail = "listening on port: " + std::to_string(port);
    log_event("START", NULL, port, GROUP_ID, detail.c_str());

    ep_ = epoll_create1(0);
    if (ep_ < 0) fatal(errstr("epoll_create1"));
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    if (epoll_ctl(ep_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) fatal(errstr("epoll_ctl(ADD listen)"));
    
    // Register timer_fd_ to EPOLL instance
    timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (timer_fd_ < 0) fatal(errstr("timerfd_create"));

    itimerspec its{};
    its.it_value.tv_sec = 5;       // first tick after 5 s
    its.it_interval.tv_sec = 5;    // then every 5 s
    if (timerfd_settime(timer_fd_, 0, &its, nullptr) < 0) fatal(errstr("timerfd_settime"));

    epoll_event tev{};
    tev.events = EPOLLIN;
    tev.data.fd = timer_fd_;
    if (epoll_ctl(ep_, EPOLL_CTL_ADD, timer_fd_, &tev) < 0) fatal(errstr("epoll_ctl(ADD timer)"));


    spare_fd_ = open("/dev/null", O_RDONLY);

}

void Server::run() {
    connect_peer(INSTRUCTOR_IP, INSTRUCTOR_PORT);

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
            if (fd == timer_fd_) { on_timer(); continue; }
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
    queue_send(fd, "HELO," GROUP_ID);
}

std::string Server::servers_list() const {
    std::string out = std::string(GROUP_ID) + ',' + own_ip_ + ',' + std::to_string(listening_port_) + ';' ; // list yourself first
    for (const auto& kv : clients_) {
        const Client& c = kv.second;
        if (c.type.value == ConnType::peer && c.outgoing && !c.connecting && !c.name.empty())
            out += c.name + ',' + c.ip + ',' + std::to_string(c.port) + ';';
    }
    return out;
}

// Queues a framed message/data and sends what the socket takes now.
void Server::queue_send(int fd, const std::string& payload) {
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

void Server::on_timer() {
    uint64_t expirations;
    if (read(timer_fd_, &expirations, sizeof expirations) < 0) return;   // must read, or epoll fires again

    time_t now = time(nullptr);

    // reply() can close a connection and erase it from clients_,
    // so collect the fds first and don't loop over the map while sending.
    std::vector<int> due;
    for (const auto& kv : clients_) {
        const Client& c = kv.second;
        if (c.type.value == ConnType::peer && !c.connecting && !c.name.empty() &&
            now - c.last_keepalive >= 60)
            due.push_back(kv.first);
    }

    for (int fd : due) {
        if (!clients_.count(fd)) continue;           // closed by an earlier queue_send() due to flush()
        Client& c = clients_[fd];
        c.last_keepalive = now;                      // set first, so a failed send is not retried every tick

        auto box = mailboxes_.find(c.name);
        size_t waiting = (box == mailboxes_.end()) ? 0 : box->second.size();
        queue_send(fd, "KEEPALIVE," + std::to_string(waiting));
    }
}

// Local address the kernel would use to reach `via_ip`. No packet is sent.
std::string Server::find_own_ip(const char* via_ip, uint16_t via_port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return "";

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(via_port);
    if (inet_pton(AF_INET, via_ip, &dst.sin_addr) != 1) { close(s); return ""; }

    std::string ip;
    if (connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof dst) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof local;
        char buf[INET_ADDRSTRLEN];
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0 &&
            inet_ntop(AF_INET, &local.sin_addr, buf, sizeof buf))
            ip = buf;
    }
    close(s);
    return ip;
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
