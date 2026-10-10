#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "framework.hpp"
#include "helpers.hpp"

using posix_tests::exit_code;
using posix_tests::TempDir;

namespace {

sockaddr_un address_of(const char *path) {
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path, sizeof address.sun_path - 1);
    return address;
}

}

TEST(sockets, a_stream_socketpair_carries_data_both_ways) {
    int pair[2];
    CHECK_OK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    CHECK_EQ(write(pair[0], "ping", 4), 4L);
    char buffer[8] = {};
    CHECK_EQ(read(pair[1], buffer, sizeof buffer), 4L);
    CHECK_EQ(std::string(buffer), std::string("ping"));
    CHECK_EQ(write(pair[1], "pong", 4), 4L);
    CHECK_EQ(read(pair[0], buffer, sizeof buffer), 4L);
    CHECK_EQ(std::string(buffer, 4), std::string("pong"));
}

TEST(sockets, datagrams_keep_their_boundaries) {
    int pair[2];
    CHECK_OK(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair));
    CHECK_EQ(send(pair[0], "one", 3, 0), 3L);
    CHECK_EQ(send(pair[0], "second", 6, 0), 6L);
    char buffer[16] = {};
    CHECK_EQ(recv(pair[1], buffer, sizeof buffer, 0), 3L);
    CHECK_EQ(recv(pair[1], buffer, sizeof buffer, 0), 6L);
    CHECK_EQ(std::string(buffer, 6), std::string("second"));
}

TEST(sockets, shutdown_gives_the_peer_eof) {
    int pair[2];
    CHECK_OK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    CHECK_OK(shutdown(pair[0], SHUT_WR));
    char byte;
    CHECK_EQ(read(pair[1], &byte, 1), 0L);
    CHECK_EQ(write(pair[1], "x", 1), 1L);
    CHECK_EQ(read(pair[0], &byte, 1), 1L);
}

TEST(sockets, a_named_socket_accepts_a_client) {
    TempDir dir;
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address = address_of("server.sock");
    CHECK_OK(bind(server, reinterpret_cast<sockaddr *>(&address), sizeof address));
    CHECK_OK(listen(server, 4));
    struct stat info = {};
    CHECK_OK(stat("server.sock", &info));
    CHECK(S_ISSOCK(info.st_mode));
    pid_t child = fork();
    if (child == 0) {
        int client = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(client, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) _exit(1);
        char reply[8] = {};
        if (write(client, "hello", 5) != 5) _exit(2);
        if (read(client, reply, sizeof reply) != 5) _exit(3);
        _exit(std::string(reply) == "HELLO" ? 0 : 4);
    }
    int connection = accept(server, nullptr, nullptr);
    CHECK(connection >= 0);
    char buffer[8] = {};
    CHECK_EQ(read(connection, buffer, sizeof buffer), 5L);
    for (char &c : buffer) c = (char)toupper(c);
    CHECK_EQ(write(connection, buffer, 5), 5L);
    CHECK_EQ(exit_code(child), 0);
    close(connection);
    close(server);
}

TEST(sockets, connecting_to_a_missing_socket_fails) {
    TempDir dir;
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address = address_of("absent.sock");
    CHECK_ERRNO(connect(client, reinterpret_cast<sockaddr *>(&address), sizeof address), ENOENT);
    close(client);
}

TEST(sockets, binding_an_existing_path_is_eaddrinuse) {
    TempDir dir;
    int first = socket(AF_UNIX, SOCK_STREAM, 0);
    int second = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address = address_of("taken.sock");
    CHECK_OK(bind(first, reinterpret_cast<sockaddr *>(&address), sizeof address));
    CHECK_ERRNO(bind(second, reinterpret_cast<sockaddr *>(&address), sizeof address), EADDRINUSE);
}

TEST(sockets, getsockname_and_getpeername_return_the_path) {
    TempDir dir;
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address = address_of("named.sock");
    CHECK_OK(bind(server, reinterpret_cast<sockaddr *>(&address), sizeof address));
    CHECK_OK(listen(server, 1));
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK_OK(connect(client, reinterpret_cast<sockaddr *>(&address), sizeof address));
    sockaddr_un seen = {};
    socklen_t length = sizeof seen;
    CHECK_OK(getsockname(server, reinterpret_cast<sockaddr *>(&seen), &length));
    CHECK_EQ(std::string(seen.sun_path), std::string("named.sock"));
    length = sizeof seen;
    seen = {};
    CHECK_OK(getpeername(client, reinterpret_cast<sockaddr *>(&seen), &length));
    CHECK_EQ(std::string(seen.sun_path), std::string("named.sock"));
}

TEST(sockets, scm_rights_passes_an_open_file) {
    TempDir dir;
    int fd = open("passed", O_RDWR | O_CREAT, 0644);
    CHECK_EQ(write(fd, "via socket", 10), 10L);
    int pair[2];
    CHECK_OK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    char payload = 'f';
    iovec iov = {&payload, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message = {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &fd, sizeof fd);
    CHECK_EQ(sendmsg(pair[0], &message, 0), 1L);
    close(fd);

    char received_payload = 0;
    iovec in = {&received_payload, 1};
    alignas(cmsghdr) char received_control[CMSG_SPACE(sizeof(int))] = {};
    msghdr reply = {};
    reply.msg_iov = &in;
    reply.msg_iovlen = 1;
    reply.msg_control = received_control;
    reply.msg_controllen = sizeof received_control;
    CHECK_EQ(recvmsg(pair[1], &reply, 0), 1L);
    cmsghdr *got = CMSG_FIRSTHDR(&reply);
    CHECK(got != nullptr);
    if (!got) return;
    CHECK_EQ(got->cmsg_type, SCM_RIGHTS);
    int passed = -1;
    std::memcpy(&passed, CMSG_DATA(got), sizeof passed);
    char buffer[11] = {};
    CHECK_EQ(pread(passed, buffer, 10, 0), 10L);
    CHECK_EQ(std::string(buffer), std::string("via socket"));
}

TEST(sockets, a_nonblocking_socket_with_nothing_to_read_is_eagain) {
    int pair[2];
    CHECK_OK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair));
    char byte;
    CHECK_ERRNO(recv(pair[0], &byte, 1, 0), EAGAIN);
    CHECK_EQ(recv(pair[0], &byte, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN, true);
}

TEST(sockets, msg_peek_leaves_the_data_queued) {
    int pair[2];
    CHECK_OK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    CHECK_EQ(write(pair[0], "peek", 4), 4L);
    char buffer[8] = {};
    CHECK_EQ(recv(pair[1], buffer, sizeof buffer, MSG_PEEK), 4L);
    std::memset(buffer, 0, sizeof buffer);
    CHECK_EQ(recv(pair[1], buffer, sizeof buffer, 0), 4L);
    CHECK_EQ(std::string(buffer), std::string("peek"));
}
