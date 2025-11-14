/* peer.c - P2P Peer Application
 * Can register content, search, download, list, and deregister content.
 * Uses UDP for index server communication and TCP for content download.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/signal.h>
#include <sys/wait.h>

#include "pdu.h"

#define BUFLEN          256     /* buffer length */
#define MAX_TCP_SOCKETS 10

/* Structure to track registered content and their TCP sockets */
struct registered_content {
    char peer_name[PEER_NAME_SIZE + 1];
    char content_name[CONTENT_NAME_SIZE + 1];
    char filename[256];                 /* Filename of the content file */
    int tcp_socket;                     /* Listening TCP socket for this content */
    struct sockaddr_in tcp_addr;
    struct registered_content *next;
};

/* Global variables */
struct registered_content *reg_list = NULL;
int udp_sock = -1;
struct sockaddr_in index_server_addr;
char my_peer_name[PEER_NAME_SIZE + 1] = {0};

/* Function prototypes */
void register_content(const char *content_name, const char *filename);
void search_and_download(const char *content_name);
void list_contents(void);
void deregister_content(const char *content_name);
void deregister_all(void);
int create_tcp_socket_for_content(const char *content_name, struct sockaddr_in *addr);
void handle_tcp_connection(int tcp_sock, const char *content_name);
void handle_user_input(char *input);
void handle_udp_response(void);
void free_reg_list(void);
struct registered_content *find_registered_content(const char *content_name);

int main(int argc, char **argv)
{
    const char *index_server = "127.0.0.1";
    int index_port = 3000;
    struct hostent *hp;
    fd_set rfds, afds;
    char input[BUFLEN];
    struct registered_content *reg;
    int nready;

    /* Parse command line arguments */
    switch (argc) {
    case 1:
        break;
    case 2:
        index_server = argv[1];
        break;
    case 3:
        index_server = argv[1];
        index_port = atoi(argv[2]);
        break;
    default:
        fprintf(stderr, "Usage: %s [index_server] [index_port]\n", argv[0]);
        exit(1);
    }

    /* Get peer name */
    printf("Enter your peer name (max %d characters): ", PEER_NAME_SIZE);
    fflush(stdout);
    if (!fgets(my_peer_name, sizeof(my_peer_name), stdin)) {
        fprintf(stderr, "Failed to read peer name\n");
        exit(1);
    }
    my_peer_name[strcspn(my_peer_name, "\r\n")] = '\0';
    if (strlen(my_peer_name) == 0 || strlen(my_peer_name) > PEER_NAME_SIZE) {
        fprintf(stderr, "Invalid peer name\n");
        exit(1);
    }

    /* Setup UDP socket for index server communication */
    memset(&index_server_addr, 0, sizeof(index_server_addr));
    index_server_addr.sin_family = AF_INET;
    index_server_addr.sin_port = htons(index_port);

    hp = gethostbyname(index_server);
    if (hp != NULL) {
        memcpy(&index_server_addr.sin_addr, hp->h_addr, hp->h_length);
    } else if ((index_server_addr.sin_addr.s_addr = inet_addr(index_server)) == INADDR_NONE) {
        fprintf(stderr, "Can't get index server address\n");
        exit(1);
    }

    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        fprintf(stderr, "Can't create UDP socket\n");
        exit(1);
    }

    if (connect(udp_sock, (struct sockaddr *)&index_server_addr,
                sizeof(index_server_addr)) < 0) {
        fprintf(stderr, "Can't connect to index server\n");
        close(udp_sock);
        exit(1);
    }

    printf("Connected to index server at %s:%d\n", index_server, index_port);
    printf("Peer name: %s\n", my_peer_name);
    printf("\nCommands:\n");
    printf("  register <content_name> <filename>  - Register content\n");
    printf("  download <content_name>             - Download content\n");
    printf("  list                                - List all registered content\n");
    printf("  deregister <content_name>           - Deregister content\n");
    printf("  quit                                - Quit (auto-deregisters all)\n");
    printf("\n> ");

    /* Setup select() for stdin and UDP socket */
    FD_ZERO(&afds);
    FD_SET(0, &afds);        /* stdin */
    FD_SET(udp_sock, &afds); /* UDP socket */

    /* Main loop using select() */
    for (;;) {
        rfds = afds;

        /* Also add all TCP listening sockets to the set */
        reg = reg_list;
        while (reg) {
            if (reg->tcp_socket >= 0) {
                FD_SET(reg->tcp_socket, &rfds);
            }
            reg = reg->next;
        }

        nready = select(FD_SETSIZE, &rfds, NULL, NULL, NULL);
        if (nready < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "select error\n");
            break;
        }

        /* Check stdin */
        if (FD_ISSET(0, &rfds)) {
            if (!fgets(input, sizeof(input), stdin)) {
                break; /* EOF */
            }
            input[strcspn(input, "\r\n")] = '\0';
            if (strlen(input) == 0) {
                printf("> ");
            } else {
                handle_user_input(input);
                printf("> ");
            }
        }

        /* Check UDP socket */
        if (FD_ISSET(udp_sock, &rfds)) {
            handle_udp_response();
        }

        /* Check TCP sockets for incoming connections */
        reg = reg_list;
        while (reg) {
            if (reg->tcp_socket >= 0 && FD_ISSET(reg->tcp_socket, &rfds)) {
                int new_sd;
                pid_t pid;

                new_sd = accept(reg->tcp_socket, NULL, NULL);
                if (new_sd >= 0) {
                    pid = fork();
                    if (pid == 0) {
                        /* Child */
                        close(reg->tcp_socket);
                        handle_tcp_connection(new_sd, reg->content_name);
                        close(new_sd);
                        exit(0);
                    }
                    if (pid < 0) {
                        fprintf(stderr, "fork error\n");
                    }
                    close(new_sd);
                }
            }
            reg = reg->next;
        }

        /* Reap zombie processes */
        while (waitpid(-1, NULL, WNOHANG) > 0) {
            /* nothing */
        }
    }

    /* Cleanup */
    deregister_all();
    free_reg_list();
    close(udp_sock);
    return 0;
}

/* Handle user input commands */
void handle_user_input(char *input)
{
    char cmd[32];
    char arg1[64];
    char arg2[64];
    int n;

    cmd[0] = '\0';
    arg1[0] = '\0';
    arg2[0] = '\0';

    n = sscanf(input, "%31s %63s %63s", cmd, arg1, arg2);

    if (strcmp(cmd, "register") == 0) {
        if (n < 3) {
            printf("Usage: register <content_name> <filename>\n");
            return;
        }
        register_content(arg1, arg2);
    } else if (strcmp(cmd, "download") == 0) {
        if (n < 2) {
            printf("Usage: download <content_name>\n");
            return;
        }
        search_and_download(arg1);
    } else if (strcmp(cmd, "list") == 0) {
        list_contents();
    } else if (strcmp(cmd, "deregister") == 0) {
        if (n < 2) {
            printf("Usage: deregister <content_name>\n");
            return;
        }
        deregister_content(arg1);
    } else if (strcmp(cmd, "quit") == 0) {
        printf("Quitting...\n");
        deregister_all();
        free_reg_list();
        close(udp_sock);
        exit(0);
    } else {
        printf("Unknown command: %s\n", cmd);
    }
}

/* Register content with index server */
void register_content(const char *content_name, const char *filename)
{
    struct pdu out;
    struct pdu in;
    struct sockaddr_in tcp_addr;
    struct registered_content *existing;
    struct registered_content *new_reg;
    int tcp_sock;
    int fd;
    struct sockaddr_in local_addr;
    socklen_t alen;
    ssize_t n;

    /* Check if content name is valid */
    if (strlen(content_name) > CONTENT_NAME_SIZE) {
        printf("Error: Content name too long (max %d characters)\n", CONTENT_NAME_SIZE);
        return;
    }

    /* Check if already registered */
    existing = find_registered_content(content_name);
    if (existing) {
        printf("Error: Content '%s' already registered\n", content_name);
        return;
    }

    /* Check if file exists */
    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        printf("Error: Cannot open file '%s'\n", filename);
        return;
    }
    close(fd);

    /* Create TCP socket for this content */
    tcp_sock = create_tcp_socket_for_content(content_name, &tcp_addr);
    if (tcp_sock < 0) {
        printf("Error: Failed to create TCP socket\n");
        return;
    }

    /* Get local IP address for registration from UDP socket */
    alen = sizeof(local_addr);
    if (getsockname(udp_sock, (struct sockaddr *)&local_addr, &alen) < 0) {
        printf("Error: Failed to get local IP address\n");
        close(tcp_sock);
        return;
    }

    if (local_addr.sin_addr.s_addr == INADDR_ANY ||
        local_addr.sin_addr.s_addr == 0) {
        printf("Error: Could not determine local IP address\n");
        close(tcp_sock);
        return;
    }

    /* Prepare registration PDU:
     * Peer Name (10 bytes) | Content Name (10 bytes) | IP (4 bytes) | Port (2 bytes)
     */
    out.type = 'R';
    memset(out.data, 0, MAX_DATA_SIZE);
    strncpy(out.data, my_peer_name, PEER_NAME_SIZE);
    strncpy(out.data + PEER_NAME_SIZE, content_name, CONTENT_NAME_SIZE);
    memcpy(out.data + PEER_NAME_SIZE + CONTENT_NAME_SIZE,
           &local_addr.sin_addr.s_addr, 4);
    memcpy(out.data + PEER_NAME_SIZE + CONTENT_NAME_SIZE + 4,
           &tcp_addr.sin_port, 2);

    n = write(udp_sock, &out, 1 + PEER_NAME_SIZE + CONTENT_NAME_SIZE + 6);
    if (n < 0) {
        printf("Error: Failed to send registration\n");
        close(tcp_sock);
        return;
    }

    /* Wait for response */
    n = read(udp_sock, &in, sizeof(in));
    if (n < 0) {
        printf("Error: Failed to receive response\n");
        close(tcp_sock);
        return;
    }

    if (in.type == 'A') {
        new_reg = (struct registered_content *)malloc(sizeof(struct registered_content));
        if (new_reg) {
            memset(new_reg, 0, sizeof(*new_reg));
            strncpy(new_reg->peer_name, my_peer_name, PEER_NAME_SIZE);
            strncpy(new_reg->content_name, content_name, CONTENT_NAME_SIZE);
            strncpy(new_reg->filename, filename, sizeof(new_reg->filename) - 1);
            new_reg->tcp_socket = tcp_sock;
            memcpy(&new_reg->tcp_addr, &tcp_addr, sizeof(tcp_addr));
            new_reg->next = reg_list;
            reg_list = new_reg;

            printf("Content '%s' registered successfully (TCP port: %d)\n",
                   content_name, ntohs(tcp_addr.sin_port));
        } else {
            close(tcp_sock);
            printf("Error: Memory allocation failed\n");
        }
    } else if (in.type == 'E') {
        in.data[MAX_DATA_SIZE - 1] = '\0';
        printf("Registration failed: %s\n", in.data);
        close(tcp_sock);
    }
}

/* Create TCP socket for content with dynamic port assignment */
int create_tcp_socket_for_content(const char *content_name, struct sockaddr_in *addr)
{
    int sock;
    socklen_t alen;

    (void)content_name; /* unused for now */

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = INADDR_ANY;
    addr->sin_port = htons(0); /* Let OS assign port */

    if (bind(sock, (struct sockaddr *)addr, sizeof(*addr)) < 0) {
        close(sock);
        return -1;
    }

    alen = sizeof(*addr);
    if (getsockname(sock, (struct sockaddr *)addr, &alen) < 0) {
        close(sock);
        return -1;
    }

    if (listen(sock, 5) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

/* Search for content and download */
void search_and_download(const char *content_name)
{
    struct pdu out;
    struct pdu in;
    struct sockaddr_in server_addr;
    int tcp_sock;
    char filename[256];
    int fd;
    char buffer[BUFLEN];
    ssize_t n;
    ssize_t total;
    ssize_t data_size;
    ssize_t written;

    /* Send search request */
    out.type = 'S';
    memset(out.data, 0, MAX_DATA_SIZE);
    strncpy(out.data, content_name, CONTENT_NAME_SIZE);

    n = write(udp_sock, &out, 1 + CONTENT_NAME_SIZE);
    if (n < 0) {
        printf("Error: Failed to send search request\n");
        return;
    }

    /* Wait for response */
    n = read(udp_sock, &in, sizeof(in));
    if (n < 0) {
        printf("Error: Failed to receive search response\n");
        return;
    }

    if (in.type == 'E') {
        in.data[MAX_DATA_SIZE - 1] = '\0';
        printf("Search failed: %s\n", in.data);
        return;
    } else if (in.type != 'S' || n < 1 + 6) {
        printf("Error: Invalid search response\n");
        return;
    }

    /* Extract server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, in.data, 4);
    memcpy(&server_addr.sin_port, in.data + 4, 2);

    printf("Found content server: %s:%d\n",
           inet_ntoa(server_addr.sin_addr), ntohs(server_addr.sin_port));

    /* Connect to content server via TCP */
    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock < 0) {
        printf("Error: Failed to create TCP socket\n");
        return;
    }

    if (connect(tcp_sock, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        printf("Error: Failed to connect to content server\n");
        close(tcp_sock);
        return;
    }

    /* Send download request */
    out.type = 'D';
    memset(out.data, 0, MAX_DATA_SIZE);
    strncpy(out.data, content_name, CONTENT_NAME_SIZE);

    n = write(tcp_sock, &out, 1 + CONTENT_NAME_SIZE);
    if (n < 0) {
        printf("Error: Failed to send download request\n");
        close(tcp_sock);
        return;
    }

    /* Create output filename */
    snprintf(filename, sizeof(filename), "downloaded_%s", content_name);

    fd = open(filename, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        printf("Error: Failed to create output file\n");
        close(tcp_sock);
        return;
    }

    /* Receive content data */
    total = 0;
    for (;;) {
        n = read(tcp_sock, &in, sizeof(in));
        if (n <= 0) {
            break;
        }

        if (in.type == 'E') {
            in.data[MAX_DATA_SIZE - 1] = '\0';
            printf("Download error: %s\n", in.data);
            close(fd);
            unlink(filename);
            close(tcp_sock);
            return;
        } else if (in.type == 'C' || in.type == 'F') {
            data_size = n - 1;
            if (data_size > 0) {
                written = write(fd, in.data, data_size);
                if (written != data_size) {
                    printf("Warning: Partial write\n");
                }
                total += written;
            }
            if (in.type == 'F') {
                break;
            }
        }
    }

    close(fd);
    close(tcp_sock);
    printf("Downloaded %zd bytes to '%s'\n", total, filename);

    /* Auto-register as content server */
    register_content(content_name, filename);
}

/* List all registered contents */
void list_contents(void)
{
    struct pdu out;
    struct pdu in;
    ssize_t n;

    out.type = 'O';
    memset(out.data, 0, MAX_DATA_SIZE);

    n = write(udp_sock, &out, 1);
    if (n < 0) {
        printf("Error: Failed to send list request\n");
        return;
    }

    n = read(udp_sock, &in, sizeof(in));
    if (n < 0) {
        printf("Error: Failed to receive list response\n");
        return;
    }

    if (in.type == 'O') {
        in.data[MAX_DATA_SIZE - 1] = '\0';
        printf("Registered contents:\n%s\n", in.data);
    } else if (in.type == 'E') {
        in.data[MAX_DATA_SIZE - 1] = '\0';
        printf("Error: %s\n", in.data);
    }
}

/* Deregister content */
void deregister_content(const char *content_name)
{
    struct pdu out;
    struct pdu in;
    struct registered_content *reg;
    struct registered_content *prev;
    ssize_t n;

    reg = find_registered_content(content_name);
    if (!reg) {
        printf("Error: Content '%s' not registered\n", content_name);
        return;
    }

    out.type = 'T';
    memset(out.data, 0, MAX_DATA_SIZE);
    strncpy(out.data, my_peer_name, PEER_NAME_SIZE);
    strncpy(out.data + PEER_NAME_SIZE, content_name, CONTENT_NAME_SIZE);

    n = write(udp_sock, &out, 1 + PEER_NAME_SIZE + CONTENT_NAME_SIZE);
    if (n < 0) {
        printf("Error: Failed to send deregistration request\n");
        return;
    }

    n = read(udp_sock, &in, sizeof(in));
    if (n < 0) {
        printf("Error: Failed to receive response\n");
        return;
    }

    if (in.type == 'A') {
        if (reg == reg_list) {
            reg_list = reg->next;
        } else {
            prev = reg_list;
            while (prev && prev->next != reg) {
                prev = prev->next;
            }
            if (prev) {
                prev->next = reg->next;
            }
        }
        if (reg->tcp_socket >= 0) {
            close(reg->tcp_socket);
        }
        free(reg);
        printf("Content '%s' deregistered successfully\n", content_name);
    } else if (in.type == 'E') {
        in.data[MAX_DATA_SIZE - 1] = '\0';
        printf("Deregistration failed: %s\n", in.data);
    }
}

/* Deregister all content */
void deregister_all(void)
{
    struct registered_content *reg;
    struct registered_content *next;

    reg = reg_list;
    while (reg) {
        next = reg->next;
        deregister_content(reg->content_name);
        reg = next;
    }
}

/* Handle TCP connection for content download */
void handle_tcp_connection(int tcp_sock, const char *content_name)
{
    struct pdu in;
    struct pdu out;
    struct registered_content *reg;
    int fd;
    ssize_t n;
    ssize_t r;
    char buffer[BUFLEN];
    char filename[256];

    /* Receive download request */
    n = read(tcp_sock, &in, sizeof(in));
    if (n < 0 || in.type != 'D') {
        out.type = 'E';
        strncpy(out.data, "Invalid download request", MAX_DATA_SIZE - 1);
        out.data[MAX_DATA_SIZE - 1] = '\0';
        write(tcp_sock, &out, 1 + strlen(out.data) + 1);
        return;
    }

    /* Use the content_name parameter to find the registered content */
    reg = find_registered_content(content_name);
    if (!reg) {
        out.type = 'E';
        strncpy(out.data, "Content not found", MAX_DATA_SIZE - 1);
        out.data[MAX_DATA_SIZE - 1] = '\0';
        write(tcp_sock, &out, 1 + strlen(out.data) + 1);
        return;
    }

    /* Open the file using the stored filename */
    fd = open(reg->filename, O_RDONLY);
    if (fd < 0) {
        snprintf(filename, sizeof(filename), "downloaded_%s", reg->content_name);
        fd = open(filename, O_RDONLY);
        if (fd < 0) {
            out.type = 'E';
            snprintf(out.data, MAX_DATA_SIZE - 1,
                     "Cannot open file '%s' for content '%s'",
                     reg->filename, reg->content_name);
            out.data[MAX_DATA_SIZE - 1] = '\0';
            write(tcp_sock, &out, 1 + strlen(out.data) + 1);
            return;
        }
    }

    /* Send file data */
    for (;;) {
        r = read(fd, buffer, MAX_DATA_SIZE);
        if (r < 0) {
            out.type = 'E';
            strncpy(out.data, "Read error", MAX_DATA_SIZE - 1);
            out.data[MAX_DATA_SIZE - 1] = '\0';
            write(tcp_sock, &out, 1 + strlen(out.data) + 1);
            break;
        }
        if (r == 0) {
            out.type = 'F';
            write(tcp_sock, &out, 1);
            break;
        }
        if (r < MAX_DATA_SIZE) {
            out.type = 'F';
            memcpy(out.data, buffer, r);
            write(tcp_sock, &out, 1 + r);
            break;
        } else {
            out.type = 'C';
            memcpy(out.data, buffer, r);
            write(tcp_sock, &out, 1 + r);
        }
    }

    close(fd);
}

/* Handle UDP response from index server
 * Currently responses are handled synchronously in the
 * functions that send requests. This function is kept
 * for future async handling if needed.
 */
void handle_udp_response(void)
{
    char buf[1];

    /* Drain any unexpected data to avoid blocking */
    if (read(udp_sock, buf, sizeof(buf)) < 0) {
        /* ignore */
    }
}

/* Find registered content by name */
struct registered_content *find_registered_content(const char *content_name)
{
    struct registered_content *current;

    current = reg_list;
    while (current) {
        if (strncmp(current->content_name, content_name, CONTENT_NAME_SIZE) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

/* Free registered content list */
void free_reg_list(void)
{
    struct registered_content *current;
    struct registered_content *next;

    current = reg_list;
    while (current) {
        next = current->next;
        if (current->tcp_socket >= 0) {
            close(current->tcp_socket);
        }
        free(current);
        current = next;
    }
    reg_list = NULL;
}
