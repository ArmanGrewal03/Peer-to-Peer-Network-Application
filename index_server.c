
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "pdu.h"

#define BUFLEN 256


struct content_entry *content_list = NULL;

void add_content(const char *peer_name, const char *content_name, struct sockaddr_in *addr);
struct content_entry *find_content(const char *content_name);
struct content_entry *find_least_used_content(const char *content_name);
int remove_content(const char *peer_name, const char *content_name);
void free_content_list(void);
void list_all_contents(char *buffer, int max_size);

int main(int argc, char *argv[])
{
    struct sockaddr_in sin, fsin;
    socklen_t alen;
    int s;
    int port = 3000;
    struct pdu in, out;

    // Parse command line arguments 
    switch (argc) {
    case 1:
        break;
    case 2:
        port = atoi(argv[1]);
        break;
    default:
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        exit(1);
    }

    // Initialize server address
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port);

    /* Create UDP socket */
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        fprintf(stderr, "can't create socket\n");
        exit(1);
    }

    /* Bind socket */
    if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        fprintf(stderr, "can't bind to port %d\n", port);
        close(s);
        exit(1);
    }

    printf("Index Server started on port %d\n", port);
    alen = sizeof(fsin);

    //Main Loop
    for (;;) {
        memset(&in, 0, sizeof(in));
        memset(&out, 0, sizeof(out));

        // Wait for incoming PDU and read it
        ssize_t n = recvfrom(s, &in, sizeof(in), 0, (struct sockaddr *)&fsin, &alen);
        if (n < 0) {
            fprintf(stderr, "recvfrom error\n");
            continue;
        }

        // Process based on PDU type
        switch (in.type) {
        case 'R': { // R for Registration
            // Format: Peer Name (10 bytes) | Content Name (10 bytes) | IP (4 bytes) | Port (2 bytes) 
            if (n < 1 + PEER_NAME_SIZE + CONTENT_NAME_SIZE + 6) {
                out.type = 'E';
                strncpy(out.data, "Invalid registration format", MAX_DATA_SIZE - 1);
                sendto(s, &out, 1 + strlen(out.data) + 1, 0, (struct sockaddr *)&fsin, alen);
                break;
            }

            char peer_name[PEER_NAME_SIZE + 1] = {0};
            char content_name[CONTENT_NAME_SIZE + 1] = {0};
            struct sockaddr_in reg_addr;

            memcpy(peer_name, in.data, PEER_NAME_SIZE);
            memcpy(content_name, in.data + PEER_NAME_SIZE, CONTENT_NAME_SIZE);
            
            // Extract IP and Port from data 
            memcpy(&reg_addr.sin_addr.s_addr, in.data + PEER_NAME_SIZE + CONTENT_NAME_SIZE, 4);
            memcpy(&reg_addr.sin_port, in.data + PEER_NAME_SIZE + CONTENT_NAME_SIZE + 4, 2);
            reg_addr.sin_family = AF_INET;

            // Check if already registered 
            struct content_entry *existing = content_list;
            int duplicate = 0;
            while (existing) { // Loops through linked list to make sure not duplicate
                if (strncmp(existing->peer_name, peer_name, PEER_NAME_SIZE) == 0 &&
                    strncmp(existing->content_name, content_name, CONTENT_NAME_SIZE) == 0) {
                    duplicate = 1;
                    break;
                }
                existing = existing->next;
            }

            if (duplicate) {
                out.type = 'E';
                strncpy(out.data, "Peer name and content already registered", MAX_DATA_SIZE - 1);
                sendto(s, &out, 1 + strlen(out.data) + 1, 0, (struct sockaddr *)&fsin, alen);
            } else {
                add_content(peer_name, content_name, &reg_addr);
                out.type = 'A';
                strncpy(out.data, "Registration successful", MAX_DATA_SIZE - 1);
                sendto(s, &out, 1 + strlen(out.data) + 1, 0, (struct sockaddr *)&fsin, alen);
                printf("Registered: Peer='%s' Content='%s' Address=%s:%d\n",
                       peer_name, content_name,
                       inet_ntoa(reg_addr.sin_addr), ntohs(reg_addr.sin_port));
            }
            break;
        }

        case 'S': { // S for Search for content and server
            // Format: Content Name (10 bytes) 
            if (n < 1 + CONTENT_NAME_SIZE) {
                out.type = 'E';
                strncpy(out.data, "Invalid search format", MAX_DATA_SIZE - 1);
                sendto(s, &out, 1 + strlen(out.data) + 1, 0, (struct sockaddr *)&fsin, alen);
                break;
            }

            char content_name[CONTENT_NAME_SIZE + 1] = {0};
            memcpy(content_name, in.data, CONTENT_NAME_SIZE);

            struct content_entry *entry = find_least_used_content(content_name);
            if (entry == NULL) {
                out.type = 'E';
                strncpy(out.data, "Content not found", MAX_DATA_SIZE - 1);
                sendto(s, &out, 1 + strlen(out.data) + 1, 0, (struct sockaddr *)&fsin, alen);
            } else {
                // Increment usage count 
                entry->usage_count++;
                
                //Format response: IP (4 bytes) | Port (2 bytes) 
                out.type = 'S';
                memcpy(out.data, &entry->addr.sin_addr.s_addr, 4);
                memcpy(out.data + 4, &entry->addr.sin_port, 2);
                sendto(s, &out, 1 + 6, 0, (struct sockaddr *)&fsin, alen);
                printf("Search: Content='%s' -> Peer='%s' Address=%s:%d\n",
                       content_name, entry->peer_name,
                       inet_ntoa(entry->addr.sin_addr), ntohs(entry->addr.sin_port));
            }
            break;
        }

        case 'T': { // De-registration 
            // Format: Peer Name (10 bytes) | Content Name (10 bytes) 
            if (n < 1 + PEER_NAME_SIZE + CONTENT_NAME_SIZE) {
                out.type = 'E';
                strncpy(out.data, "Invalid deregistration format", MAX_DATA_SIZE - 1);
                sendto(s, &out, 1 + strlen(out.data) + 1, 0, (struct sockaddr *)&fsin, alen);
                break;
            }

            char peer_name[PEER_NAME_SIZE + 1] = {0};
            char content_name[CONTENT_NAME_SIZE + 1] = {0};
            memcpy(peer_name, in.data, PEER_NAME_SIZE);
            memcpy(content_name, in.data + PEER_NAME_SIZE, CONTENT_NAME_SIZE);

            if (remove_content(peer_name, content_name)) {
                out.type = 'A';
                strncpy(out.data, "Deregistration successful", MAX_DATA_SIZE - 1);
                sendto(s, &out, 1 + strlen(out.data) + 1, 0, (struct sockaddr *)&fsin, alen);
                printf("Deregistered: Peer='%s' Content='%s'\n", peer_name, content_name);
            } else {
                out.type = 'E';
                strncpy(out.data, "Content not found for deregistration", MAX_DATA_SIZE - 1);
                sendto(s, &out, 1 + strlen(out.data) + 1, 0, (struct sockaddr *)&fsin, alen);
            }
            break;
        }

        case 'O': { /* List all content */
            char list_buffer[BUFLEN] = {0};
            list_all_contents(list_buffer, BUFLEN);
            
            out.type = 'O';
            strncpy(out.data, list_buffer, MAX_DATA_SIZE - 1);
            sendto(s, &out, 1 + strlen(out.data) + 1, 0, (struct sockaddr *)&fsin, alen);
            printf("List request from %s:%d\n", inet_ntoa(fsin.sin_addr), ntohs(fsin.sin_port));
            break;
        }

        default:
            out.type = 'E';
            strncpy(out.data, "Unknown PDU type", MAX_DATA_SIZE - 1);
            sendto(s, &out, 1 + strlen(out.data) + 1, 0, (struct sockaddr *)&fsin, alen);
            break;
        }
    }

    free_content_list();
    close(s);
    return 0;
}

// Add content to the linked list list 
void add_content(const char *peer_name, const char *content_name, struct sockaddr_in *addr)
{
    struct content_entry *new_entry = (struct content_entry *)malloc(sizeof(struct content_entry));
    if (new_entry == NULL) {
        fprintf(stderr, "Memory allocation error\n");
        return;
    }

    strncpy(new_entry->peer_name, peer_name, PEER_NAME_SIZE);
    new_entry->peer_name[PEER_NAME_SIZE] = '\0';
    strncpy(new_entry->content_name, content_name, CONTENT_NAME_SIZE);
    new_entry->content_name[CONTENT_NAME_SIZE] = '\0';
    memcpy(&new_entry->addr, addr, sizeof(struct sockaddr_in));
    new_entry->usage_count = 0;
    new_entry->next = content_list;
    content_list = new_entry;
}

// Find content entry by name 
struct content_entry *find_content(const char *content_name)
{
    struct content_entry *current = content_list;
    while (current) {
        if (strncmp(current->content_name, content_name, CONTENT_NAME_SIZE) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// Find least used content server for load balancing 
struct content_entry *find_least_used_content(const char *content_name)
{
    struct content_entry *current = content_list;
    struct content_entry *best = NULL;
    int min_usage = -1;

    while (current) {
        if (strncmp(current->content_name, content_name, CONTENT_NAME_SIZE) == 0) {
            if (min_usage == -1 || current->usage_count < min_usage) {
                min_usage = current->usage_count;
                best = current;
            }
        }
        current = current->next;
    }
    return best;
}

//Remove content from linked list 
int remove_content(const char *peer_name, const char *content_name)
{
    struct content_entry *current = content_list;
    struct content_entry *prev = NULL;

    while (current) {
        if (strncmp(current->peer_name, peer_name, PEER_NAME_SIZE) == 0 &&
            strncmp(current->content_name, content_name, CONTENT_NAME_SIZE) == 0) {
            if (prev) {
                prev->next = current->next;
            } else {
                content_list = current->next;
            }
            free(current);
            return 1;
        }
        prev = current;
        current = current->next;
    }
    return 0;
}

// Free all content entries from linked list
void free_content_list(void)
{
    struct content_entry *current = content_list;
    while (current) {
        struct content_entry *next = current->next;
        free(current);
        current = next;
    }
    content_list = NULL;
}

// List all registered contents 
void list_all_contents(char *buffer, int max_size)
// FORMAT: peer1|fileA|192.168.1.10:5000;peer2|fileB|192.168.1.11:6000;
{
    struct content_entry *current = content_list;
    int pos = 0;

    if (current == NULL) {
        strncpy(buffer, "No content registered", max_size - 1);
        return;
    }

    while (current && pos < max_size - 50) {
        int n = snprintf(buffer + pos, max_size - pos, "%s|%s|%s:%d;",
                         current->peer_name, current->content_name,
                         inet_ntoa(current->addr.sin_addr), ntohs(current->addr.sin_port));
        if (n > 0) {
            pos += n;
        }
        current = current->next;
    }
    buffer[max_size - 1] = '\0';
}

