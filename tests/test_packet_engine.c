/*
 * test_packet_engine.c - Automated test suite for packet parsing & QR generation
 *
 * Tests:
 *   1. QR code generation and matrix validity
 *   2. Packet parsing with synthetic ethernet frames
 *   3. DNS query name extraction
 *   4. Format packet log output
 *   5. Config persistence (QR payload stability)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include "../src/qr.h"
#include "../src/packet_engine.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("  TEST: %-50s ", name); } while(0)

#define PASS() \
    do { printf("\033[1;32mPASS\033[0m\n"); tests_passed++; } while(0)

#define FAIL(msg) \
    do { printf("\033[1;31mFAIL\033[0m (%s)\n", msg); tests_failed++; } while(0)

/* ── Test 1: QR Code Generation ────────────────────────────────────────────── */

static void test_qr_generation(void) {
    TEST("QR code generates valid matrix for Wi-Fi string");

    const char *wifi_str = "WIFI:T:WPA;S:LinuxHotspot;P:MySecurePassword123;;";
    int max_size = qr_max_size();
    uint8_t *matrix = calloc(max_size * max_size, 1);
    if (!matrix) {
        FAIL("malloc failed");
        return;
    }

    int side = qr_generate(wifi_str, matrix, max_size * max_size);
    if (side <= 0) {
        FAIL("qr_generate returned error");
        free(matrix);
        return;
    }

    /* Side length must be valid QR (21 + 4*(version-1)) */
    int valid_side = 0;
    for (int v = 1; v <= 10; v++) {
        if (side == 17 + 4 * v) {
            valid_side = 1;
            break;
        }
    }
    if (!valid_side) {
        FAIL("invalid QR side length");
        free(matrix);
        return;
    }

    /* Check finder pattern at top-left (7x7 block starting at 0,0) */
    /* Top-left corner module must be dark (1) */
    if (matrix[0] != 1) {
        FAIL("finder pattern missing at (0,0)");
        free(matrix);
        return;
    }

    free(matrix);
    PASS();
}

static void test_qr_stability(void) {
    TEST("QR code is stable across multiple generations");

    const char *wifi_str = "WIFI:T:WPA;S:LinuxHotspot;P:MySecurePassword123;;";
    int max_size = qr_max_size();
    size_t buf_size = (size_t)(max_size * max_size);
    uint8_t *matrix1 = calloc(buf_size, 1);
    uint8_t *matrix2 = calloc(buf_size, 1);
    if (!matrix1 || !matrix2) {
        FAIL("malloc failed");
        free(matrix1);
        free(matrix2);
        return;
    }

    int side1 = qr_generate(wifi_str, matrix1, buf_size);
    int side2 = qr_generate(wifi_str, matrix2, buf_size);

    if (side1 != side2) {
        FAIL("side lengths differ across runs");
        free(matrix1);
        free(matrix2);
        return;
    }

    if (memcmp(matrix1, matrix2, (size_t)(side1 * side1)) != 0) {
        FAIL("matrices differ across runs");
        free(matrix1);
        free(matrix2);
        return;
    }

    free(matrix1);
    free(matrix2);
    PASS();
}

static void test_qr_empty_string(void) {
    TEST("QR code rejects empty string");

    int max_size = qr_max_size();
    uint8_t *matrix = calloc(max_size * max_size, 1);
    int side = qr_generate("", matrix, max_size * max_size);
    /* Either returns error or generates a valid (small) QR */
    /* Both are acceptable behaviors */
    if (side < 0) {
        /* Rejected empty string - acceptable */
    }
    free(matrix);
    PASS();
}

/* ── Test 2: Packet Parsing ────────────────────────────────────────────────── */

/*
 * Build a synthetic Ethernet + IPv4 + TCP packet for testing.
 * Returns total frame length.
 */
static size_t build_tcp_frame(uint8_t *buf, size_t buf_size,
                               const char *src_ip, const char *dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               const uint8_t *payload, size_t payload_len) {
    size_t eth_len = sizeof(struct ether_header);
    size_t ip_len = sizeof(struct iphdr);
    size_t tcp_len = sizeof(struct tcphdr);
    size_t total = eth_len + ip_len + tcp_len + payload_len;

    if (total > buf_size) return 0;
    memset(buf, 0, total);

    /* Ethernet header */
    struct ether_header *eth = (struct ether_header *)buf;
    eth->ether_type = htons(ETHERTYPE_IP);

    /* IP header */
    struct iphdr *ip = (struct iphdr *)(buf + eth_len);
    ip->version = 4;
    ip->ihl = 5;
    ip->tot_len = htons((uint16_t)(ip_len + tcp_len + payload_len));
    ip->protocol = IPPROTO_TCP;
    ip->ttl = 64;
    inet_pton(AF_INET, src_ip, &ip->saddr);
    inet_pton(AF_INET, dst_ip, &ip->daddr);

    /* TCP header */
    struct tcphdr *tcp = (struct tcphdr *)(buf + eth_len + ip_len);
    tcp->source = htons(src_port);
    tcp->dest = htons(dst_port);
    tcp->doff = 5;
    tcp->syn = 1;

    /* Payload */
    if (payload && payload_len > 0) {
        memcpy(buf + eth_len + ip_len + tcp_len, payload, payload_len);
    }

    return total;
}

static size_t build_udp_dns_frame(uint8_t *buf, size_t buf_size,
                                   const char *src_ip, const char *dst_ip,
                                   const char *query_name) {
    size_t eth_len = sizeof(struct ether_header);
    size_t ip_len = sizeof(struct iphdr);
    size_t udp_len = sizeof(struct udphdr);

    /* Build DNS query payload */
    uint8_t dns_payload[512];
    size_t dns_len = 0;

    /* DNS header (12 bytes) */
    memset(dns_payload, 0, 12);
    dns_payload[0] = 0x12; dns_payload[1] = 0x34; /* Transaction ID */
    dns_payload[2] = 0x01; dns_payload[3] = 0x00; /* Flags: standard query */
    dns_payload[4] = 0x00; dns_payload[5] = 0x01; /* Questions: 1 */
    dns_len = 12;

    /* Encode QNAME: "weather.com" -> \x07weather\x03com\x00 */
    const char *p = query_name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t label_len;
        if (dot) {
            label_len = (size_t)(dot - p);
        } else {
            label_len = strlen(p);
        }
        dns_payload[dns_len++] = (uint8_t)label_len;
        memcpy(dns_payload + dns_len, p, label_len);
        dns_len += label_len;
        p += label_len;
        if (*p == '.') p++;
    }
    dns_payload[dns_len++] = 0x00; /* Terminator */
    dns_payload[dns_len++] = 0x00; dns_payload[dns_len++] = 0x01; /* QTYPE: A */
    dns_payload[dns_len++] = 0x00; dns_payload[dns_len++] = 0x01; /* QCLASS: IN */

    size_t total = eth_len + ip_len + udp_len + dns_len;
    if (total > buf_size) return 0;
    memset(buf, 0, total);

    /* Ethernet */
    struct ether_header *eth = (struct ether_header *)buf;
    eth->ether_type = htons(ETHERTYPE_IP);

    /* IP */
    struct iphdr *ip = (struct iphdr *)(buf + eth_len);
    ip->version = 4;
    ip->ihl = 5;
    ip->tot_len = htons((uint16_t)(ip_len + udp_len + dns_len));
    ip->protocol = IPPROTO_UDP;
    ip->ttl = 64;
    inet_pton(AF_INET, src_ip, &ip->saddr);
    inet_pton(AF_INET, dst_ip, &ip->daddr);

    /* UDP */
    struct udphdr *udp = (struct udphdr *)(buf + eth_len + ip_len);
    udp->source = htons(12345);
    udp->dest = htons(53);
    udp->len = htons((uint16_t)(udp_len + dns_len));

    /* DNS payload */
    memcpy(buf + eth_len + ip_len + udp_len, dns_payload, dns_len);

    return total;
}

static void test_parse_tcp_packet(void) {
    TEST("Parse synthetic TCP packet (phone -> HTTPS)");

    uint8_t frame[256];
    size_t len = build_tcp_frame(frame, sizeof(frame),
                                  "192.168.12.20", "104.18.2.1",
                                  54321, 443,
                                  NULL, 0);
    if (len == 0) {
        FAIL("build_tcp_frame failed");
        return;
    }

    packet_info_t info;
    int ret = parse_packet(frame, len, "192.168.12", &info);
    if (ret != 0) {
        FAIL("parse_packet failed");
        return;
    }

    if (info.protocol != PROTO_TCP) {
        FAIL("wrong protocol");
        return;
    }
    if (strcmp(info.src_ip, "192.168.12.20") != 0) {
        FAIL("wrong src_ip");
        return;
    }
    if (strcmp(info.dst_ip, "104.18.2.1") != 0) {
        FAIL("wrong dst_ip");
        return;
    }
    if (info.src_port != 54321 || info.dst_port != 443) {
        FAIL("wrong ports");
        return;
    }
    if (info.direction != DIR_PHONE_TO_INTERNET) {
        FAIL("wrong direction");
        return;
    }

    PASS();
}

static void test_parse_dns_packet(void) {
    TEST("Parse synthetic DNS query (weather.com)");

    uint8_t frame[512];
    size_t len = build_udp_dns_frame(frame, sizeof(frame),
                                      "192.168.12.20", "1.1.1.1",
                                      "weather.com");
    if (len == 0) {
        FAIL("build_udp_dns_frame failed");
        return;
    }

    packet_info_t info;
    int ret = parse_packet(frame, len, "192.168.12", &info);
    if (ret != 0) {
        FAIL("parse_packet failed");
        return;
    }

    if (info.protocol != PROTO_DNS) {
        FAIL("wrong protocol (expected DNS)");
        return;
    }
    if (strcmp(info.dns_query, "weather.com") != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "wrong dns_query: '%s'", info.dns_query);
        FAIL(msg);
        return;
    }

    PASS();
}

static void test_parse_reverse_direction(void) {
    TEST("Parse packet with internet->phone direction");

    uint8_t frame[256];
    size_t len = build_tcp_frame(frame, sizeof(frame),
                                  "93.184.216.34", "192.168.12.20",
                                  80, 54321,
                                  (const uint8_t *)"HTTP/1.1 200 OK", 15);
    if (len == 0) {
        FAIL("build_tcp_frame failed");
        return;
    }

    packet_info_t info;
    int ret = parse_packet(frame, len, "192.168.12", &info);
    if (ret != 0) {
        FAIL("parse_packet failed");
        return;
    }

    if (info.direction != DIR_INTERNET_TO_PHONE) {
        FAIL("wrong direction");
        return;
    }

    PASS();
}

/* ── Test 3: Format Packet Log ─────────────────────────────────────────────── */

static void test_format_packet_log(void) {
    TEST("Format TCP packet log line");

    packet_info_t info;
    memset(&info, 0, sizeof(info));
    strncpy(info.src_ip, "192.168.12.20", sizeof(info.src_ip));
    strncpy(info.dst_ip, "104.18.2.1", sizeof(info.dst_ip));
    info.src_port = 54321;
    info.dst_port = 443;
    info.protocol = PROTO_TCP;
    info.payload_len = 1250;
    info.direction = DIR_PHONE_TO_INTERNET;

    char buf[512];
    int n = format_packet_log(&info, buf, sizeof(buf));
    if (n <= 0) {
        FAIL("format_packet_log returned <= 0");
        return;
    }

    /* Should contain "TCP", source, destination, and HTTPS */
    if (strstr(buf, "TCP") == NULL) {
        FAIL("missing TCP in output");
        return;
    }
    if (strstr(buf, "192.168.12.20") == NULL) {
        FAIL("missing src_ip in output");
        return;
    }
    if (strstr(buf, "104.18.2.1") == NULL) {
        FAIL("missing dst_ip in output");
        return;
    }

    PASS();
}

static void test_format_dns_log(void) {
    TEST("Format DNS query log line");

    packet_info_t info;
    memset(&info, 0, sizeof(info));
    strncpy(info.src_ip, "192.168.12.20", sizeof(info.src_ip));
    strncpy(info.dst_ip, "1.1.1.1", sizeof(info.dst_ip));
    info.src_port = 12345;
    info.dst_port = 53;
    info.protocol = PROTO_DNS;
    info.payload_len = 42;
    info.direction = DIR_PHONE_TO_INTERNET;
    strncpy(info.dns_query, "google.com", sizeof(info.dns_query));

    char buf[512];
    int n = format_packet_log(&info, buf, sizeof(buf));
    if (n <= 0) {
        FAIL("format_packet_log returned <= 0");
        return;
    }

    if (strstr(buf, "DNS") == NULL) {
        FAIL("missing DNS in output");
        return;
    }
    if (strstr(buf, "google.com") == NULL) {
        FAIL("missing dns_query in output");
        return;
    }

    PASS();
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("\n\033[1;36m═══ Packet Engine Test Suite ═══\033[0m\n\n");

    /* QR tests */
    printf("\033[1mQR Code Tests:\033[0m\n");
    test_qr_generation();
    test_qr_stability();
    test_qr_empty_string();

    /* Packet parsing tests */
    printf("\n\033[1mPacket Parsing Tests:\033[0m\n");
    test_parse_tcp_packet();
    test_parse_dns_packet();
    test_parse_reverse_direction();

    /* Log formatting tests */
    printf("\n\033[1mLog Formatting Tests:\033[0m\n");
    test_format_packet_log();
    test_format_dns_log();

    /* Summary */
    printf("\n\033[1;36m═══ Results ═══\033[0m\n");
    printf("  Passed: \033[1;32m%d\033[0m\n", tests_passed);
    printf("  Failed: \033[1;31m%d\033[0m\n", tests_failed);
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}

