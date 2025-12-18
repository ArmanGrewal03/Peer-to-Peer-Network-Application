/* PDU Types:
 * R - Content Registration (Peer -> Index Server)
 * D - Content Download Request (Client -> Content Server)
 * S - Search for content and server (Peer <-> Index Server)
 * T - Content De-Registration (Peer -> Index Server)
 * C - Content Data (Content Server -> Content Client)
 * O - List of Online Registered Content (Peer <-> Index Server)
 * A - Acknowledgement (Index Server -> Peer)
 * E - Error (Between Peers or Peer <-> Index Server)
 */


#ifndef PDU_H
#define PDU_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_DATA_SIZE 100
#define PEER_NAME_SIZE 10
#define CONTENT_NAME_SIZE 10

/* PDU structure */
struct pdu {
    char type;              
    char data[MAX_DATA_SIZE];
};

/* Content registration entry structure */
struct content_entry {
    char peer_name[PEER_NAME_SIZE + 1];
    char content_name[CONTENT_NAME_SIZE + 1];
    struct sockaddr_in addr;
    int usage_count;        
    struct content_entry *next;
};


#endif 

