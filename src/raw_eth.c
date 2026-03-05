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

/* Shared with main.c — network_ready is non-static there */
extern bool network_ready;

#ifndef ETH_P_ALL
#define ETH_P_ALL 0x0003
#endif

/* Single socket used for both RX and TX */
static int raw_sock = -1;

/* Per-protocol RX counters */
static uint32_t rx_goose;
static uint32_t rx_rstp;
static uint32_t rx_hsr;
static uint32_t rx_hsr_sup;

/* Total TX counter */
static uint32_t tx_count;

/* Total RX frames (all EtherTypes) — for diagnostics */
static uint32_t rx_total;

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
	/* TODO: strip HSR tag (6 bytes: EtherType + PathID + LSDU_size + SeqNr),
	 *       duplicate detection via sequence number ring */
}

static void hsr_prp_supervision_rx(struct raw_eth_hdr *hdr, uint8_t *payload, int len)
{
	LOG_INF("HSR/PRP supervision RX from %02x:%02x:%02x:%02x:%02x:%02x len=%d",
		hdr->src[0], hdr->src[1], hdr->src[2],
		hdr->src[3], hdr->src[4], hdr->src[5], len);
	/* TODO: update HSR/PRP node table from supervision TLVs */
}

/* ================================================================
 * RX thread
 * ================================================================ */

#define RAW_ETH_STACK_SIZE 4096
#define RAW_ETH_PRIORITY   7

K_THREAD_STACK_DEFINE(raw_eth_stack, RAW_ETH_STACK_SIZE);
static struct k_thread raw_eth_thread_data;

static void raw_eth_rx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Wait for Ethernet link / IP stack to be ready */
	while (!network_ready) {
		k_msleep(500);
	}

	struct net_if *iface = net_if_get_default();

	raw_sock = zsock_socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (raw_sock < 0) {
		LOG_ERR("Failed to create AF_PACKET socket: %d", errno);
		return;
	}

	struct sockaddr_ll bind_addr = {
		.sll_family   = AF_PACKET,
		.sll_protocol = htons(ETH_P_ALL),
		.sll_ifindex  = net_if_get_by_iface(iface),
	};

	if (zsock_bind(raw_sock, (struct sockaddr *)&bind_addr,
		       sizeof(bind_addr)) < 0) {
		LOG_ERR("Failed to bind raw socket: %d", errno);
		zsock_close(raw_sock);
		raw_sock = -1;
		return;
	}

	/* Enable promiscuous mode so the NIC passes all L2 frames (including
	 * GOOSE/HSR/PRP multicasts not in the hardware MAC filter) */
	int promisc_ret = net_if_set_promisc(iface);
	if (promisc_ret < 0) {
		LOG_WRN("Promiscuous mode not supported by driver: %d", promisc_ret);
	} else {
		LOG_INF("Promiscuous mode enabled");
	}

	LOG_INF("RX thread started (ifindex=%d)", bind_addr.sll_ifindex);

	static uint8_t frame_buf[1518];

	while (1) {
		int len = zsock_recv(raw_sock, frame_buf, sizeof(frame_buf), 0);

		if (len < (int)sizeof(struct raw_eth_hdr)) {
			continue;
		}

		rx_total++;
		struct raw_eth_hdr *hdr = (struct raw_eth_hdr *)frame_buf;
		uint16_t etype           = ntohs(hdr->ethertype);
		uint8_t *payload         = frame_buf + sizeof(struct raw_eth_hdr);
		int payload_len          = len - (int)sizeof(struct raw_eth_hdr);

		/* Diagnostic: dump first 20 bytes of any non-IP/ARP/IPv6 frame */
		static int diag_count;
		if (etype != 0x0800 && etype != 0x0806 && etype != 0x86DD &&
		    etype != 0x8100 && diag_count < 20) {
			diag_count++;
			LOG_INF("Frame etype=0x%04x len=%d "
				"bytes: %02x %02x %02x %02x %02x %02x %02x %02x "
				"%02x %02x %02x %02x %02x %02x %02x %02x",
				etype, len,
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
			/* RSTP/STP BPDU: length field < 0x0600 and well-known dst */
			if (etype < 0x0600 &&
			    memcmp(hdr->dst, ETH_ADDR_STP, 6) == 0) {
				rx_rstp++;
				rstp_rx(hdr, payload, payload_len);
			}
			break;
		}
	}
}

/* ================================================================
 * TX helpers
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
	/*
	 * RSTP frames use IEEE 802.3 length + LLC header, not an EtherType.
	 * Layout after Ethernet header:
	 *   [2] length  = 3 (LLC) + bpdu_len
	 *   [1] DSAP    = 0x42
	 *   [1] SSAP    = 0x42
	 *   [1] Control = 0x03
	 *   [N] BPDU data
	 *
	 * We build the payload here (LLC + BPDU) and let raw_eth_send fill
	 * the Ethernet header; the "ethertype" field becomes the length.
	 */
	static uint8_t llc_bpdu[3 + 1518];

	if (bpdu_len > sizeof(llc_bpdu) - 3) {
		return -EMSGSIZE;
	}

	llc_bpdu[0] = 0x42; /* DSAP: STP */
	llc_bpdu[1] = 0x42; /* SSAP: STP */
	llc_bpdu[2] = 0x03; /* LLC UI */
	memcpy(llc_bpdu + 3, bpdu, bpdu_len);

	uint16_t length = (uint16_t)(3 + bpdu_len);

	return raw_eth_send(ETH_ADDR_STP, length, llc_bpdu, 3 + bpdu_len);
}

/* ================================================================
 * Public init
 * ================================================================ */

void raw_eth_start(void)
{
	k_thread_create(&raw_eth_thread_data, raw_eth_stack,
			K_THREAD_STACK_SIZEOF(raw_eth_stack),
			raw_eth_rx_thread, NULL, NULL, NULL,
			RAW_ETH_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&raw_eth_thread_data, "raw_eth_rx");
	LOG_INF("Raw Ethernet framework initialised");
}

/* ================================================================
 * Web terminal helpers
 * ================================================================ */

int raw_eth_status_str(char *buf, size_t buf_size)
{
	return snprintf(buf, buf_size,
		"=== Raw Ethernet Framework ===\n"
		"Socket fd : %d (%s)\n"
		"RX GOOSE  : %u\n"
		"RX RSTP   : %u\n"
		"RX HSR    : %u\n"
		"RX HSR/sup: %u\n"
		"RX total  : %u\n"
		"TX total  : %u",
		raw_sock, raw_sock >= 0 ? "open" : "not open",
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
	shell_print(sh, "Socket fd : %d (%s)", raw_sock,
		    raw_sock >= 0 ? "open" : "not open");
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

	/* Minimal test GOOSE payload (placeholder — not a valid PDU) */
	static const uint8_t test_goose[] = {
		0x61, 0x04,        /* GOOSE PDU tag + length */
		0x00, 0x00, 0x00, 0x00  /* placeholder data */
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

	/* Minimal RST BPDU (IEEE 802.1w Config BPDU, 35 bytes) */
	static const uint8_t test_bpdu[] = {
		0x00, 0x00,  /* Protocol ID = 0 (STP) */
		0x02,        /* Version = 2 (RSTP) */
		0x02,        /* Message type: RST BPDU */
		0x3C,        /* Flags */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Root BID */
		0x00, 0x00, 0x00, 0x00,  /* Root path cost */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Bridge BID */
		0x80, 0x01,  /* Port ID */
		0x00, 0x00,  /* Message age */
		0x14, 0x00,  /* Max age (20 s) */
		0x02, 0x00,  /* Hello time (2 s) */
		0x0F, 0x00,  /* Forward delay (15 s) */
		0x00,        /* Version 1 length */
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
