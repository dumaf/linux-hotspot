#include "packet_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/ip_icmp.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if_packet.h>
#include <pthread.h>

#define MAX_CONNECTIONS 1024
#define CONN_TIMEOUT_SEC 60

typedef struct {
    char ip1[46];
    char ip2[46];
    uint16_t port1;
    uint16_t port2;
    time_t last_seen;
    uint8_t occupied;
} conn_slot_t;

/* Global state of the packet capture engine */
static int g_sock = -1;
static volatile sig_atomic_t g_running = 0;
static packet_handler_fn g_handler = NULL;
static char g_ap_iface[IFNAMSIZ] = {0};
static char g_phone_subnet[64] = {0};
static traffic_stats_t g_stats;
static int g_initialized = 0;

static conn_slot_t g_connections[MAX_CONNECTIONS];
static uint32_t g_active_conn_count = 0;

/* Returns standard well-known port names */
static const char *get_well_known_port_name(uint16_t port) {
    switch (port) {
        case 20:   return "FTP-DATA";
        case 21:   return "FTP";
        case 22:   return "SSH";
        case 23:   return "Telnet";
        case 25:   return "SMTP";
        case 53:   return "DNS";
        case 67:   return "DHCP";
        case 68:   return "DHCP";
        case 69:   return "TFTP";
        case 80:   return "HTTP";
        case 110:  return "POP3";
        case 123:  return "NTP";
        case 143:  return "IMAP";
        case 161:  return "SNMP";
        case 194:  return "IRC";
        case 443:  return "HTTPS";
        case 465:  return "SMTPS";
        case 514:  return "Syslog";
        case 587:  return "Submission";
        case 636:  return "LDAPS";
        case 853:  return "DoT";
        case 993:  return "IMAPS";
        case 995:  return "POP3S";
        case 1194: return "OpenVPN";
        case 1883: return "MQTT";
        case 3306: return "MySQL";
        case 3389: return "RDP";
        case 5222: return "XMPP";
        case 5228: return "GooglePlay";
        case 5353: return "mDNS";
        case 5432: return "PostgreSQL";
        case 8080: return "HTTP-Proxy";
        case 8443: return "HTTPS-Alt";
        default:   return NULL;
    }
}

/* Parse DNS query name from DNS message payload, handling compression pointers */
static void parse_dns_query(const uint8_t *dns_payload, size_t dns_len, char *out_name, size_t max_out) {
    if (!dns_payload || dns_len < 12 || !out_name || max_out == 0) {
        if (out_name && max_out > 0) {
            out_name[0] = '\0';
        }
        return;
    }

    out_name[0] = '\0';

    /* DNS header: QDCOUNT is at byte offset 4-5 */
    uint16_t qdcount = (uint16_t)(((uint16_t)dns_payload[4] << 8) | dns_payload[5]);
    if (qdcount == 0) {
        return;
    }

    size_t curr = 12; /* Start of question section */
    size_t out_len = 0;
    int jumps = 0;
    const int max_jumps = 16;

    while (curr < dns_len) {
        uint8_t label_len = dns_payload[curr];
        if (label_len == 0) {
            /* Zero byte marks end of domain name */
            break;
        }

        /* Check for compression pointer (top two bits 11 -> 0xC0) */
        if ((label_len & 0xC0) == 0xC0) {
            if (curr + 1 >= dns_len) {
                break;
            }
            if (++jumps > max_jumps) {
                break; /* Protect against circular pointer loops */
            }
            uint16_t ptr_offset = (uint16_t)(((uint16_t)(label_len & 0x3F) << 8) | dns_payload[curr + 1]);
            if (ptr_offset >= dns_len) {
                break; /* Pointer points outside message boundary */
            }
            curr = ptr_offset;
            continue;
        }

        /* Standard label: reserved bits must not be set */
        if ((label_len & 0xC0) != 0) {
            break;
        }

        curr++; /* Move past length byte */
        if (curr + label_len > dns_len) {
            break; /* Label extends beyond payload */
        }

        /* Append '.' delimiter if not first label */
        if (out_len > 0) {
            if (out_len + 1 >= max_out) {
                break;
            }
            out_name[out_len++] = '.';
        }

        /* Copy label characters */
        for (uint8_t i = 0; i < label_len; i++) {
            if (out_len + 1 >= max_out) {
                break;
            }
            char c = (char)dns_payload[curr + i];
            if (c >= 32 && c <= 126) {
                out_name[out_len++] = c;
            } else {
                out_name[out_len++] = '?';
            }
        }
        curr += label_len;
    }

    out_name[out_len] = '\0';
}

/* Safe copy helper that guarantees null-termination without compiler warnings */
static void safe_copy_str(char *dst, const char *src, size_t dst_size) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < dst_size && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Order connection endpoints for bidirectional equivalence */
static void normalize_endpoints(const char *s_ip, uint16_t s_port,
                                const char *d_ip, uint16_t d_port,
                                char *out_ip1, uint16_t *out_port1,
                                char *out_ip2, uint16_t *out_port2) {
    int cmp = strcmp(s_ip, d_ip);
    if (cmp < 0 || (cmp == 0 && s_port <= d_port)) {
        safe_copy_str(out_ip1, s_ip, 46);
        *out_port1 = s_port;
        safe_copy_str(out_ip2, d_ip, 46);
        *out_port2 = d_port;
    } else {
        safe_copy_str(out_ip1, d_ip, 46);
        *out_port1 = d_port;
        safe_copy_str(out_ip2, s_ip, 46);
        *out_port2 = s_port;
    }
}

/* Prune inactive connection entries older than CONN_TIMEOUT_SEC */
static void prune_connections_locked(time_t now) {
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (g_connections[i].occupied) {
            if (now - g_connections[i].last_seen > CONN_TIMEOUT_SEC) {
                g_connections[i].occupied = 0;
                if (g_active_conn_count > 0) {
                    g_active_conn_count--;
                }
            }
        }
    }
}

/* Touch an existing connection or add a new one */
static void conn_touch_or_add_locked(const char *s_ip, uint16_t s_port,
                                     const char *d_ip, uint16_t d_port,
                                     time_t now) {
    char ip1[46], ip2[46];
    uint16_t port1 = 0, port2 = 0;
    normalize_endpoints(s_ip, s_port, d_ip, d_port, ip1, &port1, ip2, &port2);

    int free_slot = -1;
    int oldest_slot = -1;
    time_t oldest_time = now;

    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (g_connections[i].occupied) {
            if (g_connections[i].port1 == port1 &&
                g_connections[i].port2 == port2 &&
                strcmp(g_connections[i].ip1, ip1) == 0 &&
                strcmp(g_connections[i].ip2, ip2) == 0) {
                g_connections[i].last_seen = now;
                return;
            }
            if (g_connections[i].last_seen < oldest_time) {
                oldest_time = g_connections[i].last_seen;
                oldest_slot = (int)i;
            }
        } else if (free_slot < 0) {
            free_slot = (int)i;
        }
    }

    int target_slot = free_slot;
    if (target_slot < 0) {
        target_slot = oldest_slot;
    } else {
        g_active_conn_count++;
    }

    if (target_slot >= 0) {
        g_connections[target_slot].occupied = 1;
        safe_copy_str(g_connections[target_slot].ip1, ip1, sizeof(g_connections[target_slot].ip1));
        safe_copy_str(g_connections[target_slot].ip2, ip2, sizeof(g_connections[target_slot].ip2));
        g_connections[target_slot].port1 = port1;
        g_connections[target_slot].port2 = port2;
        g_connections[target_slot].last_seen = now;
    }
}

/* Remove a connection when FIN or RST is observed */
static void conn_remove_locked(const char *s_ip, uint16_t s_port,
                               const char *d_ip, uint16_t d_port) {
    char ip1[46], ip2[46];
    uint16_t port1 = 0, port2 = 0;
    normalize_endpoints(s_ip, s_port, d_ip, d_port, ip1, &port1, ip2, &port2);

    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (g_connections[i].occupied &&
            g_connections[i].port1 == port1 &&
            g_connections[i].port2 == port2 &&
            strcmp(g_connections[i].ip1, ip1) == 0 &&
            strcmp(g_connections[i].ip2, ip2) == 0) {
            g_connections[i].occupied = 0;
            if (g_active_conn_count > 0) {
                g_active_conn_count--;
            }
            return;
        }
    }
}

/* Update traffic statistics thread-safely */
static void update_stats(const packet_info_t *info, size_t raw_len, const uint8_t *raw_pkt) {
    pthread_mutex_lock(&g_stats.lock);

    g_stats.total_packets++;
    if (info->direction == DIR_INTERNET_TO_PHONE) {
        g_stats.total_bytes_in += raw_len;
    } else {
        g_stats.total_bytes_out += raw_len;
    }

    switch (info->protocol) {
        case PROTO_TCP:
            g_stats.tcp_packets++;
            break;
        case PROTO_UDP:
            g_stats.udp_packets++;
            break;
        case PROTO_DNS:
            g_stats.dns_queries++;
            break;
        case PROTO_ICMP:
            g_stats.icmp_packets++;
            break;
        case PROTO_OTHER:
        default:
            break;
    }

    if (info->protocol == PROTO_TCP) {
        time_t now = time(NULL);
        prune_connections_locked(now);

        uint8_t flags = 0;
        if (raw_pkt && raw_len >= 34) {
            size_t eth_off = 14;
            uint16_t etype = (uint16_t)(((uint16_t)raw_pkt[12] << 8) | raw_pkt[13]);
            if (etype == 0x8100 && raw_len >= 18) {
                eth_off = 18;
            }
            if (raw_len >= eth_off + sizeof(struct iphdr)) {
                const struct iphdr *iph = (const struct iphdr *)(raw_pkt + eth_off);
                size_t ip_hl = (size_t)iph->ihl * 4;
                if (ip_hl >= sizeof(struct iphdr) && raw_len >= eth_off + ip_hl + sizeof(struct tcphdr)) {
                    /* In TCP header, flags byte is at byte 13 */
                    flags = raw_pkt[eth_off + ip_hl + 13];
                }
            }
        }

        /* Check FIN (0x01) or RST (0x04) */
        if (flags & (0x01 | 0x04)) {
            conn_remove_locked(info->src_ip, info->src_port, info->dst_ip, info->dst_port);
        } else {
            conn_touch_or_add_locked(info->src_ip, info->src_port, info->dst_ip, info->dst_port, now);
        }

        g_stats.active_connections = g_active_conn_count;
    }

    pthread_mutex_unlock(&g_stats.lock);
}

/* Parse a raw ethernet frame into packet_info_t */
int parse_packet(const uint8_t *frame, size_t frame_len, const char *phone_subnet, packet_info_t *info) {
    if (!frame || frame_len < 14 || !info) {
        return -1;
    }

    memset(info, 0, sizeof(*info));

    /* Ethernet header parsing */
    size_t eth_hdr_len = 14;
    uint16_t eth_type = (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);

    /* Handle 802.1Q VLAN Tagging */
    if (eth_type == 0x8100) {
        if (frame_len < 18) {
            return -1;
        }
        eth_hdr_len = 18;
        eth_type = (uint16_t)(((uint16_t)frame[16] << 8) | frame[17]);
    } else if (eth_type == 0x88A8) { /* 802.1ad QinQ */
        if (frame_len < eth_hdr_len + 4) {
            return -1;
        }
        eth_hdr_len += 4;
        eth_type = (uint16_t)(((uint16_t)frame[eth_hdr_len - 2] << 8) | frame[eth_hdr_len - 1]);
        if (eth_type == 0x8100) {
            if (frame_len < eth_hdr_len + 4) {
                return -1;
            }
            eth_hdr_len += 4;
            eth_type = (uint16_t)(((uint16_t)frame[eth_hdr_len - 2] << 8) | frame[eth_hdr_len - 1]);
        }
    }

    /* Only parse IPv4 frames */
    if (eth_type != 0x0800) {
        return -1;
    }

    /* IPv4 header validation */
    if (frame_len < eth_hdr_len + sizeof(struct iphdr)) {
        return -1;
    }

    const struct iphdr *iph = (const struct iphdr *)(frame + eth_hdr_len);
    if (iph->version != 4) {
        return -1;
    }

    size_t ip_hl = (size_t)iph->ihl * 4;
    if (ip_hl < sizeof(struct iphdr) || frame_len < eth_hdr_len + ip_hl) {
        return -1;
    }

    uint16_t ip_tot_len = ntohs(iph->tot_len);
    size_t captured_ip = frame_len - eth_hdr_len;
    size_t actual_ip_len = (ip_tot_len > 0 && ip_tot_len <= captured_ip) ? ip_tot_len : captured_ip;
    if (actual_ip_len < ip_hl) {
        return -1;
    }
    size_t ip_payload_len = actual_ip_len - ip_hl;

    /* Format IP strings */
    if (!inet_ntop(AF_INET, &(iph->saddr), info->src_ip, sizeof(info->src_ip))) {
        return -1;
    }
    if (!inet_ntop(AF_INET, &(iph->daddr), info->dst_ip, sizeof(info->dst_ip))) {
        return -1;
    }

    /* Direction detection */
    if (phone_subnet && phone_subnet[0] != '\0') {
        size_t sub_len = strlen(phone_subnet);
        if (strncmp(info->src_ip, phone_subnet, sub_len) == 0) {
            info->direction = DIR_PHONE_TO_INTERNET;
        } else if (strncmp(info->dst_ip, phone_subnet, sub_len) == 0) {
            info->direction = DIR_INTERNET_TO_PHONE;
        } else {
            /* Support CIDR subnet notation (e.g. 192.168.12.1/24) */
            const char *slash = strchr(phone_subnet, '/');
            if (slash) {
                const char *last_dot = strrchr(phone_subnet, '.');
                if (last_dot && last_dot < slash) {
                    size_t pfx_len = (size_t)(last_dot - phone_subnet);
                    if (strncmp(info->src_ip, phone_subnet, pfx_len) == 0) {
                        info->direction = DIR_PHONE_TO_INTERNET;
                    } else if (strncmp(info->dst_ip, phone_subnet, pfx_len) == 0) {
                        info->direction = DIR_INTERNET_TO_PHONE;
                    } else {
                        info->direction = DIR_PHONE_TO_INTERNET;
                    }
                } else {
                    info->direction = DIR_PHONE_TO_INTERNET;
                }
            } else {
                info->direction = DIR_PHONE_TO_INTERNET;
            }
        }
    } else {
        info->direction = DIR_PHONE_TO_INTERNET;
    }

    /* Check fragment offset */
    uint16_t frag_off = ntohs(iph->frag_off);
    if ((frag_off & 0x1FFF) != 0) {
        info->protocol = PROTO_OTHER;
        info->payload_len = ip_payload_len;
        return 0;
    }

    const uint8_t *trans = frame + eth_hdr_len + ip_hl;

    /* Dissect transport protocol */
    if (iph->protocol == IPPROTO_TCP) {
        info->protocol = PROTO_TCP;
        if (ip_payload_len >= sizeof(struct tcphdr)) {
            const struct tcphdr *tcph = (const struct tcphdr *)trans;
            info->src_port = ntohs(tcph->source);
            info->dst_port = ntohs(tcph->dest);
            size_t tcp_hl = (size_t)tcph->doff * 4;
            if (tcp_hl >= sizeof(struct tcphdr) && ip_payload_len >= tcp_hl) {
                info->payload_len = ip_payload_len - tcp_hl;
            } else {
                info->payload_len = 0;
            }
        } else {
            info->payload_len = 0;
        }
        return 0;
    }

    if (iph->protocol == IPPROTO_UDP) {
        if (ip_payload_len >= sizeof(struct udphdr)) {
            const struct udphdr *udph = (const struct udphdr *)trans;
            info->src_port = ntohs(udph->source);
            info->dst_port = ntohs(udph->dest);

            uint16_t udp_len = ntohs(udph->len);
            size_t udp_data_len = 0;
            if (udp_len >= sizeof(struct udphdr)) {
                udp_data_len = udp_len - sizeof(struct udphdr);
                size_t max_avail = ip_payload_len - sizeof(struct udphdr);
                if (udp_data_len > max_avail) {
                    udp_data_len = max_avail;
                }
            }
            info->payload_len = udp_data_len;

            /* Check DNS port 53 */
            if (info->src_port == 53 || info->dst_port == 53) {
                info->protocol = PROTO_DNS;
                if (udp_data_len >= 12) {
                    parse_dns_query(trans + sizeof(struct udphdr), udp_data_len,
                                    info->dns_query, sizeof(info->dns_query));
                }
            } else {
                info->protocol = PROTO_UDP;
            }
        } else {
            info->protocol = PROTO_UDP;
            info->payload_len = 0;
        }
        return 0;
    }

    if (iph->protocol == IPPROTO_ICMP) {
        info->protocol = PROTO_ICMP;
        info->src_port = 0;
        info->dst_port = 0;
        if (ip_payload_len >= sizeof(struct icmphdr)) {
            info->payload_len = ip_payload_len - sizeof(struct icmphdr);
        } else {
            info->payload_len = 0;
        }
        return 0;
    }

    info->protocol = PROTO_OTHER;
    info->src_port = 0;
    info->dst_port = 0;
    info->payload_len = ip_payload_len;
    return 0;
}

/* Format a packet_info_t into a clean human-readable log line */
int format_packet_log(const packet_info_t *info, char *buf, size_t buf_size) {
    if (!info || !buf || buf_size == 0) {
        return -1;
    }

    /* Timestamp in [HH:MM:SS] format */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "[%02d:%02d:%02d]",
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    /* Protocol tag formatted to 7 characters */
    const char *proto_tag;
    switch (info->protocol) {
        case PROTO_DNS:   proto_tag = "[DNS]  "; break;
        case PROTO_TCP:   proto_tag = "[TCP]  "; break;
        case PROTO_UDP:   proto_tag = "[UDP]  "; break;
        case PROTO_ICMP:  proto_tag = "[ICMP] "; break;
        case PROTO_OTHER: proto_tag = "[OTHR] "; break;
        default:          proto_tag = "[UNK]  "; break;
    }

    /* Endpoints formatting */
    char ep[128];
    if (info->protocol == PROTO_DNS) {
        snprintf(ep, sizeof(ep), "%s -> %s", info->src_ip, info->dst_ip);
    } else if (info->protocol == PROTO_TCP || info->protocol == PROTO_UDP) {
        if (info->direction == DIR_PHONE_TO_INTERNET) {
            const char *svc = get_well_known_port_name(info->dst_port);
            if (svc) {
                snprintf(ep, sizeof(ep), "%s -> %s:%u [%s]",
                         info->src_ip, info->dst_ip, info->dst_port, svc);
            } else if (info->dst_port > 0) {
                snprintf(ep, sizeof(ep), "%s -> %s:%u",
                         info->src_ip, info->dst_ip, info->dst_port);
            } else {
                snprintf(ep, sizeof(ep), "%s -> %s", info->src_ip, info->dst_ip);
            }
        } else if (info->direction == DIR_INTERNET_TO_PHONE) {
            const char *svc = get_well_known_port_name(info->src_port);
            if (svc) {
                snprintf(ep, sizeof(ep), "%s:%u [%s] -> %s",
                         info->src_ip, info->src_port, svc, info->dst_ip);
            } else if (info->src_port > 0) {
                snprintf(ep, sizeof(ep), "%s:%u -> %s",
                         info->src_ip, info->src_port, info->dst_ip);
            } else {
                snprintf(ep, sizeof(ep), "%s -> %s", info->src_ip, info->dst_ip);
            }
        } else {
            const char *d_svc = get_well_known_port_name(info->dst_port);
            const char *s_svc = get_well_known_port_name(info->src_port);
            if (d_svc) {
                snprintf(ep, sizeof(ep), "%s -> %s:%u [%s]",
                         info->src_ip, info->dst_ip, info->dst_port, d_svc);
            } else if (s_svc) {
                snprintf(ep, sizeof(ep), "%s:%u [%s] -> %s",
                         info->src_ip, info->src_port, s_svc, info->dst_ip);
            } else if (info->src_port > 0 && info->dst_port > 0) {
                snprintf(ep, sizeof(ep), "%s:%u -> %s:%u",
                         info->src_ip, info->src_port, info->dst_ip, info->dst_port);
            } else {
                snprintf(ep, sizeof(ep), "%s -> %s", info->src_ip, info->dst_ip);
            }
        }
    } else {
        snprintf(ep, sizeof(ep), "%s -> %s", info->src_ip, info->dst_ip);
    }

    /* Detail section formatting */
    char detail[320];
    if (info->protocol == PROTO_DNS && info->dns_query[0] != '\0') {
        snprintf(detail, sizeof(detail), "Query: '%s'", info->dns_query);
    } else {
        snprintf(detail, sizeof(detail), "Len: %zu", info->payload_len);
    }

    size_t ep_len = strlen(ep);
    int pad = (ep_len < 31) ? (int)(31 - ep_len) : 2;

    int written = snprintf(buf, buf_size, "%s %s%s%*s%s",
                           time_str, proto_tag, ep, pad, "", detail);
    if (written < 0) {
        return -1;
    }
    if ((size_t)written >= buf_size) {
        buf[buf_size - 1] = '\0';
        return (int)(buf_size - 1);
    }
    return written;
}

/* Initialize packet engine on a given interface */
int packet_engine_init(const char *ap_iface, const char *phone_subnet, packet_handler_fn handler) {
    if (!ap_iface || !phone_subnet) {
        return -1;
    }

    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }

    if (!g_initialized) {
        if (pthread_mutex_init(&g_stats.lock, NULL) != 0) {
            return -1;
        }
        g_initialized = 1;
    }

    pthread_mutex_lock(&g_stats.lock);
    g_stats.total_packets = 0;
    g_stats.total_bytes_in = 0;
    g_stats.total_bytes_out = 0;
    g_stats.tcp_packets = 0;
    g_stats.udp_packets = 0;
    g_stats.dns_queries = 0;
    g_stats.icmp_packets = 0;
    g_stats.active_connections = 0;
    memset(g_connections, 0, sizeof(g_connections));
    g_active_conn_count = 0;
    pthread_mutex_unlock(&g_stats.lock);

    safe_copy_str(g_ap_iface, ap_iface, sizeof(g_ap_iface));
    safe_copy_str(g_phone_subnet, phone_subnet, sizeof(g_phone_subnet));

    g_handler = handler;

    unsigned int ifindex = if_nametoindex(ap_iface);
    if (ifindex == 0) {
        return -1;
    }

    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = (int)ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);

    if (bind(sock, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        close(sock);
        return -1;
    }

    g_sock = sock;
    return 0;
}

/* Start the capture loop (blocks, run in a thread) */
int packet_engine_run(void) {
    if (g_sock < 0) {
        return -1;
    }

    g_running = 1;
    struct pollfd pfd;
    pfd.fd = g_sock;
    pfd.events = POLLIN;

    uint8_t buffer[65536];

    while (g_running) {
        int ret = poll(&pfd, 1, 100); /* 100ms poll timeout for responsive stop */
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (ret == 0) {
            continue; /* Timeout, check g_running */
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return -1;
        }

        if (pfd.revents & POLLIN) {
            ssize_t n = recvfrom(g_sock, buffer, sizeof(buffer), 0, NULL, NULL);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                return -1;
            }

            if (n == 0) {
                continue;
            }

            packet_info_t info;
            if (parse_packet(buffer, (size_t)n, g_phone_subnet, &info) == 0) {
                update_stats(&info, (size_t)n, buffer);
                if (g_handler) {
                    g_handler(&info, buffer, (size_t)n);
                }
            }
        }
    }

    return 0;
}

/* Stop the capture loop */
void packet_engine_stop(void) {
    g_running = 0;
}

/* Get current traffic statistics (thread-safe copy) */
void packet_engine_get_stats(traffic_stats_t *out) {
    if (!out) {
        return;
    }
    if (!g_initialized) {
        memset(out, 0, sizeof(*out));
        return;
    }
    pthread_mutex_lock(&g_stats.lock);
    out->total_packets = g_stats.total_packets;
    out->total_bytes_in = g_stats.total_bytes_in;
    out->total_bytes_out = g_stats.total_bytes_out;
    out->tcp_packets = g_stats.tcp_packets;
    out->udp_packets = g_stats.udp_packets;
    out->dns_queries = g_stats.dns_queries;
    out->icmp_packets = g_stats.icmp_packets;
    out->active_connections = g_stats.active_connections;
    pthread_mutex_unlock(&g_stats.lock);
}

/* Clean up resources */
void packet_engine_cleanup(void) {
    g_running = 0;
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    if (g_initialized) {
        pthread_mutex_destroy(&g_stats.lock);
        g_initialized = 0;
    }
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_connections, 0, sizeof(g_connections));
    g_active_conn_count = 0;
    g_handler = NULL;
    g_ap_iface[0] = '\0';
    g_phone_subnet[0] = '\0';
}
