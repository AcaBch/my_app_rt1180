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
| `status` | Show system status |

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
