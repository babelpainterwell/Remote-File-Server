#include <stdio.h>       // for printing
#include <stdlib.h>      // for exit and stuff
#include <string.h>      // string ops
#include <unistd.h>      // for close, etc
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <libgen.h>      // for dirname
#include <errno.h>
#include <pthread.h>     // for threads!

#define SERVER_PORT 2024
#define SERVER_ROOT "server_root"
#define XOR_KEY 0xAA  // just a simple XOR key for "encryption"

// We'll lock file ops so multiple clients won't break things
static pthread_mutex_t file_op_mutex = PTHREAD_MUTEX_INITIALIZER;

struct client_info {
    int sock;
};

// Quick and dirty line recv from socket
int recv_line(int sock, char *buf, int maxlen) {
    int total = 0;
    char c;
    while (total < maxlen - 1) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) {
            // no data
            break;
        }
        buf[total++] = c;
        if (c == '\n')
            break; // one line done
    }
    buf[total] = '\0';
    return total;
}

// Ensure the directories exist for the requested path
// This basically does a 'mkdir -p' style behavior.
int ensure_directories(const char *remote_path) {
    char *path_copy = strdup(remote_path);
    char *dir = dirname(path_copy);

    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", SERVER_ROOT, dir);

    char *p = full_path + strlen(SERVER_ROOT) + 1; 
    for (; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(full_path, 0777); // don't care if it fails
            *p = '/';
        }
    }
    mkdir(full_path, 0777);
    free(path_copy);
    return 0;
}

// write a meta file for permission handling
int write_meta_file(const char *full_path, char permission) {
    char meta_path[1100];
    snprintf(meta_path, sizeof(meta_path), "%s.meta", full_path);

    FILE *m = fopen(meta_path, "w");
    if (!m) return -1;
    fputc(permission, m);
    fclose(m);
    return 0;
}

// read permission from meta file, default is W if none
char read_meta_file(const char *full_path) {
    char meta_path[1100];
    snprintf(meta_path, sizeof(meta_path), "%s.meta", full_path);

    FILE *m = fopen(meta_path, "r");
    if (!m) {
        return 'W'; // default writeable if no meta
    }
    char perm = fgetc(m);
    fclose(m);
    if (perm != 'R' && perm != 'W') perm = 'W';
    return perm;
}

// XOR encryption/decryption function
void xor_data(char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        data[i] ^= XOR_KEY;
    }
}

// Handle one client connection in a separate thread
void handle_client(int client_sock) {
    char line[1024];
    int len = recv_line(client_sock, line, sizeof(line));
    if (len <= 0) {
        close(client_sock);
        return;
    }

    // We expect commands like:
    // WRITE remote_path [R|W]
    // GET remote_path
    // RM remote_path
    char *cmd = strtok(line, " \n");
    char *remote_path = strtok(NULL, " \n");

    if (!cmd || !remote_path) {
        char *err_msg = "ERROR: Missing command or path\n";
        send(client_sock, err_msg, strlen(err_msg), 0);
        close(client_sock);
        return;
    }

    if (strcmp(cmd, "WRITE") == 0) {
        char *perm_str = strtok(NULL, " \n");
        char permission = 'W'; 
        if (perm_str && (perm_str[0] == 'R' || perm_str[0] == 'W')) {
            permission = perm_str[0];
        }

        char size_line[64];
        len = recv_line(client_sock, size_line, sizeof(size_line));
        if (len <= 0) {
            perror("No size line from client for WRITE?");
            close(client_sock);
            return;
        }

        long file_size = atol(size_line);
        if (file_size <= 0) {
            perror("Invalid file size from client!");
            close(client_sock);
            return;
        }

        ensure_directories(remote_path);
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", SERVER_ROOT, remote_path);

        // lock so no one else messes with the file
        pthread_mutex_lock(&file_op_mutex);
        char existing_perm = read_meta_file(full_path);
        if (existing_perm == 'R') {
            pthread_mutex_unlock(&file_op_mutex);
            char *err = "ERROR: File is read-only\n";
            send(client_sock, err, strlen(err), 0);
            close(client_sock);
            return;
        }

        FILE *f = fopen(full_path, "wb");
        if (!f) {
            perror("Can't open file for writing on server side");
            pthread_mutex_unlock(&file_op_mutex);
            close(client_sock);
            return;
        }

        long bytes_remaining = file_size;
        char buffer[1024];
        while (bytes_remaining > 0) {
            int chunk = (bytes_remaining > 1024) ? 1024 : (int)bytes_remaining;
            int received = recv(client_sock, buffer, chunk, 0);
            if (received <= 0) {
                perror("Client stopped sending the file?");
                break;
            }
            xor_data(buffer, received); // "encrypt" data before saving
            fwrite(buffer, 1, received, f);
            bytes_remaining -= received;
        }
        fclose(f);

        // write permission metadata after finishing the file
        write_meta_file(full_path, permission);

        pthread_mutex_unlock(&file_op_mutex);

        char *okmsg = "OK\n";
        send(client_sock, okmsg, strlen(okmsg), 0);

    } else if (strcmp(cmd, "GET") == 0) {
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", SERVER_ROOT, remote_path);

        pthread_mutex_lock(&file_op_mutex);
        FILE *f = fopen(full_path, "rb");
        if (!f) {
            pthread_mutex_unlock(&file_op_mutex);
            char *err_msg = "ERROR\n";
            send(client_sock, err_msg, strlen(err_msg), 0);
            close(client_sock);
            return;
        }

        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        char size_line[64];
        snprintf(size_line, sizeof(size_line), "%ld\n", file_size);
        send(client_sock, size_line, strlen(size_line), 0);

        char buffer[1024];
        long bytes_remaining = file_size;
        while (bytes_remaining > 0) {
            size_t chunk = (bytes_remaining > 1024) ? 1024 : bytes_remaining;
            size_t read_bytes = fread(buffer, 1, chunk, f);
            if (read_bytes == 0 && ferror(f)) {
                // reading error
                break;
            }
            xor_data(buffer, read_bytes);
            send(client_sock, buffer, read_bytes, 0);
            bytes_remaining -= read_bytes;
        }

        fclose(f);
        pthread_mutex_unlock(&file_op_mutex);

    } else if (strcmp(cmd, "RM") == 0) {
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", SERVER_ROOT, remote_path);

        pthread_mutex_lock(&file_op_mutex);
        char file_perm = read_meta_file(full_path);
        if (file_perm == 'R') {
            pthread_mutex_unlock(&file_op_mutex);
            char *msg = "ERROR\n";
            send(client_sock, msg, strlen(msg), 0);
            close(client_sock);
            return;
        }

        if (remove(full_path) == 0) {
            char meta_path[1100];
            snprintf(meta_path, sizeof(meta_path), "%s.meta", full_path);
            remove(meta_path);
            pthread_mutex_unlock(&file_op_mutex);
            char *msg = "OK\n";
            send(client_sock, msg, strlen(msg), 0);
        } else {
            pthread_mutex_unlock(&file_op_mutex);
            char *msg = "ERROR\n";
            send(client_sock, msg, strlen(msg), 0);
        }

    } else {
        // unknown command
        char *err_msg = "ERROR: Unrecognized command\n";
        send(client_sock, err_msg, strlen(err_msg), 0);
    }

    close(client_sock);
}

// thread wrapper
void *thread_func(void *arg) {
    struct client_info *info = (struct client_info*)arg;
    handle_client(info->sock);
    free(info);
    return NULL;
}

int main() {
    // create server root if not exists
    mkdir(SERVER_ROOT, 0777);

    int socket_desc = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_desc < 0) {
        perror("No socket for server!");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(socket_desc, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Can't bind. Check if port is in use?");
        close(socket_desc);
        exit(EXIT_FAILURE);
    }
    if (listen(socket_desc, 10) < 0) {
        perror("Can't listen. Something's wrong.");
        close(socket_desc);
        exit(EXIT_FAILURE);
    }
    printf("Server up and listening on port %d...\n", SERVER_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t c = sizeof(client_addr);
        int client_sock = accept(socket_desc, (struct sockaddr*)&client_addr, &c);
        if (client_sock < 0) {
            perror("Accept failed. Just continue...");
            continue;
        }

        pthread_t tid;
        struct client_info *info = malloc(sizeof(struct client_info));
        info->sock = client_sock;

        if (pthread_create(&tid, NULL, thread_func, info) != 0) {
            perror("Thread creation failed!");
            close(client_sock);
            free(info);
            continue;
        }
        pthread_detach(tid);
    }

    close(socket_desc);
    return 0;
}
