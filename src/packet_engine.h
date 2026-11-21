#ifndef PACKET_ENGINE_H
#define PACKET_ENGINE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

typedef enum {
    DIR_PHONE_TO_INTERNET,
    DIR_INTERNET_TO_PHONE
} direction_t;

typedef enum {
    PROTO_TCP,
    PROTO_UDP,
    PROTO_ICMP,
    PROTO_DNS,
    PROTO_OTHER
} proto_t;

typedef struct {
    char src_ip[46];
    char dst_ip[46];
    uint16_t src_port;
    uint16_t dst_port;
    proto_t protocol;
    size_t payload_len;
    char dns_query[256];  // populated if DNS
    direction_t direction;
} packet_info_t;

// Thread-safe traffic statistics
typedef struct {
    pthread_mutex_t lock;
    uint64_t total_packets;
    uint64_t total_bytes_in;      // internet -> phone
    uint64_t total_bytes_out;     // phone -> internet
    uint64_t tcp_packets;
    uint64_t udp_packets;
    uint64_t dns_queries;
    uint64_t icmp_packets;
    uint32_t active_connections;
} traffic_stats_t;

// Callback for packet processing hook
typedef int (*packet_handler_fn)(const packet_info_t *info, const uint8_t *raw_pkt, size_t raw_len);

// Initialize packet engine on a given interface
// ap_iface: the AP interface name (e.g., "ap0")
// phone_subnet: the subnet prefix for phone clients (e.g., "192.168.12")
// handler: optional callback for each captured packet (can be NULL)
// Returns 0 on success, -1 on error
int packet_engine_init(const char *ap_iface, const char *phone_subnet, packet_handler_fn handler);

// Start the capture loop (blocks, run in a thread)
// Returns 0 on clean exit, -1 on error
int packet_engine_run(void);

// Stop the capture loop
void packet_engine_stop(void);

// Get current traffic statistics (thread-safe copy)
void packet_engine_get_stats(traffic_stats_t *out);

// Clean up resources
void packet_engine_cleanup(void);

// Parse a raw ethernet frame into packet_info_t
// Returns 0 on success, -1 if not a parseable IP packet
int parse_packet(const uint8_t *frame, size_t frame_len, const char *phone_subnet, packet_info_t *info);

// Format a packet_info_t into a human-readable log line
// buf: output buffer, buf_size: size of buffer
// Returns number of chars written
int format_packet_log(const packet_info_t *info, char *buf, size_t buf_size);

#endif
