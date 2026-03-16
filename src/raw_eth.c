/*
 * raw_eth.c — Raw Ethernet Layer 2 framework
 *
 * Provides an AF_PACKET/SOCK_RAW receive thread that dispatches frames to
 * protocol-specific stub handlers for:
 *   - GOOSE     (IEC 61850-8-1, EtherType 0x88B8)
 *   - RSTP BPDU (IEEE 802.1w, LLC to 01:80:C2:00:00:00)
 *   - HSR tag   (IEC 62439-3, EtherType 0x892F)
 *   - HSR/PRP supervision (EtherType 0x88FB)
 *
 * Two RX threads: one for ENET0 (switch_port@0) and one for ENET4 (enetc_psi0).
 * Also exposes shell commands: raw_eth status / raw_eth send goose|rstp
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <string.h>
#include <errno.h>

#include "raw_eth.h"

LOG_MODULE_REGISTER(raw_eth, LOG_LEVEL_INF);

/* Shared with main.c */
extern bool network_ready;
extern bool network_ready2;
extern struct net_if *eth1_iface;

#ifndef ETH_P_ALL
#define ETH_P_ALL 0x0003
#endif

/* One socket per physical port */
static int raw_sock  = -1;   /* ENET0 (switch_port@0) */
static int raw_sock2 = -1;   /* ENET4 (enetc_psi0)    */

/* Per-protocol RX counters */
static uint32_t rx_goose;
static uint32_t rx_rstp;
static uint32_t rx_hsr;
static uint32_t rx_hsr_sup;

/* Total counters */
static uint32_t rx_total;
static uint32_t tx_count;

/* TX serialisation */
static K_MUTEX_DEFINE(tx_mutex);

/* Well-known multicast MACs */
const uint8_t ETH_ADDR_STP[6]     = { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x00 };
const uint8_t ETH_ADDR_HSR_SUP[6] = { 0x01, 0x15, 0x4E, 0x00, 0x01, 0x00 };

/* ================================================================
 * Protocol stub handlers (RX)
 * ================================================================ */

static void goose_rx(struct raw_eth_hdr *hdr, uint8_t *payload, int len)
{
	LOG_INF("GOOSE RX from %02x:%02x:%02x:%02x:%02x:%02x len=%d",
		hdr->src[0], hdr->src[1], hdr->src[2],
		hdr->src[3], hdr->src[4], hdr->src[5], len);
	/* TODO: parse IEC 61850 GOOSE PDU (ASN.1/BER) */
}

static void rstp_rx(struct raw_eth_hdr *hdr, uint8_t *payload, int len)
{
	LOG_INF("RSTP BPDU RX from %02x:%02x:%02x:%02x:%02x:%02x len=%d",
		hdr->src[0], hdr->src[1], hdr->src[2],
		hdr->src[3], hdr->src[4], hdr->src[5], len);
	/* TODO: parse IEEE 802.1D / 802.1w BPDU fields */
}

static void hsr_rx(struct raw_eth_hdr *hdr, uint8_t *payload, int len)
{
	LOG_INF("HSR frame RX tag=0x892F from %02x:%02x:%02x:%02x:%02x:%02x len=%d",
		hdr->src[0], hdr->src[1], hdr->src[2],
		hdr->src[3], hdr->src[4], hdr->src[5], len);
}

static void hsr_prp_supervision_rx(struct raw_eth_hdr *hdr, uint8_t *payload, int len)
{
	LOG_INF("HSR/PRP supervision RX from %02x:%02x:%02x:%02x:%02x:%02x len=%d",
		hdr->src[0], hdr->src[1], hdr->src[2],
		hdr->src[3], hdr->src[4], hdr->src[5], len);
}

/* ================================================================
 * Shared frame dispatcher (called by both RX threads)
 * ================================================================ */

static int diag_count;

static void dispatch_frame(const uint8_t *frame_buf, int len, const char *iface_label)
{
	struct raw_eth_hdr *hdr = (struct raw_eth_hdr *)frame_buf;
	uint16_t etype           = ntohs(hdr->ethertype);
	uint8_t *payload         = (uint8_t *)frame_buf + sizeof(struct raw_eth_hdr);
	int payload_len          = len - (int)sizeof(struct raw_eth_hdr);

	rx_total++;

	/* Diagnostic: dump first 20 non-IP/ARP frames across both ports */
	if (etype != 0x0800 && etype != 0x0806 && etype != 0x86DD &&
	    etype != 0x8100 && diag_count < 20) {
		diag_count++;
		LOG_INF("[%s] etype=0x%04x len=%d "
			"%02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x",
			iface_label, etype, len,
			frame_buf[0],  frame_buf[1],  frame_buf[2],  frame_buf[3],
			frame_buf[4],  frame_buf[5],  frame_buf[6],  frame_buf[7],
			frame_buf[8],  frame_buf[9],  frame_buf[10], frame_buf[11],
			frame_buf[12], frame_buf[13], frame_buf[14], frame_buf[15]);
	}

	switch (etype) {
	case ETH_P_GOOSE:
		rx_goose++;
		goose_rx(hdr, payload, payload_len);
		break;
	case ETH_P_HSR:
		rx_hsr++;
		hsr_rx(hdr, payload, payload_len);
		break;
	case ETH_P_HSR_SUP:
		rx_hsr_sup++;
		hsr_prp_supervision_rx(hdr, payload, payload_len);
		break;
	default:
		if (etype < 0x0600 &&
		    memcmp(hdr->dst, ETH_ADDR_STP, 6) == 0) {
			rx_rstp++;
			rstp_rx(hdr, payload, payload_len);
		}
		break;
	}
}

/* ================================================================
 * Generic RX thread
 *   p1 = bool *ready    — wait until true before opening socket
 *   p2 = struct net_if ** — pointer to iface variable (NULL → default)
 *   p3 = int *sock_out  — where to store the opened socket fd
 * ================================================================ */

#define RAW_ETH_STACK_SIZE 4096
#define RAW_ETH_PRIORITY   7

K_THREAD_STACK_DEFINE(raw_eth_stack,  RAW_ETH_STACK_SIZE);
K_THREAD_STACK_DEFINE(raw_eth_stack2, RAW_ETH_STACK_SIZE);
static struct k_thread raw_eth_thread_data;
static struct k_thread raw_eth_thread2_data;

static void raw_eth_rx_thread(void *p1, void *p2, void *p3)
{
	bool            *ready    = (bool *)p1;
	struct net_if  **iface_ref = (struct net_if **)p2;
	int             *sock_out  = (int *)p3;

	/* Wait for IP stack / link to be ready */
	while (!(*ready)) {
		k_msleep(500);
	}

	struct net_if *iface = (iface_ref && *iface_ref)
				? *iface_ref
				: net_if_get_default();

	const char *label = (iface == net_if_get_default())
			    ? "ENET0" : "ENET4";

	int sock = zsock_socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (sock < 0) {
		LOG_ERR("[%s] Failed to create AF_PACKET socket: %d", label, errno);
		return;
	}

	*sock_out = sock;

	struct sockaddr_ll bind_addr = {
		.sll_family   = AF_PACKET,
		.sll_protocol = htons(ETH_P_ALL),
		.sll_ifindex  = net_if_get_by_iface(iface),
	};

	if (zsock_bind(sock, (struct sockaddr *)&bind_addr,
		       sizeof(bind_addr)) < 0) {
		LOG_ERR("[%s] Failed to bind raw socket: %d", label, errno);
		zsock_close(sock);
		*sock_out = -1;
		return;
	}

	int ret = net_if_set_promisc(iface);
	if (ret < 0) {
		LOG_WRN("[%s] Promiscuous mode not supported: %d", label, ret);
	} else {
		LOG_INF("[%s] Promiscuous mode enabled", label);
	}

	LOG_INF("[%s] RX thread started (ifindex=%d)", label, bind_addr.sll_ifindex);

	uint8_t frame_buf[1518];

	while (1) {
		int len = zsock_recv(sock, frame_buf, sizeof(frame_buf), 0);

		if (len < (int)sizeof(struct raw_eth_hdr)) {
			continue;
		}

		dispatch_frame(frame_buf, len, label);
	}
}

/* ================================================================
 * TX helpers (always send via ENET0 socket)
 * ================================================================ */

int raw_eth_send(const uint8_t *dst_mac, uint16_t ethertype,
		 const uint8_t *payload, size_t payload_len)
{
	static uint8_t tx_buf[1518];

	if (raw_sock < 0) {
		return -ENODEV;
	}
	if (payload_len > sizeof(tx_buf) - sizeof(struct raw_eth_hdr)) {
		return -EMSGSIZE;
	}

	k_mutex_lock(&tx_mutex, K_FOREVER);

	struct net_if *iface = net_if_get_default();
	struct raw_eth_hdr *hdr = (struct raw_eth_hdr *)tx_buf;

	memcpy(hdr->dst, dst_mac, 6);
	memcpy(hdr->src, net_if_get_link_addr(iface)->addr, 6);
	hdr->ethertype = htons(ethertype);
	memcpy(tx_buf + sizeof(*hdr), payload, payload_len);

	struct sockaddr_ll dst_addr = {
		.sll_family  = AF_PACKET,
		.sll_ifindex = net_if_get_by_iface(iface),
		.sll_halen   = 6,
	};
	memcpy(dst_addr.sll_addr, dst_mac, 6);

	int ret = zsock_sendto(raw_sock, tx_buf,
			       sizeof(*hdr) + payload_len, 0,
			       (struct sockaddr *)&dst_addr, sizeof(dst_addr));
	if (ret > 0) {
		tx_count++;
	}

	k_mutex_unlock(&tx_mutex);
	return ret;
}

int goose_send(const uint8_t *dst, const uint8_t *goose_pdu, size_t pdu_len)
{
	return raw_eth_send(dst, ETH_P_GOOSE, goose_pdu, pdu_len);
}

int rstp_send_bpdu(const uint8_t *bpdu, size_t bpdu_len)
{
	static uint8_t llc_bpdu[3 + 1518];

	if (bpdu_len > sizeof(llc_bpdu) - 3) {
		return -EMSGSIZE;
	}

	llc_bpdu[0] = 0x42;
	llc_bpdu[1] = 0x42;
	llc_bpdu[2] = 0x03;
	memcpy(llc_bpdu + 3, bpdu, bpdu_len);

	return raw_eth_send(ETH_ADDR_STP, (uint16_t)(3 + bpdu_len),
			    llc_bpdu, 3 + bpdu_len);
}

/* ================================================================
 * Public init — starts one RX thread per physical port
 * ================================================================ */

void raw_eth_start(void)
{
	k_thread_create(&raw_eth_thread_data, raw_eth_stack,
			K_THREAD_STACK_SIZEOF(raw_eth_stack),
			raw_eth_rx_thread,
			&network_ready, NULL, &raw_sock,
			RAW_ETH_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&raw_eth_thread_data, "raw_eth_enet0");

	k_thread_create(&raw_eth_thread2_data, raw_eth_stack2,
			K_THREAD_STACK_SIZEOF(raw_eth_stack2),
			raw_eth_rx_thread,
			&network_ready2, &eth1_iface, &raw_sock2,
			RAW_ETH_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&raw_eth_thread2_data, "raw_eth_enet4");

	LOG_INF("Raw Ethernet framework initialised (ENET0 + ENET4)");
}

/* ================================================================
 * Web terminal helpers
 * ================================================================ */

int raw_eth_status_str(char *buf, size_t buf_size)
{
	return snprintf(buf, buf_size,
		"=== Raw Ethernet Framework ===\n"
		"ENET0 sock: %d (%s)\n"
		"ENET4 sock: %d (%s)\n"
		"RX GOOSE  : %u\n"
		"RX RSTP   : %u\n"
		"RX HSR    : %u\n"
		"RX HSR/sup: %u\n"
		"RX total  : %u\n"
		"TX total  : %u",
		raw_sock,  raw_sock  >= 0 ? "open" : "not open",
		raw_sock2, raw_sock2 >= 0 ? "open" : "not open",
		rx_goose, rx_rstp, rx_hsr, rx_hsr_sup, rx_total, tx_count);
}

int raw_eth_test_send_goose(const uint8_t *dst)
{
	static const uint8_t test_goose[] = {
		0x61, 0x04, 0x00, 0x00, 0x00, 0x00
	};
	return goose_send(dst, test_goose, sizeof(test_goose));
}

int raw_eth_test_send_rstp(void)
{
	static const uint8_t test_bpdu[] = {
		0x00, 0x00, 0x02, 0x02, 0x3C,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x80, 0x01, 0x00, 0x00, 0x14, 0x00, 0x02, 0x00,
		0x0F, 0x00, 0x00,
	};
	return rstp_send_bpdu(test_bpdu, sizeof(test_bpdu));
}

/* ================================================================
 * Shell commands
 * ================================================================ */

static int cmd_raw_eth_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "=== Raw Ethernet Framework ===");
	shell_print(sh, "ENET0 sock: %d (%s)", raw_sock,
		    raw_sock  >= 0 ? "open" : "not open");
	shell_print(sh, "ENET4 sock: %d (%s)", raw_sock2,
		    raw_sock2 >= 0 ? "open" : "not open");
	shell_print(sh, "RX GOOSE  : %u", rx_goose);
	shell_print(sh, "RX RSTP   : %u", rx_rstp);
	shell_print(sh, "RX HSR    : %u", rx_hsr);
	shell_print(sh, "RX HSR/sup: %u", rx_hsr_sup);
	shell_print(sh, "RX total  : %u", rx_total);
	shell_print(sh, "TX total  : %u", tx_count);
	return 0;
}

/* Parse colon-separated hex MAC, e.g. "01:0c:cd:04:00:01" */
static int parse_mac(const char *str, uint8_t *mac)
{
	unsigned int b[6];
	int n = sscanf(str, "%x:%x:%x:%x:%x:%x",
		       &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
	if (n != 6) {
		return -EINVAL;
	}
	for (int i = 0; i < 6; i++) {
		mac[i] = (uint8_t)b[i];
	}
	return 0;
}

static int cmd_raw_eth_send_goose(const struct shell *sh,
				  size_t argc, char **argv)
{
	uint8_t dst[6];

	if (argc < 2) {
		shell_error(sh, "Usage: raw_eth send goose <dst_mac>");
		shell_print(sh, "  e.g.: raw_eth send goose 01:0c:cd:04:00:01");
		return -EINVAL;
	}

	if (parse_mac(argv[1], dst) != 0) {
		shell_error(sh, "Invalid MAC address: %s", argv[1]);
		return -EINVAL;
	}

	static const uint8_t test_goose[] = {
		0x61, 0x04,
		0x00, 0x00, 0x00, 0x00
	};

	int ret = goose_send(dst, test_goose, sizeof(test_goose));

	if (ret < 0) {
		shell_error(sh, "goose_send failed: %d", ret);
		return ret;
	}

	shell_print(sh, "GOOSE test frame sent to %02x:%02x:%02x:%02x:%02x:%02x (%d bytes)",
		    dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], ret);
	return 0;
}

static int cmd_raw_eth_send_rstp(const struct shell *sh,
				 size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	static const uint8_t test_bpdu[] = {
		0x00, 0x00,
		0x02,
		0x02,
		0x3C,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x80, 0x01,
		0x00, 0x00,
		0x14, 0x00,
		0x02, 0x00,
		0x0F, 0x00,
		0x00,
	};

	int ret = rstp_send_bpdu(test_bpdu, sizeof(test_bpdu));

	if (ret < 0) {
		shell_error(sh, "rstp_send_bpdu failed: %d", ret);
		return ret;
	}

	shell_print(sh, "RSTP BPDU test frame sent to 01:80:C2:00:00:00 (%d bytes)", ret);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(raw_eth_send_cmds,
	SHELL_CMD_ARG(goose, NULL,
		"Send test GOOSE frame\nUsage: raw_eth send goose <dst_mac>",
		cmd_raw_eth_send_goose, 2, 0),
	SHELL_CMD(rstp, NULL,
		"Send test RSTP BPDU to 01:80:C2:00:00:00",
		cmd_raw_eth_send_rstp),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(raw_eth_cmds,
	SHELL_CMD(status, NULL,
		"Show raw Ethernet socket state and RX/TX counters",
		cmd_raw_eth_status),
	SHELL_CMD(send, &raw_eth_send_cmds,
		"Send a test frame (goose|rstp)",
		NULL),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(raw_eth, &raw_eth_cmds,
	"Raw Ethernet framework (GOOSE/RSTP/HSR/PRP)", NULL);
