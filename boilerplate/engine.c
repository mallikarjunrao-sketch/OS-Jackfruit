#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTROL_PATH "/tmp/mini_runtime.sock"

typedef struct container {
    char id[32];
    pid_t pid;
    struct container *next;
} container_t;

static container_t *head = NULL;

static char stack[STACK_SIZE];

int child_fn(void *arg) {
    char **args = (char **)arg;

    chroot(args[0]);
    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);

    execvp(args[1], &args[1]);
    perror("exec");
    return 1;
}

void add_container(char *id, pid_t pid) {
    container_t *c = malloc(sizeof(container_t));
    strcpy(c->id, id);
    c->pid = pid;
    c->next = head;
    head = c;
}

container_t* find_container(char *id) {
    container_t *c = head;
    while (c) {
        if (strcmp(c->id, id) == 0)
            return c;
        c = c->next;
    }
    return NULL;
}

void handle_start(int client_fd, char *id, char *rootfs, char *cmd) {
    char *args[] = {rootfs, cmd, NULL};

    pid_t pid = clone(child_fn,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      args);

    if (pid < 0) {
        write(client_fd, "FAIL\n", 5);
        return;
    }

    add_container(id, pid);

    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd >= 0) {
        struct monitor_request req;
        strcpy(req.container_id, id);
        req.pid = pid;
        req.soft_limit_bytes = 40 * 1024 * 1024;
        req.hard_limit_bytes = 64 * 1024 * 1024;
        ioctl(fd, MONITOR_REGISTER, &req);
        close(fd);
    }

    write(client_fd, "OK\n", 3);
}

void handle_ps(int client_fd) {
    char buffer[256];
    container_t *c = head;

    while (c) {
        snprintf(buffer, sizeof(buffer), "%s : %d\n", c->id, c->pid);
        write(client_fd, buffer, strlen(buffer));
        c = c->next;
    }
}

void handle_stop(int client_fd, char *id) {
    container_t *c = find_container(id);
    if (!c) {
        write(client_fd, "NOT FOUND\n", 10);
        return;
    }

    kill(c->pid, SIGKILL);
    write(client_fd, "STOPPED\n", 8);
}

void run_supervisor() {
    int server_fd, client_fd;
    struct sockaddr_un addr;

    unlink(CONTROL_PATH);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Supervisor running...\n");

    while (1) {
        client_fd = accept(server_fd, NULL, NULL);

        char buf[256];
        read(client_fd, buf, sizeof(buf));

        char cmd[16], id[32], rootfs[128], prog[128];
        sscanf(buf, "%s %s %s %s", cmd, id, rootfs, prog);

        if (strcmp(cmd, "start") == 0) {
            handle_start(client_fd, id, rootfs, prog);
        }
        else if (strcmp(cmd, "ps") == 0) {
            handle_ps(client_fd);
        }
        else if (strcmp(cmd, "stop") == 0) {
            handle_stop(client_fd, id);
        }

        close(client_fd);
    }
}

void send_request(char *msg) {
    int sock;
    struct sockaddr_un addr;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    connect(sock, (struct sockaddr*)&addr, sizeof(addr));

    write(sock, msg, strlen(msg));

    char buf[256];
    int n = read(sock, buf, sizeof(buf)-1);
    if (n > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }

    close(sock);
}

int main(int argc, char *argv[]) {

    if (argc < 2) {
        printf("Usage\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        run_supervisor();
    }

    else if (strcmp(argv[1], "start") == 0) {
        char msg[256];
        sprintf(msg, "start %s %s %s", argv[2], argv[3], argv[4]);
        send_request(msg);
    }

    else if (strcmp(argv[1], "ps") == 0) {
        send_request("ps");
    }

    else if (strcmp(argv[1], "stop") == 0) {
        char msg[256];
        sprintf(msg, "stop %s", argv[2]);
        send_request(msg);
    }

    return 0;
}
