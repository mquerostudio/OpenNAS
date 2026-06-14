/*
 * Minimal DNS hijack: listen on UDP :53, answer every A query with a
 * single fixed IP. No recursion, no EDNS, no compression beyond echoing
 * the question back (which RFC 1035 §4.1 allows).
 *
 * Packet layout we produce (big-endian):
 *   [ID 2B][FLAGS 2B = 0x8180][QD 2B = 1][AN 2B = 1][NS 2B = 0][AR 2B = 0]
 *   [echoed question (QNAME + QTYPE 2B + QCLASS 2B)]
 *   [0xC00C (pointer to offset 12, the QNAME)]
 *   [TYPE 2B = 1 (A)][CLASS 2B = 1 (IN)][TTL 4B = 60][RDLENGTH 2B = 4][RDATA 4B = ip]
 */

#include "dns_hijack.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "dns_hijack";

#define DNS_PORT        53
#define MAX_QUERY_LEN   512

static TaskHandle_t s_task = NULL;
static int s_sock = -1;
static uint32_t s_target_ip_be = 0;

static int build_answer(const uint8_t *query, int qlen, uint8_t *out, int out_max)
{
    if (qlen < 12 || qlen + 16 > out_max) return -1;

    /* Find end of QNAME + 4 bytes (QTYPE, QCLASS) */
    int p = 12;
    while (p < qlen && query[p] != 0) {
        int len = query[p];
        if (len & 0xC0) return -1;   /* refuse compression in question */
        p += len + 1;
        if (p >= qlen) return -1;
    }
    p += 1 + 4;  /* null byte + QTYPE + QCLASS */
    if (p > qlen) return -1;

    int question_len = p - 12;
    int total = 12 + question_len + 16;
    if (total > out_max) return -1;

    /* Header */
    memcpy(out, query, 2);         /* ID */
    out[2] = 0x81;                 /* QR=1, Opcode=0, AA=0, TC=0, RD=1 */
    out[3] = 0x80;                 /* RA=1, Z=0, RCODE=0 */
    out[4] = 0; out[5] = 1;        /* QDCOUNT = 1 */
    out[6] = 0; out[7] = 1;        /* ANCOUNT = 1 */
    out[8] = 0; out[9] = 0;        /* NSCOUNT */
    out[10] = 0; out[11] = 0;      /* ARCOUNT */

    /* Question echoed back */
    memcpy(out + 12, query + 12, question_len);

    /* Answer */
    uint8_t *ans = out + 12 + question_len;
    ans[0] = 0xC0; ans[1] = 0x0C;  /* pointer to offset 12 */
    ans[2] = 0;    ans[3] = 1;     /* TYPE A */
    ans[4] = 0;    ans[5] = 1;     /* CLASS IN */
    ans[6] = 0; ans[7] = 0; ans[8] = 0; ans[9] = 60;   /* TTL 60 s */
    ans[10] = 0; ans[11] = 4;      /* RDLENGTH 4 */
    memcpy(ans + 12, &s_target_ip_be, 4);

    return total;
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t rx[MAX_QUERY_LEN];
    uint8_t tx[MAX_QUERY_LEN];

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind failed: %d", errno);
        close(s_sock);
        s_sock = -1;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS hijack active on :53");

    for (;;) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        int n = recvfrom(s_sock, rx, sizeof(rx), 0, (struct sockaddr *)&src, &src_len);
        if (n <= 0) {
            if (errno == EINTR) continue;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int ans_len = build_answer(rx, n, tx, sizeof(tx));
        if (ans_len < 0) continue;
        sendto(s_sock, tx, ans_len, 0, (struct sockaddr *)&src, src_len);
    }
}

esp_err_t dns_hijack_start(uint32_t target_ip_be)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    s_target_ip_be = target_ip_be;
    BaseType_t r = xTaskCreate(dns_task, "dns_hijack", 3072, NULL, 4, &s_task);
    return (r == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

void dns_hijack_stop(void)
{
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
}
