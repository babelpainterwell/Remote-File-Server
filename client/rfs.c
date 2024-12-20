#include <stdio.h>     // for printing and file ops
#include <stdlib.h>    // for exit, etc
#include <string.h>    // for string ops
#include <unistd.h>    // for close, read, etc
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>  // for file stats maybe

#define SERVER_PORT 2024
#define SERVER_ADDR "127.0.0.1"

// A simple function to receive a line (terminated by newline or EOF) from a socket
// Not fancy, just grabs char by char.
int recv_line(int sock, char *buf, int maxlen) {
    int total = 0;
    char c;
    while (total < maxlen - 1) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) {
            // no more data or error, just bail
            break;
        }
        buf[total++] = c;
        if (c == '\n')
            break;  // got a full line
    }
    buf[total] = '\0';
    return total;
}

int main(int argc, char *argv[]) {
    // just do some basic arg parsing
    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s WRITE local-file-path remote-file-path [R|W]\n", argv[0]);
        fprintf(stderr, "  %s GET remote-file-path local-file-path\n", argv[0]);
        fprintf(stderr, "  %s RM remote-file-path\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *command = argv[1];

    // If user wants to write a file to server
    if (strcmp(command, "WRITE") == 0) {
        
        if (argc < 4) {
            fprintf(stderr, "Dude, you need to provide: %s WRITE local-file remote-file [R|W]\n", argv[0]);
            exit(EXIT_FAILURE);
        }

        char *local_path = argv[2];
        char *remote_path = argv[3];
        char permission = 'W'; // Default is 'W' if not specified
        if (argc > 4) {
            // let's see if user specified R or W
            if (argv[4][0] == 'R' || argv[4][0] == 'W') {
                permission = argv[4][0];
            }
        }

        FILE *f = fopen(local_path, "rb");
        if (!f) {
            perror("Couldn't open local file. Check if it exists or permissions!");
            exit(EXIT_FAILURE);
        }

        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        // create the socket
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("No socket for you!");
            fclose(f);
            exit(EXIT_FAILURE);
        }

        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        server_addr.sin_addr.s_addr = inet_addr(SERVER_ADDR);

        // connect to server
        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("Couldn't connect to server. Is it running?");
            fclose(f);
            close(sock);
            exit(EXIT_FAILURE);
        }

        // send WRITE command line
        char cmd_line[1024];
        snprintf(cmd_line, sizeof(cmd_line), "WRITE %s %c\n", remote_path, permission);
        send(sock, cmd_line, strlen(cmd_line), 0);

        // send file size line
        char size_line[64];
        snprintf(size_line, sizeof(size_line), "%ld\n", file_size);
        send(sock, size_line, strlen(size_line), 0);

        char buffer[1024];
        long bytes_remaining = file_size;
        // read from file and send to server until done
        while (bytes_remaining > 0) {
            size_t chunk = (bytes_remaining > 1024) ? 1024 : bytes_remaining;
            size_t read_bytes = fread(buffer, 1, chunk, f);
            if (read_bytes == 0 && ferror(f)) {
                perror("Something bad happened while reading the file. Aborting!");
                break;
            }
            if (read_bytes > 0) {
                int sent = send(sock, buffer, read_bytes, 0);
                if (sent <= 0) {
                    perror("Sending data to server failed. Maybe server closed connection?");
                    break;
                }
                bytes_remaining -= read_bytes;
            } else {
                // probably EOF or something
                break;
            }
        }

        fclose(f);

        memset(buffer, 0, sizeof(buffer));
        int r = recv(sock, buffer, sizeof(buffer)-1, 0);
        if (r > 0) {
            printf("Server says: %s", buffer);
        }

        close(sock);

    } else if (strcmp(command, "GET") == 0) {
        // GET remote_file local_file
        if (argc < 4) {
            fprintf(stderr, "Dude, usage: %s GET remote-file-path local-file-path\n", argv[0]);
            exit(EXIT_FAILURE);
        }

        char *remote_path = argv[2];
        char *local_path = argv[3];

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("No socket, no talkie.");
            exit(EXIT_FAILURE);
        }

        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        server_addr.sin_addr.s_addr = inet_addr(SERVER_ADDR);

        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("Can't connect for GET. Server might be down?");
            close(sock);
            exit(EXIT_FAILURE);
        }

        char cmd_line[1024];
        snprintf(cmd_line, sizeof(cmd_line), "GET %s\n", remote_path);
        send(sock, cmd_line, strlen(cmd_line), 0);

        char line[1024];
        int len = recv_line(sock, line, sizeof(line));
        if (len <= 0) {
            fprintf(stderr, "Server isn't talking back. Something's off.\n");
            close(sock);
            exit(EXIT_FAILURE);
        }

        if (strncmp(line, "ERROR", 5) == 0) {
            fprintf(stderr, "Server says: File not found or can't open it.\n");
            close(sock);
            exit(EXIT_FAILURE);
        }

        long file_size = atol(line);
        if (file_size <= 0) {
            fprintf(stderr, "Invalid size from server. Maybe empty file?\n");
            close(sock);
            exit(EXIT_FAILURE);
        }

        FILE *f = fopen(local_path, "wb");
        if (!f) {
            perror("Can't open local file to save data. Check permissions?");
            close(sock);
            exit(EXIT_FAILURE);
        }

        long bytes_remaining = file_size;
        char buffer[1024];
        while (bytes_remaining > 0) {
            int chunk = (bytes_remaining > 1024) ? 1024 : (int)bytes_remaining;
            int received = recv(sock, buffer, chunk, 0);
            if (received <= 0) {
                perror("We got cut off while receiving the file!");
                break;
            }
            fwrite(buffer, 1, received, f);
            bytes_remaining -= received;
        }

        fclose(f);
        close(sock);

        if (bytes_remaining == 0) {
            printf("Got the file: %s\n", local_path);
        } else {
            fprintf(stderr, "File didn't fully arrive. Partial data only.\n");
        }

    } else if (strcmp(command, "RM") == 0) {
        // remove a file on the server
        if (argc < 3) {
            fprintf(stderr, "To remove: %s RM remote-file-path\n", argv[0]);
            exit(EXIT_FAILURE);
        }

        char *remote_path = argv[2];

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("No socket. Cannot remove.");
            exit(EXIT_FAILURE);
        }

        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        server_addr.sin_addr.s_addr = inet_addr(SERVER_ADDR);

        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("Connect failed. Can't send remove request.");
            close(sock);
            exit(EXIT_FAILURE);
        }

        char cmd_line[1024];
        snprintf(cmd_line, sizeof(cmd_line), "RM %s\n", remote_path);
        send(sock, cmd_line, strlen(cmd_line), 0);

        char response[1024];
        int r = recv_line(sock, response, sizeof(response));
        if (r > 0) {
            if (strncmp(response, "OK", 2) == 0) {
                printf("Server deleted the file/folder for us.\n");
            } else {
                printf("Server says: Couldn't remove it.\n");
            }
        } else {
            printf("Server gave no response at all.\n");
        }

        close(sock);

    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        exit(EXIT_FAILURE);
    }

    return 0;
}
