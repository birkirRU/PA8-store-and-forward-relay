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

std::ofstream log_file;

// One line in server.log: "[12/Oct/2026:18:23:45 +0000] <line>".
// A failing write (e.g. disk full) is reported once and never stops the server.
void logmsg(const std::string& line) {
    static bool reported = false;
    log_file << '[' << log_timestamp() << "] " << line << std::endl;  // endl flushes
    if (!log_file) {
        if (!reported) fprintf(stderr, "warning: writing server.log failed\n");
        reported = true;
        log_file.clear();
    }
}

// Client text made safe for one log line: control characters become \xNN.
std::string printable(const std::string& s) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7f && c != '\\') { out += static_cast<char>(c); continue; }
        out += "\\x";
        out += hex[c >> 4];
        out += hex[c & 0xf];
    }
    return out;
}

std::string errstr(const std::string& what) { return what + ": " + strerror(errno); }

// Startup or unrecoverable error: report on stderr and in the log, then exit.
[[noreturn]] void fatal(const std::string& why) {
    fprintf(stderr, "tsamserver: %s\n", why.c_str());
    logmsg("FATAL " + why);
    exit(1);
}

volatile sig_atomic_t stop_signal = 0;
void on_stop_signal(int sig) { stop_signal = sig; }

}  // namespace

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
class Server {
public:
    explicit Server(uint16_t port);
    void run();  // returns when SIGINT/SIGTERM arrives

private:
    struct Client {
        int id;                   // shown in the log; fds get reused, ids do not
        std::string in;           // received bytes not yet forming a complete frame
        std::string out;          // reply bytes the socket has not accepted yet
        bool want_write = false;  // watching EPOLLOUT because `out` is not empty
    };
    struct Message {
        std::string from;
        std::string text;
    };

    void accept_client();
    void read_client(int fd);
    void handle_command(int fd, const std::string& payload);
    std::string sendmsg(const std::vector<std::string>& f);
    std::string getmsg(const std::vector<std::string>& f);
    std::string statusreq();
    void reply(int fd, const std::string& text);
    bool flush(int fd);
    void close_client(int fd, const std::string& reason);
    std::string tag(int fd) { return "client " + std::to_string(clients_[fd].id); }

    int ep_ = -1;
    int listen_fd_ = -1;
    int spare_fd_ = -1;  // kept free so we can still accept-and-close when out of fds
    int next_id_ = 1;
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
        logmsg(errstr("setsockopt(SO_REUSEADDR)"));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // all interfaces
    addr.sin_port = htons(port);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0)
        fatal(errstr("bind to port " + std::to_string(port)));
    if (listen(listen_fd_, SOMAXCONN) < 0) fatal(errstr("listen"));

    ep_ = epoll_create1(0);
    if (ep_ < 0) fatal(errstr("epoll_create1"));
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    if (epoll_ctl(ep_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) fatal(errstr("epoll_ctl(ADD listen)"));

    spare_fd_ = open("/dev/null", O_RDONLY);
    logmsg("tsamserver started on port " + std::to_string(port) + " (pid " + std::to_string(getpid()) + ")");
}

void Server::run() {
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

            if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) read_client(fd);
            if (clients_.count(fd) && (events[i].events & EPOLLOUT)) flush(fd);
        }
    }
    logmsg("shutting down on signal " + std::to_string(stop_signal) + ": " +
           std::to_string(clients_.size()) + " connected, " + std::to_string(stored_) +
           " stored message(s) discarded");
}

// Level-triggered epoll: if more clients are waiting, we are told again.
void Server::accept_client() {
    int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
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
            logmsg("out of file descriptors, rejected a connection");
            return;
        }
        logmsg(errstr("accept"));
        return;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        logmsg(errstr("epoll_ctl(ADD client), connection refused"));
        close(fd);
        return;
    }
    clients_[fd] = Client{next_id_++, "", "", false};
    logmsg(tag(fd) + " connected (" + std::to_string(clients_.size()) + " connected)");
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
    for (const std::string& why : problems) logmsg(tag(fd) + ": discarded bytes: " + why);

    for (const std::string& p : payloads) {
        if (!clients_.count(fd)) break;  // a failed reply closed the connection
        handle_command(fd, p);
    }
}

void Server::handle_command(int fd, const std::string& payload) {
    logmsg(tag(fd) + " RECV " + printable(payload));

    std::string name = payload.substr(0, payload.find(','));
    std::string answer;
    if (name == "SENDMSG")        answer = sendmsg(split_fields(payload, 4));
    else if (name == "GETMSG")    answer = getmsg(split_fields(payload, 2));
    else if (name == "STATUSREQ") answer = (payload == "STATUSREQ") ? statusreq() : "";

    if (answer.empty()) {  // unknown command, or wrong fields for a known one
        logmsg(tag(fd) + ": rejected unknown or malformed command '" + printable(name) + "'");
        answer = "ERROR,command does not exist or is malformed";
    }
    reply(fd, answer);
}

// SENDMSG,<from>,<to>,<message>   -> "OK"
std::string Server::sendmsg(const std::vector<std::string>& f) {
    if (f.size() != 4 || !is_valid_name(f[1]) || !is_valid_name(f[2])) return "";
    mailboxes_[f[2]].push_back({f[1], f[3]});
    stored_++;
    logmsg("STATE stored message from " + f[1] + " for " + f[2] + " (" +
           std::to_string(mailboxes_[f[2]].size()) + " waiting for them, " +
           std::to_string(stored_) + " stored in total)");
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
    logmsg("STATE delivered message from " + m.from + " to " + f[1] + " (" + std::to_string(left) +
           " left for them, " + std::to_string(stored_) + " stored in total)");
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
void Server::reply(int fd, const std::string& text) {
    std::string frame = encode_frame(text);
    if (frame.empty()) {
        logmsg(tag(fd) + ": reply longer than " + std::to_string(MAX_PAYLOAD) + " bytes, not sent");
        return;
    }
    logmsg(tag(fd) + " SEND " + printable(text));
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
        logmsg(tag(fd) + (want ? ": output queued, client is slow to read" : ": queued output sent"));
    }
    return true;
}

// Stored messages are kept: users do not have to be connected to receive mail.
void Server::close_client(int fd, const std::string& reason) {
    std::string who = tag(fd);
    if (!clients_[fd].in.empty())
        logmsg(who + ": dropped " + std::to_string(clients_[fd].in.size()) + " bytes of an incomplete frame");
    epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);  // close() removes it anyway; failure is harmless
    if (close(fd) < 0 && errno != EINTR) logmsg(errstr("close"));  // never retried on Linux
    clients_.erase(fd);
    logmsg(who + " disconnected: " + reason + " (" + std::to_string(clients_.size()) + " connected)");
}

// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }
    unsigned int port = parse_port(argv[1]);

    // A new log per run, so the log of a demo session matches its trace and client output.
    log_file.open("server.log", std::ios::trunc);
    if (!log_file) {
        perror("cannot open server.log");
        return 1;
    }

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
