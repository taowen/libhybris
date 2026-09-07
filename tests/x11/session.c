/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop(int signal) { stopped = signal; }
static void reap(pid_t pid) {
    if (pid <= 0) return;
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction action = {.sa_handler = stop};
    sigaction(SIGTERM, &action, NULL); sigaction(SIGINT, &action, NULL);
    sigaction(SIGALRM, &action, NULL); alarm(25);
    int code = 2, ready[2] = {-1, -1};
    pid_t server = -1, client = -1;
    const char *path = "x11.sock";
    int xlib = getenv("HYBRIS_X11_XLIB") != NULL;
    char display_name[24]; snprintf(display_name, sizeof(display_name), ":%u", 1000u + (unsigned)getpid() % 30000u);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, path);
    socklen_t address_length = sizeof(address);
    if (xlib) {
        address.sun_path[0] = 0;
        snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1, "/tmp/.X11-unix/X%s", display_name + 1);
        address_length = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(address.sun_path + 1);
        setenv("DISPLAY", display_name, 1);
    }
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, address_length) ||
        listen(listener, 8) || pipe2(ready, O_CLOEXEC)) goto done;
    server = fork();
    if (server < 0) goto done;
    if (!server) {
        setpgid(0, 0);
        char fd[24], notify[24];
        snprintf(fd, sizeof(fd), "%d", listener);
        snprintf(notify, sizeof(notify), "%d", ready[1]);
        fcntl(listener, F_SETFD, 0); fcntl(ready[1], F_SETFD, 0);
        int log = open("xwayland.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (log < 0 || dup2(log, 1) < 0 || dup2(log, 2) < 0) _exit(126);
        close(log);
        const char *xkb = getenv("XKB_CONFIG_ROOT");
        if (!xkb) _exit(126);
        const char *server_path = getenv("HYBRIS_X11_SERVER");
        if (!server_path) _exit(126);
        execl(server_path, "Xwayland", display_name, "-listenfd", fd,
              "-displayfd", notify, "-nolisten", "tcp", "-nolisten", "unix",
              "-nolock", "-ac", "-geometry", "320x240", "-xkbdir", xkb, (char *)NULL);
        _exit(127);
    }
    setpgid(server, server);
    close(listener); listener = -1;
    close(ready[1]); ready[1] = -1;
    struct pollfd wait = {.fd = ready[0], .events = POLLIN};
    int got = poll(&wait, 1, 10000);
    char display[32];
    if (got <= 0 || !(wait.revents & POLLIN) || read(ready[0], display, sizeof(display)) <= 0) {
        printf("X11_SESSION server=NOT_READY\n"); goto done;
    }
    printf("X11_SESSION server_pid=%ld ready=1\n", (long)server);
    client = fork();
    if (client < 0) goto done;
    if (!client) {
        setpgid(0, 0);
        execv(argv[1], argv + 1);
        _exit(127);
    }
    setpgid(client, client);
    int status;
    while (!stopped) {
        pid_t result = waitpid(client, &status, WNOHANG);
        if (result == client) {
            code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            break;
        }
        if (result < 0 && errno != EINTR) break;
        usleep(10000);
    }
    if (stopped) code = 124;
done:
    reap(client); reap(server);
    if (listener >= 0) close(listener);
    if (ready[0] >= 0) close(ready[0]);
    if (ready[1] >= 0) close(ready[1]);
    unlink(path);
    printf("X11_SESSION exit=%d children_reaped=1\n", code);
    return code;
}
