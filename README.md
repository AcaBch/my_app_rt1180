# my_app_rt1180

Custom Zephyr RTOS application for the **NXP MIMXRT1180-EVK** board (CM33 core).

## Features

- **HTTPS Web Server** on port 443 with ECDSA P-256 TLS
- **HTTP redirect** on port 80 → HTTPS
- **Web Interface** — LED control dashboard with auto-refresh
- **Web Terminal** — browser-based shell for sending commands
- **GPIO** — LED (D7) control and button (SW8) interrupt
- **Zephyr Shell** — interactive shell over UART
- **REST API** — JSON status endpoint at `/api/status`
- **Raw Ethernet framework** — AF_PACKET Layer 2 RX/TX for GOOSE, RSTP, HSR, PRP

## Hardware

| Item | Value |
|------|-------|
| Board | MIMXRT1180-EVK |
| Core | CM33 |
| LED | D7 (red, `led0`) |
| Button | SW8 (`sw0`) |
| Ethernet port | PHY (3) / ENETC0 |
| DIP SW5 | OFF OFF ON OFF (4,3,2,1) |

## Network

| Setting | Value |
|---------|-------|
| IP Address | 192.168.0.132 |
| Netmask | 255.255.255.0 |
| Gateway | 192.168.0.1 |
| HTTPS | https://192.168.0.132/ |
| HTTP | http://192.168.0.132/ (redirects to HTTPS) |

> **Note:** Accept the self-signed certificate warning in your browser.

## Build & Flash

Requires [Zephyr SDK](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) and `west`.

```bash
# Build
west build my_app_rt1180 --board mimxrt1180_evk/mimxrt1189/cm33 --build-dir my_app_rt1180/build

# Flash
west flash --build-dir my_app_rt1180/build
```

## Serial Shell

Connect to `/dev/ttyACM0` at **115200 baud**:

```bash
minicom -D /dev/ttyACM0 -b 115200 -o
```

### Shell Commands

| Command | Description |
|---------|-------------|
| `led on` | Turn LED on |
| `led off` | Turn LED off |
| `led toggle` | Toggle LED |
| `status` | Show system status (IP, LED, uptime, network) |
| `raw_eth status` | Show raw Ethernet socket state and RX/TX counters |
| `raw_eth send goose <mac>` | Send a test GOOSE frame (EtherType 0x88B8) to `<mac>` |
| `raw_eth send rstp` | Send a test RSTP BPDU to `01:80:C2:00:00:00` |

## Web Terminal Commands

Available at `https://192.168.0.132/terminal`:

| Command | Description |
|---------|-------------|
| `help` | Show available commands |
| `status` | System status (LED, uptime, network, button presses) |
| `led on/off/toggle` | LED control |
| `uptime` | System uptime |
| `version` | Zephyr and board version |
| `threads` | List kernel threads |
| `net status` | Network status |
| `reboot` | Reboot the board |

## API Endpoints

| Endpoint | Description |
|----------|-------------|
| `GET /` | Web dashboard |
| `GET /api/status` | JSON status |
| `GET /led/on` | Turn LED on |
| `GET /led/off` | Turn LED off |
| `GET /led/toggle` | Toggle LED |
| `GET /terminal` | Web terminal |
| `GET /exec?cmd=<cmd>` | Execute web terminal command |

## Raw Ethernet Framework (GOOSE / RSTP / HSR / PRP)

Implemented in `src/raw_eth.c` / `src/raw_eth.h`. An `AF_PACKET SOCK_RAW` socket
captures all Layer 2 frames in a dedicated RX thread. Incoming frames are
dispatched by EtherType to protocol-specific stub handlers ready for real logic.

| Protocol | EtherType | Destination MAC |
|----------|-----------|-----------------|
| GOOSE (IEC 61850-8-1) | `0x88B8` | `01:0C:CD:04:00:xx` |
| RSTP BPDU (IEEE 802.1w) | LLC (length < 0x0600) | `01:80:C2:00:00:00` |
| HSR tag (IEC 62439-3) | `0x892F` | — |
| HSR/PRP supervision | `0x88FB` | `01:15:4E:00:01:00` |

### Kconfig options added

```
CONFIG_NET_SOCKETS_PACKET=y               # AF_PACKET socket support
CONFIG_NET_ETHERNET_FORWARD_UNRECOGNISED_ETHERTYPE=y  # pass custom EtherTypes up
CONFIG_NET_PROMISCUOUS_MODE=y             # receive all L2 multicasts
```

### Shell commands

| Command | Description |
|---------|-------------|
| `raw_eth status` | Socket state and per-protocol RX/TX counters |
| `raw_eth send goose <mac>` | Send a test GOOSE frame to `<mac>` |
| `raw_eth send rstp` | Send a test RSTP BPDU to `01:80:C2:00:00:00` |

### Notes

- The RX thread starts after `network_ready` and runs at priority 7.
- Stub handlers (`goose_rx`, `rstp_rx`, `hsr_rx`, `hsr_prp_supervision_rx`) log
  the incoming frame and are ready for protocol-specific payload parsing.
- Requires a **direct physical Ethernet connection** or a switch that forwards
  the relevant EtherTypes/multicast groups. VM NAT mode will not pass raw L2 frames.

---

## TLS Certificate

Self-signed ECDSA P-256 certificate, valid 1 year.
To regenerate:

```bash
openssl ecparam -name prime256v1 -genkey -noout -out server_ec.key.pem
openssl req -new -x509 -key server_ec.key.pem -out server_ec.crt.pem \
  -days 365 -nodes -subj "/C=US/O=MyApp/CN=192.168.0.132" \
  -addext "subjectAltName=IP:192.168.0.132"
openssl x509 -in server_ec.crt.pem -outform DER -out server.crt.der
openssl pkcs8 -topk8 -inform PEM -outform DER -in server_ec.key.pem -out server.key.der -nocrypt
xxd -i server.crt.der | sed 's/server_crt_der/server_cert_der/g' > src/certs.h
xxd -i server.key.der >> src/certs.h
```
