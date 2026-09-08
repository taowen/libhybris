/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
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
    int code = 2;
    pid_t client = -1;
    const char *display = getenv("DISPLAY");
    if (!display || !*display) {
        fprintf(stderr, "X11_SESSION requires an existing DISPLAY\n");
        return 2;
    }
    printf("X11_SESSION display=%s server_owner=external\n", display);
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
            client = -1;
            break;
        }
        if (result < 0 && errno != EINTR) break;
        usleep(10000);
    }
    if (stopped) code = 124;
done:
    reap(client);
    printf("X11_SESSION exit=%d children_reaped=1\n", code);
    return code;
}
