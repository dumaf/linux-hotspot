/*
 * packet_engine - Pure C Packet Monitoring & Wi-Fi QR Code for Linux Hotspot
 *
 * Main entry point: parses CLI arguments, prints Wi-Fi QR code,
 * and runs the raw-socket packet capture engine.
 *
 * Usage:
 *   ./packet_engine --iface ap0 --subnet 192.168.12
 *   ./packet_engine --qr-only --ssid MySSID --password MyPass
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>

#include "qr.h"
#include "packet_engine.h"

/* ── Configuration ─────────────────────────────────────────────────────────── */

typedef struct {
    char iface[64];         /* AP interface name (e.g., "ap0") */
    char subnet[32];        /* Subnet prefix (e.g., "192.168.12") */
    char ssid[64];          /* Wi-Fi SSID for QR code */
    char password[64];      /* Wi-Fi password for QR code */
    int  qr_only;           /* If set, just print QR and exit */
    int  stats_interval;    /* Seconds between stats refresh (default: 5) */
} config_t;

static volatile sig_atomic_t g_running = 1;

/* ── Signal Handler ────────────────────────────────────────────────────────── */

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
    packet_engine_stop();
}

/* ── Wi-Fi QR Code ─────────────────────────────────────────────────────────── */

static void print_wifi_qr(const char *ssid, const char *password) {
    char wifi_str[512];
    snprintf(wifi_str, sizeof(wifi_str),
             "WIFI:T:WPA;S:%s;P:%s;;", ssid, password);

    printf("\n");
    printf("\033[1;36m╔══════════════════════════════════════════╗\033[0m\n");
    printf("\033[1;36m║\033[0m  \033[1mScan this QR code to connect to Wi-Fi\033[0m  \033[1;36m║\033[0m\n");
    printf("\033[1;36m╚══════════════════════════════════════════╝\033[0m\n");
    printf("\n");

    if (qr_print_terminal(wifi_str) != 0) {
        fprintf(stderr, "[packet_engine] Warning: QR generation failed, "
                "printing text fallback.\n");
    }

    printf("\n");
    printf("  \033[1mSSID:\033[0m     %s\n", ssid);
    printf("  \033[1mPassword:\033[0m %s\n", password);
    printf("  \033[1mSecurity:\033[0m WPA2\n");
    printf("\n");
}

/* ── Stats Dashboard ───────────────────────────────────────────────────────── */

static void *stats_thread(void *arg) {
    int interval = *(int *)arg;
    traffic_stats_t stats;
    char time_buf[32];

    while (g_running) {
        sleep(interval);
        if (!g_running) break;

        packet_engine_get_stats(&stats);

        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm);

        printf("\n\033[1;33m── Dashboard [%s] ──────────────────────────\033[0m\n",
               time_buf);
        printf("  Packets:     %" PRIu64 "\n", stats.total_packets);
        printf("  Bytes In:    %" PRIu64 " (internet → phone)\n", stats.total_bytes_in);
        printf("  Bytes Out:   %" PRIu64 " (phone → internet)\n", stats.total_bytes_out);
        printf("  TCP:         %" PRIu64 "  UDP: %" PRIu64
               "  DNS: %" PRIu64 "  ICMP: %" PRIu64 "\n",
               stats.tcp_packets, stats.udp_packets,
               stats.dns_queries, stats.icmp_packets);
        printf("\033[1;33m───────────────────────────────────────────────\033[0m\n");
        fflush(stdout);
    }
    return NULL;
}

/* ── Packet Display Callback ───────────────────────────────────────────────── */

static int packet_display_handler(const packet_info_t *info,
                                   const uint8_t *raw_pkt,
                                   size_t raw_len) {
    (void)raw_pkt;
    (void)raw_len;

    char buf[512];
    format_packet_log(info, buf, sizeof(buf));
    printf("%s\n", buf);
    fflush(stdout);
    return 0; /* continue processing */
}

/* ── Usage ─────────────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s --iface <AP_IFACE> --subnet <PREFIX> [options]\n", prog);
    printf("  %s --qr-only --ssid <SSID> --password <PASS>\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  --iface <name>       AP interface to monitor (e.g., ap0)\n");
    printf("  --subnet <prefix>    Phone subnet prefix (e.g., 192.168.12)\n");
    printf("  --ssid <name>        Wi-Fi SSID (for QR code display)\n");
    printf("  --password <pass>    Wi-Fi password (for QR code display)\n");
    printf("  --qr-only            Print QR code and exit\n");
    printf("  --stats-interval <s> Seconds between stats updates (default: 5)\n");
    printf("  -h, --help           Show this help\n");
}

/* ── Argument Parsing ──────────────────────────────────────────────────────── */

static int parse_args(int argc, char *argv[], config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->stats_interval = 5;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--iface") == 0) && i + 1 < argc) {
            snprintf(cfg->iface, sizeof(cfg->iface), "%s", argv[++i]);
        } else if ((strcmp(argv[i], "--subnet") == 0) && i + 1 < argc) {
            snprintf(cfg->subnet, sizeof(cfg->subnet), "%s", argv[++i]);
        } else if ((strcmp(argv[i], "--ssid") == 0) && i + 1 < argc) {
            snprintf(cfg->ssid, sizeof(cfg->ssid), "%s", argv[++i]);
        } else if ((strcmp(argv[i], "--password") == 0) && i + 1 < argc) {
            snprintf(cfg->password, sizeof(cfg->password), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--qr-only") == 0) {
            cfg->qr_only = 1;
        } else if ((strcmp(argv[i], "--stats-interval") == 0) && i + 1 < argc) {
            cfg->stats_interval = atoi(argv[++i]);
            if (cfg->stats_interval < 1) cfg->stats_interval = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    config_t cfg;

    if (parse_args(argc, argv, &cfg) != 0) {
        return 1;
    }

    /* QR-only mode: print QR and exit */
    if (cfg.qr_only) {
        if (cfg.ssid[0] == '\0' || cfg.password[0] == '\0') {
            fprintf(stderr, "Error: --qr-only requires --ssid and --password.\n");
            return 1;
        }
        print_wifi_qr(cfg.ssid, cfg.password);
        return 0;
    }

    /* Packet capture mode: requires --iface and --subnet */
    if (cfg.iface[0] == '\0' || cfg.subnet[0] == '\0') {
        fprintf(stderr, "Error: --iface and --subnet are required for packet capture.\n");
        usage(argv[0]);
        return 1;
    }

    /* Set up signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Print QR code if SSID/password are provided */
    if (cfg.ssid[0] != '\0' && cfg.password[0] != '\0') {
        print_wifi_qr(cfg.ssid, cfg.password);
    }

    printf("\033[1;32m[packet_engine]\033[0m Starting packet capture on %s "
           "(subnet: %s.x)...\n", cfg.iface, cfg.subnet);

    /* Initialize packet engine */
    if (packet_engine_init(cfg.iface, cfg.subnet, packet_display_handler) != 0) {
        fprintf(stderr, "[packet_engine] Failed to initialize. "
                "Are you running as root?\n");
        return 1;
    }

    /* Launch stats dashboard thread */
    pthread_t stats_tid;
    pthread_create(&stats_tid, NULL, stats_thread, &cfg.stats_interval);

    /* Run capture loop (blocks until stopped) */
    int ret = packet_engine_run();

    /* Clean up */
    g_running = 0;
    pthread_join(stats_tid, NULL);
    packet_engine_cleanup();

    printf("\n\033[1;32m[packet_engine]\033[0m Stopped.\n");
    return ret;
}
