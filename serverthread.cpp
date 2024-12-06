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

#define MAX_BUFFER_SIZE 1024
#define MAX_CONNECTIONS 500
#define DEBUG_MODE 1

// Debug Logging Helper
#define LOG_DEBUG(fmt, ...)            \
    do {                               \
        if (DEBUG_MODE) {              \
            fprintf(stderr, fmt, __VA_ARGS__); \
        }                              \
    } while (0)

/**
 * @brief Logs an error and optionally exits the program.
 *
 * @param message Error message to display.
 * @param terminate If 1, exits the program with failure status.
 */
void log_error(const char *message, int terminate) {
    perror(message);
    if (terminate) {
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief Handles a client's HTTP request in a separate thread.
 */
void *process_client_request(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);
    pthread_detach(pthread_self());

    char recv_buffer[MAX_BUFFER_SIZE];
    char http_method[10], file_path[100], http_version[10];
    char response_header[MAX_BUFFER_SIZE];
    char *response_content = NULL;
    FILE *requested_file = NULL;
    int content_size = 0;
    int bytes_received = 0;

    memset(recv_buffer, 0, sizeof(recv_buffer));

    // Receive HTTP request
    bytes_received = recv(client_fd, recv_buffer, sizeof(recv_buffer) - 1, 0);
    if (bytes_received <= 0) {
        log_error("Error reading from client socket", 0);
        close(client_fd);
        return NULL;
    }
    recv_buffer[bytes_received] = '\0';

    LOG_DEBUG("Received request:\n%s\n", recv_buffer);

    if (strstr(recv_buffer, "\r\n\r\n") == NULL) {
        const char *bad_request = "HTTP/1.1 400 Bad Request\r\n\r\nIncomplete HTTP request.\r\n";
        send(client_fd, bad_request, strlen(bad_request), 0);
        close(client_fd);
        return NULL;
    }

    if (sscanf(recv_buffer, "%s %s %s", http_method, file_path, http_version) != 3) {
        const char *bad_request = "HTTP/1.1 400 Bad Request\r\n\r\nMalformed HTTP request line.\r\n";
        send(client_fd, bad_request, strlen(bad_request), 0);
        close(client_fd);
        return NULL;
    }

    // Validate HTTP method
    int is_get = strcmp(http_method, "GET") == 0;
    int is_head = strcmp(http_method, "HEAD") == 0;
    if (!is_get && !is_head) {
        const char *method_not_allowed = "HTTP/1.1 405 Method Not Allowed\r\n\r\nSupported methods: GET, HEAD.\r\n";
        send(client_fd, method_not_allowed, strlen(method_not_allowed), 0);
        close(client_fd);
        return NULL;
    }

    if (strcmp(http_version, "HTTP/1.1") != 0 && strcmp(http_version, "HTTP/1.0") != 0) {
        const char *version_not_supported = "HTTP/1.1 505 HTTP Version Not Supported\r\n\r\n";
        send(client_fd, version_not_supported, strlen(version_not_supported), 0);
        close(client_fd);
        return NULL;
    }

    // Handle file path
    if (file_path[0] == '/') {
        memmove(file_path, file_path + 1, strlen(file_path));
    }
    if (strlen(file_path) == 0) {
        strcpy(file_path, "index.html");
    }

    requested_file = fopen(file_path, "r");
    if (!requested_file) {
        const char *not_found = "HTTP/1.1 404 Not Found\r\n\r\nThe requested file was not found.\r\n";
        send(client_fd, not_found, strlen(not_found), 0);
        close(client_fd);
        return NULL;
    }

    fseek(requested_file, 0, SEEK_END);
    content_size = ftell(requested_file);
    fseek(requested_file, 0, SEEK_SET);

    snprintf(response_header, sizeof(response_header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Length: %d\r\n"
             "Content-Type: text/html\r\n"
             "Connection: close\r\n\r\n",
             content_size);
    send(client_fd, response_header, strlen(response_header), 0);

    if (is_get) {
        response_content = (char *)malloc(content_size);
        if (response_content) {
            fread(response_content, 1, content_size, requested_file);
            send(client_fd, response_content, content_size, 0);
            free(response_content);
        } else {
            const char *internal_error = "HTTP/1.1 500 Internal Server Error\r\n\r\nMemory allocation failed.\r\n";
            send(client_fd, internal_error, strlen(internal_error), 0);
        }
    }

    fclose(requested_file);
    close(client_fd);
    return NULL;
}

/**
 * @brief Initializes the server socket for listening.
 */
int initialize_server_socket(const char *address, const char *port) {
    struct addrinfo hints, *server_info;
    int server_fd;
    int opt = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(address, port, &hints, &server_info) != 0) {
        log_error("Error resolving address and port", 1);
    }

    server_fd = socket(server_info->ai_family, server_info->ai_socktype, server_info->ai_protocol);
    if (server_fd < 0) {
        log_error("Error creating server socket", 1);
    }

    // Allow reusing the address if already in use
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_error("Error setting socket options", 1);
    }

    if (bind(server_fd, server_info->ai_addr, server_info->ai_addrlen) < 0) {
        log_error("Error binding server socket", 1);
    }

    if (listen(server_fd, MAX_CONNECTIONS) < 0) {
        log_error("Error listening on server socket", 1);
    }

    freeaddrinfo(server_info);
    return server_fd;
}

/**
 * @brief Main function to start the server.
 */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <address:port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *address = strtok(argv[1], ":");
    char *port = strtok(NULL, ":");
    if (!address || !port) {
        fprintf(stderr, "Invalid format. Use <address:port>\n");
        exit(EXIT_FAILURE);
    }

    int server_fd = initialize_server_socket(address, port);
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    while (1) {
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            log_error("Error accepting client connection", 0);
            continue;
        }

        int *client_fd_ptr = (int *)malloc(sizeof(int));
        if (!client_fd_ptr) {
            log_error("Memory allocation failed", 0);
            close(client_fd);
            continue;
        }
        *client_fd_ptr = client_fd;

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, process_client_request, client_fd_ptr) != 0) {
            log_error("Error creating thread", 0);
            free(client_fd_ptr);
            close(client_fd);
        }
    }

    close(server_fd);
    return 0;
}
