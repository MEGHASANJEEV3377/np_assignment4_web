#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <errno.h>

#define MAX_BUFFER_SIZE 8192
#define MAX_PATH_DEPTH 2
#define RECV_TIMEOUT_MS 5000
#define MAX_RECV_ATTEMPTS 100

void log_error(const char *msg, int terminate) {
    perror(msg);
    if (terminate) exit(EXIT_FAILURE);
}

// ✅ MIME type detection function
const char *get_mime_type(const char *filename) {
    if (strstr(filename, ".html")) return "text/html";
    if (strstr(filename, ".htm")) return "text/html";
    if (strstr(filename, ".txt")) return "text/plain";
    if (strstr(filename, ".jpg")) return "image/jpeg";
    if (strstr(filename, ".jpeg")) return "image/jpeg";
    if (strstr(filename, ".png")) return "image/png";
    if (strstr(filename, ".css")) return "text/css";
    if (strstr(filename, ".js")) return "application/javascript";
    if (strstr(filename, ".json")) return "application/json";
    if (strstr(filename, ".pdf")) return "application/pdf";
    return "application/octet-stream";
}

void *process_client_request(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);
    pthread_detach(pthread_self());

    char recv_buffer[MAX_BUFFER_SIZE];
    char http_method[10], file_path[256], http_version[10];
    char response_header[MAX_BUFFER_SIZE];
    char *response_content = NULL;
    FILE *requested_file = NULL;
    int content_size = 0, total_received = 0;

    memset(recv_buffer, 0, sizeof(recv_buffer));
    struct timeval timeout;
    timeout.tv_sec = RECV_TIMEOUT_MS / 1000;
    timeout.tv_usec = (RECV_TIMEOUT_MS % 1000) * 1000;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    int attempts = 0;
    while (total_received < MAX_BUFFER_SIZE - 1 && attempts++ < MAX_RECV_ATTEMPTS) {
        ssize_t n = recv(client_fd, recv_buffer + total_received, MAX_BUFFER_SIZE - 1 - total_received, 0);
        if (n <= 0) break;
        total_received += n;
        recv_buffer[total_received] = '\0';
        if (strstr(recv_buffer, "\r\n\r\n")) break;
    }

    if (!strstr(recv_buffer, "\r\n\r\n")) {
        close(client_fd);
        return NULL;
    }

    if (sscanf(recv_buffer, "%9s %255s %9s", http_method, file_path, http_version) != 3) {
        const char *badline = "HTTP/1.1 400 Bad Request\r\n\r\nMalformed request line.\r\n";
        send(client_fd, badline, strlen(badline), 0);
        close(client_fd);
        return NULL;
    }

    if (strcmp(http_method, "GET") != 0 && strcmp(http_method, "HEAD") != 0) {
        const char *bad_method = "HTTP/1.1 405 Method Not Allowed\r\n\r\nSupported methods: GET, HEAD.\r\n";
        send(client_fd, bad_method, strlen(bad_method), 0);
        close(client_fd);
        return NULL;
    }

    if (strcmp(http_version, "HTTP/1.1") != 0 && strcmp(http_version, "HTTP/1.0") != 0) {
        const char *bad_version = "HTTP/1.1 505 HTTP Version Not Supported\r\n\r\n";
        send(client_fd, bad_version, strlen(bad_version), 0);
        close(client_fd);
        return NULL;
    }

    int slash_count = 0;
    for (size_t i = 0; i < strlen(file_path); ++i) {
        if (file_path[i] == '/') slash_count++;
    }
    if (slash_count > MAX_PATH_DEPTH || strstr(file_path, "..")) {
        const char *bad_path = "HTTP/1.1 403 Forbidden\r\n\r\nInvalid path.\r\n";
        send(client_fd, bad_path, strlen(bad_path), 0);
        close(client_fd);
        return NULL;
    }

    if (file_path[0] == '/') memmove(file_path, file_path + 1, strlen(file_path));
    if (strlen(file_path) == 0) strcpy(file_path, "index.html");

    // ✅ Open file in binary mode
    requested_file = fopen(file_path, "rb");
    if (!requested_file) {
        const char *not_found = "HTTP/1.1 404 Not Found\r\n\r\nThe requested file was not found.\r\n";
        send(client_fd, not_found, strlen(not_found), 0);
        close(client_fd);
        return NULL;
    }

    fseek(requested_file, 0, SEEK_END);
    content_size = ftell(requested_file);
    fseek(requested_file, 0, SEEK_SET);

    // ✅ Detect correct MIME type
    const char *mime_type = get_mime_type(file_path);

    snprintf(response_header, sizeof(response_header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Length: %d\r\n"
             "Content-Type: %s\r\n"
             "Connection: close\r\n\r\n",
             content_size, mime_type);
    send(client_fd, response_header, strlen(response_header), 0);

    if (strcmp(http_method, "GET") == 0) {
        response_content = (char *)malloc(content_size);
        if (response_content) {
            size_t read_size = fread(response_content, 1, content_size, requested_file);
            size_t total_sent = 0;
            while (total_sent < read_size) {
                ssize_t sent = send(client_fd, response_content + total_sent, read_size - total_sent, 0);
                if (sent <= 0) break;
                total_sent += sent;
            }
            free(response_content);
        } else {
            const char *error = "HTTP/1.1 500 Internal Server Error\r\n\r\nMemory allocation failed.\r\n";
            send(client_fd, error, strlen(error), 0);
        }
    }

    fclose(requested_file);
    close(client_fd);
    return NULL;
}

int initialize_server_socket(const char *address, const char *port) {
    struct addrinfo hints, *server_info;
    int server_fd;
    int opt = 1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(address, port, &hints, &server_info) != 0)
        log_error("getaddrinfo failed", 1);

    server_fd = socket(server_info->ai_family, server_info->ai_socktype, server_info->ai_protocol);
    if (server_fd < 0)
        log_error("socket creation failed", 1);

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        log_error("setsockopt failed", 1);

    if (bind(server_fd, server_info->ai_addr, server_info->ai_addrlen) < 0)
        log_error("bind failed", 1);

    if (listen(server_fd, 100) < 0)
        log_error("listen failed", 1);

    freeaddrinfo(server_info);
    return server_fd;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <address:port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *input = strdup(argv[1]);
    char *address = strtok(input, ":");
    char *port = strtok(NULL, ":");

    if (!address || !port) {
        fprintf(stderr, "Invalid address:port format\n");
        exit(EXIT_FAILURE);
    }

    int server_fd = initialize_server_socket(address, port);
    printf("Server is listening on %s:%s\n", address, port);
    fflush(stdout);

    struct sockaddr_storage client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    while (1) {
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            log_error("accept failed", 0);
            continue;
        }

        printf("Accepted connection\n");
        fflush(stdout);

        int *client_fd_ptr = (int *)malloc(sizeof(int));
        if (!client_fd_ptr) {
            log_error("malloc failed", 0);
            close(client_fd);
            continue;
        }
        *client_fd_ptr = client_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, process_client_request, client_fd_ptr) != 0) {
            log_error("pthread_create failed", 0);
            free(client_fd_ptr);
            close(client_fd);
        }
    }

    close(server_fd);
    return 0;
}
