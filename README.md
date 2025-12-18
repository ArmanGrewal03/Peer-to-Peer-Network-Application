# Peer-to-Peer Content Indexing & Transfer (C, Sockets)

A small C networking project that uses a custom **PDU (Protocol Data Unit)** format to support:
- registering content with an **Index Server**
- searching/listing registered content
- requesting content downloads from a **Content Server/Peer**
- deregistering content
- transferring content data using PDUs

This project is built around simple message types exchanged between peers and the index server.

## Protocol Overview

PDUs are defined as:

- `type` (1 byte char)
- `data` (fixed-size payload)

### PDU Types

| Type | Meaning | Direction |
|------|---------|-----------|
| `R` | Content Registration | Peer → Index Server |
| `S` | Search for content & server | Peer ↔ Index Server |
| `O` | List Online Registered Content | Peer ↔ Index Server |
| `T` | Content De-Registration | Peer → Index Server |
| `D` | Content Download Request | Client → Content Server |
| `C` | Content Data | Content Server → Content Client |
| `A` | Acknowledgement | Index Server → Peer |
| `E` | Error | Peer ↔ Peer or Peer ↔ Index Server |

## Data Structures

The project maintains a linked list of registered content entries (example fields):
- peer name
- content name
- peer address (`sockaddr_in`)
- usage count (for tracking downloads or popularity)
