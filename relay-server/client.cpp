#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common.h"

namespace {

// After stdin ends, wait at most this long for replies still owed to us.
const int REPLY_TIMEOUT_MS = 5000;

void print(const char* tag, const std::string& text) {
    printf("%s %s: %s\n", timestamp().c_str(), tag, text.c_str());
}

// Connects to host:port (tries every address the name resolves to). Exits on failure.
int connect_to(const char* host, const char* port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "cannot resolve %s: %s\n", host, gai_strerror(rc));
        exit(1);
    }

    int sock = -1, err = 0;
    for (addrinfo* ai = res; ai != nullptr && sock < 0; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) { err = errno; continue; }
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
            err = errno;
            close(sock);
            sock = -1;
        }
    }
    freeaddrinfo(res);
    if (sock < 0) {
        fprintf(stderr, "cannot connect to %s:%s: %s\n", host, port, strerror(err));
        exit(1);
    }
    return sock;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <ip> <port>\n", argv[0]);
        return 1;
    }
    parse_port(argv[2]);

    signal(SIGPIPE, SIG_IGN);              // a dead server makes send() fail instead of killing us
    setvbuf(stdout, nullptr, _IOLBF, 0);   // line-buffered even when piped into client-output.txt

    int sock = connect_to(argv[1], argv[2]);
    print("INFO", std::string("connected to ") + argv[1] + ":" + argv[2]);
    print("INFO", "commands: SENDMSG,<from>,<to>,<message>  GETMSG,<user>  STATUSREQ");

    std::string input;     // typed text not yet ended by a newline
    std::string received;  // server bytes not yet forming a complete frame
    int awaiting = 0;      // commands sent that have not been answered yet (one reply each)
    bool stdin_open = true;

    pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {sock, POLLIN, 0}};

    for (;;) {
        if (!stdin_open && awaiting == 0) break;  // input done and every reply received
        fds[0].fd = stdin_open ? STDIN_FILENO : -1;  // poll ignores negative fds

        int n = poll(fds, 2, stdin_open ? -1 : REPLY_TIMEOUT_MS);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            return 1;
        }
        if (n == 0) {
            print("ERROR", "no reply to " + std::to_string(awaiting) + " command(s), giving up");
            return 1;
        }

        // The user typed something. read() on the fd, not fgets(), so no line can hide
        // in a stdio buffer where poll() does not see it.
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[1024];
            ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
            if (r < 0 && errno != EINTR) {
                perror("read stdin");
                return 1;
            }
            if (r == 0) {                        // Ctrl-D or end of piped input
                stdin_open = false;
                input += '\n';                   // send a last line that had no newline
            }
            if (r > 0) input.append(buf, r);

            size_t nl;
            while ((nl = input.find('\n')) != std::string::npos) {
                std::string line = input.substr(0, nl);
                input.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                std::string frame = encode_frame(line);
                if (frame.empty()) {
                    print("ERROR", "command longer than " + std::to_string(MAX_PAYLOAD) + " bytes, not sent");
                    continue;
                }
                if (!send_all(sock, frame)) {
                    perror("send");
                    return 1;
                }
                print("SENT", line);
                awaiting++;
            }
        }

        // The server sent something: print every complete reply.
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[4096];
            ssize_t r = recv(sock, buf, sizeof buf, 0);
            if (r == 0) {
                print("INFO", "server closed the connection");
                return awaiting == 0 ? 0 : 1;
            }
            if (r < 0) {
                if (errno == EINTR) continue;
                perror("recv");
                return 1;
            }
            received.append(buf, r);

            std::vector<std::string> replies, problems;
            extract_frames(received, replies, problems);
            for (const std::string& why : problems) print("WARNING", "discarded bytes from server: " + why);
            for (const std::string& rep : replies) {
                print("RECV", rep);
                if (awaiting > 0) awaiting--;
            }
        }
    }
    close(sock);
    return 0;
}
