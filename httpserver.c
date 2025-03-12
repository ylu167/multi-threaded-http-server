#include "helper_funcs.h"

#include "queue.h"
#include "rwlock.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUFSIZE     4096
#define REQEX       "^([a-zA-Z]{1,8}) /([a-zA-Z0-9.-]{1,63}) (HTTP/[0-9]\\.[0-9])\r\n"
#define HEADEX      "([a-zA-Z0-9.-]{1,128}): ([ -~]{1,128})\r\n"
#define OK          "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nOK\n"
#define CREATED     "HTTP/1.1 201 Created\r\nContent-Length: 8\r\n\r\nCreated\n"
#define BAD_REQUEST "HTTP/1.1 400 Bad Request\r\nContent-Length: 12\r\n\r\nBad Request\n"
#define FORBIDDEN   "HTTP/1.1 403 Forbidden\r\nContent-Length: 10\r\n\r\nForbidden\n"
#define NOT_FOUND   "HTTP/1.1 404 Not Found\r\nContent-Length: 10\r\n\r\nNot Found\n"
#define INTERNAL_SERVER_ERROR                                                                      \
    "HTTP/1.1 500 Internal Server Error\r\nContent-Length: "                                       \
    "22\r\n\r\nInternal Server Error\n"
#define NOT_IMPLEMENTED                                                                            \
    "HTTP/1.1 501 Not Implemented\r\nContent-Length: 16\r\n\r\nNot "                               \
    "Implemented\n"
#define VERSION_NOT_SUPPORTED                                                                      \
    "HTTP/1.1 505 Version Not Supported\r\nContent-Length: 22\r\n\r\nVersion "                     \
    "Not Supported\n"

typedef struct node {
    char *filename;
    rwlock_t *rwlock;
    struct node *next;
} node_t;

typedef struct file_locks {
    node_t *head;
    node_t *tail;
    pthread_mutex_t mutex;

} file_locks_t;

file_locks_t *new_file_locks() {
    file_locks_t *file_locks = (file_locks_t *) malloc(sizeof(file_locks_t));
    file_locks->head = NULL;
    file_locks->tail = NULL;
    pthread_mutex_init(&file_locks->mutex, NULL);
    return file_locks;
}

node_t *add_node(file_locks_t *file_locks, char *filename) {

    node_t *new_node = (node_t *) malloc(sizeof(node_t));
    new_node->filename = (char *) malloc(sizeof(char) * (strlen(filename) + 1));
    strcpy(new_node->filename, filename);
    new_node->rwlock = rwlock_new(N_WAY, 1);
    new_node->next = NULL;
    pthread_mutex_lock(&file_locks->mutex);
    if (file_locks->head == NULL) {
        file_locks->head = file_locks->tail = new_node;
    } else {
        file_locks->tail->next = new_node;
        file_locks->tail = new_node;
    }
    pthread_mutex_unlock(&file_locks->mutex);
    return new_node;
}

node_t *get_node(file_locks_t *file_locks, char *filename) {
    node_t *curr = file_locks->head;
    while (curr != NULL) {
        if (strcmp(filename, curr->filename) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return add_node(file_locks, filename);
}

void file_read_lock(file_locks_t *file_locks, char *filename) {
    node_t *node = get_node(file_locks, filename);
    reader_lock(node->rwlock);
}

void file_read_unlock(file_locks_t *file_locks, char *filename) {
    node_t *node = get_node(file_locks, filename);
    reader_unlock(node->rwlock);
}
void file_write_lock(file_locks_t *file_locks, char *filename) {
    node_t *node = get_node(file_locks, filename);
    writer_lock(node->rwlock);
}

void file_write_unlock(file_locks_t *file_locks, char *filename) {
    node_t *node = get_node(file_locks, filename);
    writer_unlock(node->rwlock);
}

typedef struct Request {
    int sock_fd; // Socket file descriptor
    char *version; // HTTP version
    char *command; // Command (GET, PUT)
    char *file_name; // File name requested
    char *message_body; // Message body of the request
    int content_length; // Content length of the request
    int remaining_bytes; // Remaining bytes after reading the request
    int request_ID;
} Request;

int parse_request(Request *request, char *buf, ssize_t bytes_read);
int process_request(Request *request);
int process_get(Request *request);
int process_put(Request *request);
void *process_in_thread();

queue_t *queue;
file_locks_t *file_locks;

int main(int argc, char *argv[]) {
    if (argc != 4 && argc != 2) {
        return EXIT_FAILURE;
    }
    int threads_count = 4;
    int port;
    if (argc == 2) {
        port = strtol(argv[1], NULL, 10);
        if (errno == EINVAL) {
            fprintf(stderr, "Invalid Port\n");
            return EXIT_FAILURE;
        }
    } else if (argc == 4) {
        if (strcmp(argv[1], "-t") != 0) {
            return EXIT_FAILURE;
        }
        threads_count = strtol(argv[2], NULL, 10);
        if (errno == EINVAL) {
            fprintf(stderr, "Invalid threads\n");
            return EXIT_FAILURE;
        }
        port = strtol(argv[3], NULL, 10);
        if (errno == EINVAL) {
            fprintf(stderr, "Invalid Port\n");
            return EXIT_FAILURE;
        }
    }

    Listener_Socket socket;
    // Convert port number from string to integer

    if (errno == EINVAL) {
        fprintf(stderr, "Invalid Port\n");
        return EXIT_FAILURE;
    }
    if (listener_init(&socket, port) == -1) {
        fprintf(stderr, "Invalid Port\n");
        return EXIT_FAILURE;
    }
    queue = queue_new(threads_count);
    file_locks = new_file_locks();

    pthread_t threads[threads_count];

    for (int i = 0; i < threads_count; i++) {
        pthread_create(&threads[i], NULL, process_in_thread, NULL);
    }

    while (true) {
        int sock_fd = listener_accept(&socket);
        if (sock_fd == -1) {
            fprintf(stderr, "Unable to Establish Connection\n");
            return (EXIT_FAILURE);
        }
        uintptr_t intptr = (uintptr_t) sock_fd;
        queue_push(queue, (void *) intptr);
    }
    return EXIT_SUCCESS;
}

void *process_in_thread() {
    while (true) {
        uintptr_t intptr;
        queue_pop(queue, (void **) &intptr);
        int sock_fd = (int) intptr;
        Request request;
        request.sock_fd = sock_fd;
        char buf[BUFSIZE + 1] = { '\0' };
        // Read the request until the end of headers
        ssize_t bytes_read = read_until(sock_fd, buf, BUFSIZE, "\r\n\r\n");
        if (bytes_read == -1) {
            // Write bad request response if reading fails
            dprintf(request.sock_fd, BAD_REQUEST);
            return NULL;
        }
        if (parse_request(&request, buf, bytes_read) != EXIT_FAILURE) {
            process_request(&request);
        }
        // Close the socket connection
        close(sock_fd);
    }
    return NULL;
}

// Parse the HTTP request
int parse_request(Request *request, char *buf, ssize_t bytes_read) {
    request->request_ID = 0;
    int offset = 0;
    // Regular expression structure
    regex_t re;
    // Array to store matches
    regmatch_t matches[4];
    int rc;
    // Compile the regular expression pattern for HTTP requests
    rc = regcomp(&re, REQEX, REG_EXTENDED);
    // Execute the regular expression matching
    rc = regexec(&re, buf, 4, matches, 0);
    if (rc == 0) {
        // If match found
        // Set the command in the request structure
        request->command = buf;
        // Set the file name in the request structure
        request->file_name = buf + matches[2].rm_so;
        // Set the HTTP version in the request structure
        request->version = buf + matches[3].rm_so;

        buf[matches[1].rm_eo] = '\0';
        request->file_name[matches[2].rm_eo - matches[2].rm_so] = '\0';
        request->version[matches[3].rm_eo - matches[3].rm_so] = '\0';

        buf += matches[3].rm_eo + 2;
        offset += matches[3].rm_eo + 2;
    } else {
        // If no match found
        // Send bad request response
        dprintf(request->sock_fd, BAD_REQUEST);
        // Free the regular expression memory
        regfree(&re);
        return EXIT_FAILURE;
    }
    // Initialize content length to -1
    request->content_length = -1;
    // Compile the regular expression pattern for HTTP headers
    rc = regcomp(&re, HEADEX, REG_EXTENDED);
    // Execute the regular expression matching for headers
    rc = regexec(&re, buf, 3, matches, 0);
    // Loop through all header matches
    while (rc == 0) {
        buf[matches[1].rm_eo] = '\0';
        buf[matches[2].rm_eo] = '\0';
        // If the header is Content-Length
        if (strncmp(buf, "Content-Length", 14) == 0) {
            int value = strtol(buf + matches[2].rm_so, NULL, 10);
            if (errno == EINVAL) {
                dprintf(request->sock_fd, BAD_REQUEST);
            }
            request->content_length = value;
        } else if (strncmp(buf, "Request-Id", 11) == 0) {
            int value = strtol(buf + matches[2].rm_so, NULL, 10);
            if (errno == EINVAL) {
                dprintf(request->sock_fd, BAD_REQUEST);
            }
            request->request_ID = value;
        }
        buf += matches[2].rm_eo + 2;
        offset += matches[2].rm_eo + 2;
        rc = regexec(&re, buf, 3, matches, 0);
    }

    if ((rc != 0) && (buf[0] == '\r' && buf[1] == '\n')) {
        // Set the message body in the request structure
        request->message_body = buf + 2;
        offset += 2;
        // Calculate the remaining bytes after headers
        request->remaining_bytes = bytes_read - offset;
    } else if (rc != 0) {
        dprintf(request->sock_fd, BAD_REQUEST);
        regfree(&re);
        return EXIT_FAILURE;
    }
    regfree(&re);
    return EXIT_SUCCESS;
}

// Process the HTTP request
int process_request(Request *request) {
    // If HTTP version is not supported
    if (strncmp(request->version, "HTTP/1.1", 8) != 0) {
        // Send version not supported response
        dprintf(request->sock_fd, VERSION_NOT_SUPPORTED);
        return EXIT_FAILURE;
    } else if (strncmp(request->command, "GET", 3) == 0) {
        // Process GET request
        return process_get(request);
    } else if (strncmp(request->command, "PUT", 3) == 0) {
        // Process PUT request
        return process_put(request);
    } else {
        // Send not implemented response
        dprintf(request->sock_fd, NOT_IMPLEMENTED);
        return EXIT_FAILURE;
    }
}
// Process the GET request
int process_get(Request *request) {
    // If content length is specified in GET request
    if (request->content_length != -1) {
        // Send bad request response
        dprintf(request->sock_fd, BAD_REQUEST);

        return EXIT_FAILURE;
    }
    // If remaining bytes are present after headers
    if (request->remaining_bytes > 0) {
        // Send bad request response
        dprintf(request->sock_fd, BAD_REQUEST);
        return EXIT_FAILURE;
    }
    int fd; // File descriptor
    // If file is a directory
    if ((fd = open(request->file_name, O_RDONLY | O_DIRECTORY)) != -1) {
        // Send forbidden response
        dprintf(request->sock_fd, FORBIDDEN);
        fprintf(stderr, "GET,/%s,403,%d\n", request->file_name, request->request_ID);
        return EXIT_FAILURE;
    }
    file_read_lock(file_locks, request->file_name);
    // If file cannot be opened
    if ((fd = open(request->file_name, O_RDONLY)) == -1) {
        // If file not found
        if (errno == ENOENT) {
            // Send not found response
            dprintf(request->sock_fd, NOT_FOUND);
            fprintf(stderr, "GET,/%s,404,%d\n", request->file_name, request->request_ID);
            // If access is denied
        } else if (errno == EACCES) {
            // Send forbidden response
            fprintf(stderr, "GET,/%s,403,%d\n", request->file_name, request->request_ID);
            dprintf(request->sock_fd, FORBIDDEN);
        } else {
            // Send internal server error response
            dprintf(request->sock_fd, INTERNAL_SERVER_ERROR);
            fprintf(stderr, "GET,/%s,500,%d\n", request->file_name, request->request_ID);
        }
        file_read_unlock(file_locks, request->file_name);
        return EXIT_FAILURE;
    }
    // File status structure
    struct stat st;
    // Get file status
    fstat(fd, &st);
    // Get file size
    off_t size = st.st_size;
    // Send OK response with content length
    dprintf(request->sock_fd, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", size);
    fprintf(stderr, "GET,/%s,200,%d\n", request->file_name, request->request_ID);
    // Write file contents to socket
    int bytes_written = pass_n_bytes(fd, request->sock_fd, size);
    // If error in writing
    if (bytes_written == -1) {
        // Send internal server error response
        dprintf(request->sock_fd, INTERNAL_SERVER_ERROR);
        file_read_unlock(file_locks, request->file_name);
        return EXIT_FAILURE;
    }
    // Close the file descriptor
    close(fd);
    file_read_unlock(file_locks, request->file_name);
    return EXIT_SUCCESS;
}
// Process the PUT request
int process_put(Request *request) {
    // If content length is not specified in PUT request
    if (request->content_length == -1) {
        // Send bad request response
        dprintf(request->sock_fd, BAD_REQUEST);
        return EXIT_FAILURE;
    }
    int fd;
    int status_code = 0;
    // If file is a directory
    if ((fd = open(request->file_name, O_WRONLY | O_DIRECTORY, 0666)) != -1) {
        // Send forbidden response
        dprintf(request->sock_fd, FORBIDDEN);
        fprintf(stderr, "PUT,/%s,403,%d\n", request->file_name, request->request_ID);
        return EXIT_FAILURE;
    }
    file_write_lock(file_locks, request->file_name);
    // If file cannot be opened or created
    if ((fd = open(request->file_name, O_WRONLY | O_CREAT | O_EXCL, 0666)) == -1) {
        // If file already exists
        if (errno == EEXIST) {
            // Set status code to 200
            status_code = 200;
            // If access is denied
        } else if (errno == EACCES) {
            // Send forbidden response
            dprintf(request->sock_fd, FORBIDDEN);
            fprintf(stderr, "PUT,/%s,403,%d\n", request->file_name, request->request_ID);
            file_write_unlock(file_locks, request->file_name);
            return EXIT_FAILURE;
        } else {
            // Send internal server error response
            dprintf(request->sock_fd, INTERNAL_SERVER_ERROR);
            fprintf(stderr, "PUT,/%s,500,%d\n", request->file_name, request->request_ID);
            file_write_unlock(file_locks, request->file_name);
            return EXIT_FAILURE;
        }
        // If file is created successfully
    } else if (fd != -1) {
        // Set status code to 201
        status_code = 201;
    }
    // If file already exists
    if (status_code == 200) {
        if ((fd = open(request->file_name, O_WRONLY | O_CREAT | O_TRUNC, 0666)) == -1) {
            if (errno == EACCES) {
                dprintf(request->sock_fd, FORBIDDEN);
                fprintf(stderr, "PUT,/%s,403,%d\n", request->file_name, request->request_ID);
                file_write_unlock(file_locks, request->file_name);
                return EXIT_FAILURE;
            } else {
                dprintf(request->sock_fd, INTERNAL_SERVER_ERROR);
                fprintf(stderr, "PUT,/%s,500,%d\n", request->file_name, request->request_ID);
                file_write_unlock(file_locks, request->file_name);
                return EXIT_FAILURE;
            }
        }
    }
    // Write message body to file
    int bytes = write_n_bytes(fd, request->message_body, request->remaining_bytes);
    // If error in writing
    if (bytes == -1) {
        // Send internal server error response
        dprintf(request->sock_fd, INTERNAL_SERVER_ERROR);
        fprintf(stderr, "PUT,/%s,500,%d\n", request->file_name, request->request_ID);
        close(fd);
        file_write_unlock(file_locks, request->file_name);
        return EXIT_FAILURE;
    }
    // Calculate size of remaining data
    int remaining = request->content_length - request->remaining_bytes;
    // Write remaining data to file
    bytes = pass_n_bytes(request->sock_fd, fd, remaining);
    // If error in writing
    if (bytes == -1) {
        dprintf(request->sock_fd, INTERNAL_SERVER_ERROR);
        close(fd);
        file_write_unlock(file_locks, request->file_name);
        return EXIT_FAILURE;
    }

    if (status_code == 201) {
        dprintf(request->sock_fd, CREATED);
        fprintf(stderr, "PUT,/%s,201,%d\n", request->file_name, request->request_ID);
    } else {
        dprintf(request->sock_fd, OK);
        fprintf(stderr, "PUT,/%s,200,%d\n", request->file_name, request->request_ID);
    }
    close(fd);
    file_write_unlock(file_locks, request->file_name);
    return EXIT_SUCCESS;
}
