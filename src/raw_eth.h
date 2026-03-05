/*
 * raw_eth.h — Raw Ethernet Layer 2 framework for industrial protocols
 *
 * Supports:
 *   GOOSE     (IEC 61850-8-1)         EtherType 0x88B8
 *   HSR tag   (IEC 62439-3)           EtherType 0x892F
 *   HSR/PRP supervision               EtherType 0x88FB
 *   RSTP BPDU (IEEE 802.1w)           LLC to 01:80:C2:00:00:00
 */

#ifndef RAW_ETH_H
#define RAW_ETH_H

#include <stdint.h>
#include <stddef.h>

/* EtherType constants */
#define ETH_P_GOOSE    0x88B8U  /* IEC 61850-8-1 GOOSE */
#define ETH_P_HSR      0x892FU  /* IEC 62439-3 HSR tag */
#define ETH_P_HSR_SUP  0x88FBU  /* HSR/PRP supervision */
/* RSTP uses LLC on dst 01:80:C2:00:00:00 — ethertype field is frame length */

/* Well-known multicast destination MACs */
extern const uint8_t ETH_ADDR_STP[6];      /* 01:80:C2:00:00:00  RSTP/STP */
extern const uint8_t ETH_ADDR_HSR_SUP[6];  /* 01:15:4E:00:01:00  HSR supervision */

/* Ethernet frame header (matches struct net_eth_hdr layout) */
struct raw_eth_hdr {
	uint8_t  dst[6];
	uint8_t  src[6];
	uint16_t ethertype;  /* network byte order */
} __packed;

/* Public API */
void raw_eth_start(void);

int raw_eth_send(const uint8_t *dst_mac, uint16_t ethertype,
		 const uint8_t *payload, size_t payload_len);

/* Protocol-specific TX helpers */
int goose_send(const uint8_t *dst, const uint8_t *goose_pdu, size_t pdu_len);
int rstp_send_bpdu(const uint8_t *bpdu, size_t len);

#endif /* RAW_ETH_H */
