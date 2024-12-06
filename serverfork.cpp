#include <stdio.h>         // For standard input/output functions like printf, perror
#include <stdlib.h>        // For general utilities like malloc, free, exit
#include <string.h>        // For string manipulation functions like memset, strcmp, strstr, etc.
#include <unistd.h>        // For POSIX operating system API functions like close, fork
#include <arpa/inet.h>     // For networking functions like inet_ntoa
#include <sys/types.h>     // For system data types like socklen_t
#include <sys/socket.h>    // For socket programming functions and structures
#include <netinet/in.h>    // For sockaddr_in structure and constants
#include <netdb.h>         // For getaddrinfo and addrinfo structure
#include <signal.h>        // For handling signals like SIGCHLD
#include <fcntl.h>         // For file control options
#include <errno.h>         // For errno and error codes

// Define the maximum buffer size for incoming requests and outgoing responses
#define MAX_BUFFER_SIZE 1024

/**
 * @brief Logs an error message and optionally terminates the program.
 *
 * @param message The error message to log.
 * @param terminate If non-zero, the program will exit after logging the error.
 */
void log_error(const char *message, int terminate) {
    perror(message); // Print the error message to stderr
    if (terminate) {  // If termination flag is set
        exit(EXIT_FAILURE); // Exit the program with a failure status
    }
}

/**
 * @brief Processes an HTTP client request.
 *
 * This function reads the HTTP request from the client, verifies its correctness,
 * serves the requested file if it exists, and sends appropriate HTTP responses.
 *
 * @param client_fd The file descriptor for the client socket.
 */
void process_client_request(int client_fd) {
    char recv_buffer[MAX_BUFFER_SIZE];       // Buffer to store the client's HTTP request
    char http_method[10], file_path[100], http_version[10]; // Variables to store HTTP method, path, and version
    FILE *requested_file;                    // File pointer to access requested files
    char response_header[MAX_BUFFER_SIZE];   // Buffer for constructing HTTP response headers
    char *response_content = NULL;           // Pointer for storing file data to send
    int content_size = 0;                    // Size of the file to be served
    int bytes_received;                      // Number of bytes received from the client

    memset(recv_buffer, 0, sizeof(recv_buffer)); // Clear the receive buffer

    // Receive data from the client socket
    bytes_received = recv(client_fd, recv_buffer, sizeof(recv_buffer) - 1, 0);

    // Check if data was not received or an error occurred
    if (bytes_received <= 0) {
        log_error("Error reading from client socket", 0); // Log the error
        close(client_fd); // Close the client socket
        return; // Return to avoid further processing
    }
    recv_buffer[bytes_received] = '\0'; // Null-terminate the received data to make it a valid C string

    /**
     * @brief Verify that the request ends with \r\n\r\n
     *
     * HTTP requests end with an empty line (\r\n\r\n) indicating the end of headers.
     * If this terminator is not present, the request is considered incomplete or malformed.
     */
    if (strstr(recv_buffer, "\r\n\r\n") == NULL) {
        // If the terminator is not found, respond with a 400 Bad Request
        char bad_request[] = "HTTP/1.1 400 Bad Request\r\n\r\nIncomplete HTTP request.\r\n";
        send(client_fd, bad_request, strlen(bad_request), 0); // Send error response
        close(client_fd); // Close the client socket
        return; // Exit the function
    }

    // Parse the HTTP request line into method, path, and version
    if (sscanf(recv_buffer, "%s %s %s", http_method, file_path, http_version) != 3) {
        // If the request does not contain exactly three parts (malformed request line)
        char bad_request[] = "HTTP/1.1 400 Bad Request\r\n\r\nMalformed request line.\r\n";
        send(client_fd, bad_request, strlen(bad_request), 0); // Send error response
        close(client_fd); // Close the client socket
        return; // Exit the function
    }

    /**
     * @brief Validate the HTTP method
     *
     * Only GET and HEAD methods are supported. If the method is not one of these,
     * respond with a 405 Method Not Allowed.
     */
    int is_get = strcmp(http_method, "GET") == 0;   // Check if the method is "GET"
    int is_head = strcmp(http_method, "HEAD") == 0; // Check if the method is "HEAD"
    if (!is_get && !is_head) { // If the method is not supported
        char method_not_allowed[] = "HTTP/1.1 405 Method Not Allowed\r\n\r\nSupported methods: GET, HEAD.\r\n";
        send(client_fd, method_not_allowed, strlen(method_not_allowed), 0); // Send error response
        close(client_fd); // Close the client socket
        return; // Exit the function
    }

    /**
     * @brief Validate the HTTP version
     *
     * Only HTTP/1.1 and HTTP/1.0 are supported. If the version is not one of these,
     * respond with a 505 HTTP Version Not Supported.
     */
    if (strcmp(http_version, "HTTP/1.1") != 0 && strcmp(http_version, "HTTP/1.0") != 0) {
        char version_not_supported[] = "HTTP/1.1 505 HTTP Version Not Supported\r\n\r\n";
        send(client_fd, version_not_supported, strlen(version_not_supported), 0); // Send error response
        close(client_fd); // Close the client socket
        return; // Exit the function
    }

    /**
     * @brief For HTTP/1.1, the Host header is mandatory
     *
     * Check if the Host header is present in the request. If not, respond with a 400 Bad Request.
     */
    if (strcmp(http_version, "HTTP/1.1") == 0) {
        // Look for "Host:" header preceded by \r\n or \n to ensure it's a header line
        if (strstr(recv_buffer, "\r\nHost:") == NULL && strstr(recv_buffer, "\nHost:") == NULL) {
            // If "Host:" header is not found
            char missing_host[] = "HTTP/1.1 400 Bad Request\r\n\r\nHost header is required.\r\n";
            send(client_fd, missing_host, strlen(missing_host), 0); // Send error response
            close(client_fd); // Close the client socket
            return; // Exit the function
        }
    }

    /**
     * @brief Adjust the file path
     *
     * Remove the leading '/' from the file path to map it correctly to the local file system.
     * If no specific file is requested, default to "index.html".
     */
    if (file_path[0] == '/') {
        memmove(file_path, file_path + 1, strlen(file_path)); // Remove the leading '/'
    }
    if (strlen(file_path) == 0) { // If no specific file is requested
        strcpy(file_path, "index.html"); // Default to "index.html"
    }

    /**
     * @brief Open the requested file
     *
     * Attempt to open the requested file in read mode. If the file does not exist,
     * respond with a 404 Not Found.
     */
    requested_file = fopen(file_path, "r");
    if (!requested_file) { // If the file does not exist
        char not_found[] = "HTTP/1.1 404 Not Found\r\n\r\nThe requested file does not exist.\r\n";
        send(client_fd, not_found, strlen(not_found), 0); // Send 404 error response
    } else {
        // Determine the size of the file
        fseek(requested_file, 0, SEEK_END); // Move to the end of the file
        content_size = ftell(requested_file); // Get the size of the file
        fseek(requested_file, 0, SEEK_SET); // Move back to the beginning of the file

        /**
         * @brief Construct the HTTP response header
         *
         * The response header includes the status line, Content-Length, Connection status,
         * and Content-Type. It is terminated with an extra \r\n to indicate the end of headers.
         */
        snprintf(response_header, sizeof(response_header),
                 "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\nContent-Type: text/html\r\n\r\n",
                 content_size);
        send(client_fd, response_header, strlen(response_header), 0); // Send the response header

        /**
         * @brief If the request method is GET, send the file content
         *
         * For HEAD requests, only the headers are sent without the body.
         */
        if (is_get) {
            // Allocate memory for the file content with explicit cast to char*
            response_content = (char*) malloc(content_size);
            if (response_content) { // Check if memory allocation was successful
                size_t read_size = fread(response_content, 1, content_size, requested_file); // Read the file content
                if (read_size > 0) { // If reading was successful
                    send(client_fd, response_content, read_size, 0); // Send the file content
                }
                free(response_content); // Free the allocated memory
            } else {
                // If memory allocation failed, respond with a 500 Internal Server Error
                char internal_error[] = "HTTP/1.1 500 Internal Server Error\r\n\r\nMemory allocation failed.\r\n";
                send(client_fd, internal_error, strlen(internal_error), 0); // Send error response
            }
        }

        fclose(requested_file); // Close the file
    }

    close(client_fd); // Close the client socket after responding
}

/**
 * @brief Initializes the server socket.
 *
 * This function sets up the server socket by resolving the address and port,
 * creating the socket, binding it, and setting it to listen for incoming connections.
 *
 * @param address The IP address to bind to (can be NULL for any address).
 * @param port The port number to listen on.
 * @return The file descriptor for the server socket.
 */
int initialize_server_socket(const char *address, const char *port) {
    struct addrinfo hints, *server_info; // Structures for address information
    int server_fd; // File descriptor for the server socket

    memset(&hints, 0, sizeof(hints)); // Clear the hints structure
    hints.ai_family = AF_UNSPEC; // Allow both IPv4 and IPv6
    hints.ai_socktype = SOCK_STREAM; // Use TCP
    hints.ai_flags = AI_PASSIVE; // Bind to the given address

    // Resolve the address and port
    int gai_result = getaddrinfo(address, port, &hints, &server_info);
    if (gai_result != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai_result)); // Print the getaddrinfo error
        exit(EXIT_FAILURE); // Exit with failure status
    }

    // Create a socket
    server_fd = socket(server_info->ai_family, server_info->ai_socktype, server_info->ai_protocol);
    if (server_fd < 0) {
        log_error("Error creating server socket", 1); // Log and terminate if socket creation fails
    }

    // Bind the socket to the specified address and port
    if (bind(server_fd, server_info->ai_addr, server_info->ai_addrlen) < 0) {
        if (errno == EACCES) { // Permission denied
            fprintf(stderr, "Permission denied: Cannot bind to port %s. Try using a port number above 1024.\n", port);
        } else if (errno == EADDRINUSE) { // Address already in use
            fprintf(stderr, "Address already in use: Port %s is already in use.\n", port);
        } else { // Other errors
            log_error("Error binding server socket", 1);
        }
        freeaddrinfo(server_info); // Free the address information memory
        exit(EXIT_FAILURE); // Exit with failure status
    }

    // Set the socket to listen for incoming connections
    if (listen(server_fd, 100) < 0) {
        log_error("Error listening on server socket", 1); // Log and terminate if listening fails
    }

    freeaddrinfo(server_info); // Free the address information memory
    return server_fd; // Return the server socket descriptor
}

/**
 * @brief The main function to start the server.
 *
 * This function parses command-line arguments to obtain the address and port,
 * initializes the server socket, and enters an infinite loop to accept and handle client connections.
 *
 * @param argc The number of command-line arguments.
 * @param argv The array of command-line argument strings.
 * @return int Exit status of the program.
 */
int main(int argc, char *argv[]) {
    // Ensure the correct number of arguments is provided
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <address:port>\n", argv[0]); // Print usage instructions
        exit(EXIT_FAILURE); // Exit with failure status
    }

    // Parse the address and port from the input argument
    char *address = strtok(argv[1], ":"); // Extract the address part
    char *port = strtok(NULL, ":");       // Extract the port part
    if (!address || !port) { // Check if both address and port are present
        fprintf(stderr, "Invalid format. Use <address:port>\n");
        exit(EXIT_FAILURE); // Exit with failure status
    }

    signal(SIGCHLD, SIG_IGN); // Prevent zombie processes by ignoring child process termination

    int server_fd = initialize_server_socket(address, port); // Initialize the server socket
    struct sockaddr_in client_addr; // Structure to hold client address information
    socklen_t client_addr_len = sizeof(client_addr); // Size of the client address structure

    // Enter an infinite loop to handle incoming connections
    while (1) {
        // Accept an incoming client connection
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) { // Check if accept fails
            log_error("Error accepting client connection", 0); // Log the error and continue
            continue; // Skip to the next iteration
        }

        pid_t pid = fork(); // Fork a new process to handle the client
        if (pid < 0) { // Check if fork failed
            log_error("Error creating process for client", 0); // Log the error and continue
        } else if (pid == 0) { // Child process
            close(server_fd); // Close the server socket in the child process
            process_client_request(client_fd); // Handle the client's request
            exit(EXIT_SUCCESS); // Exit the child process successfully
        } else { // Parent process
            close(client_fd); // Close the client socket in the parent process
        }
    }

    close(server_fd); // Close the server socket (never reached in this implementation)
    return 0; // Return success
}
