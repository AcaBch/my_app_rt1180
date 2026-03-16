/*
 * Custom Zephyr Application for MIMXRT1180-EVK
 * Demonstrates: GPIO, LED, Button, Logging, Shell, HTTPS Web Server
 */

#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/sys/reboot.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Include generated certificates */
#include "certs.h"

/* Raw Ethernet framework (GOOSE/RSTP/HSR/PRP) */
#include "raw_eth.h"

LOG_MODULE_REGISTER(my_app_rt1180, LOG_LEVEL_INF);

/* TLS credential tag */
#define TLS_TAG 1

/* LED - using the board's red LED (D7) */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* Button - using the board's user button (SW8) */
#define SW0_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);

/* Button callback data */
static struct gpio_callback button_cb_data;

/* Counter for button presses */
static volatile uint32_t button_press_count = 0;

/* Track LED state since gpio_pin_get_dt is unreliable on output pins */
static volatile int led_on = 0;

/* Network management callback */
static struct net_mgmt_event_callback mgmt_cb;
bool network_ready = false;
static char ip_addr_str[NET_IPV4_ADDR_LEN] = "192.168.0.132";

/* Button pressed callback */
void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	button_press_count++;
	LOG_INF("Button pressed! Count: %d", button_press_count);
	led_on = !led_on;
	gpio_pin_set_dt(&led, led_on);
}

/* ==================== HTTPS Server ==================== */

#define HTTPS_PORT 443
#define HTTP_PORT 80
#define HTTP_STACK_SIZE 8192
#define HTTP_PRIORITY 5

K_THREAD_STACK_DEFINE(https_stack, HTTP_STACK_SIZE);
struct k_thread https_thread_data;

K_THREAD_STACK_DEFINE(http_stack, 4096);
struct k_thread http_thread_data;

/* HTML page template */
static const char html_template[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/html\r\n"
	"Connection: close\r\n\r\n"
	"<!DOCTYPE html>"
	"<html><head>"
	"<title>MIMXRT1180-EVK Status (HTTPS)</title>"
	"<meta http-equiv=\"refresh\" content=\"5\">"
	"<style>"
	"body{font-family:Arial,sans-serif;margin:40px;background:#1a1a2e;color:#eee;}"
	"h1{color:#0f4c75;}"
	".card{background:#16213e;padding:20px;margin:10px 0;border-radius:8px;}"
	".status{font-size:24px;}"
	".led-on{color:#00ff00;}"
	".led-off{color:#ff4444;}"
	".secure{color:#00ff00;font-size:12px;}"
	"button{padding:10px 20px;margin:5px;font-size:16px;cursor:pointer;border:none;border-radius:4px;}"
	".btn-on{background:#00aa00;color:white;}"
	".btn-off{background:#aa0000;color:white;}"
	".btn-toggle{background:#0066cc;color:white;}"
	"</style></head><body>"
	"<h1>MIMXRT1180-EVK Web Interface</h1>"
	"<p class=\"secure\">Secure Connection (HTTPS/TLS)</p>"
	"<div class=\"card\">"
	"<h2>System Status</h2>"
	"<p class=\"status\">LED: <span class=\"%s\">%s</span></p>"
	"<p class=\"status\">Button Presses: %d</p>"
	"<p class=\"status\">Uptime: %lld seconds</p>"
	"</div>"
	"<div class=\"card\">"
	"<h2>LED Control</h2>"
	"<a href=\"/led/on\"><button class=\"btn-on\">LED ON</button></a>"
	"<a href=\"/led/off\"><button class=\"btn-off\">LED OFF</button></a>"
	"<a href=\"/led/toggle\"><button class=\"btn-toggle\">TOGGLE</button></a>"
	"</div>"
	"<div class=\"card\">"
	"<h2>Web Terminal</h2>"
	"<a href=\"/terminal\"><button class=\"btn-toggle\">Open Terminal</button></a>"
	"</div>"
	"<div class=\"card\">"
	"<h2>API Endpoints</h2>"
	"<ul>"
	"<li>GET /api/status - JSON status</li>"
	"<li>GET /led/on - Turn LED on</li>"
	"<li>GET /led/off - Turn LED off</li>"
	"<li>GET /led/toggle - Toggle LED</li>"
	"<li>GET /terminal - Web Terminal</li>"
	"</ul></div></body></html>";

static const char json_template[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: application/json\r\n"
	"Connection: close\r\n\r\n"
	"{\"led\":\"%s\",\"button_presses\":%d,\"uptime_ms\":%lld,\"board\":\"%s\",\"secure\":true}";

static const char redirect_response[] =
	"HTTP/1.1 302 Found\r\n"
	"Location: /\r\n"
	"Connection: close\r\n\r\n";

/* Terminal/Shell web page */
static const char terminal_html[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/html\r\n"
	"Connection: close\r\n\r\n"
	"<!DOCTYPE html><html><head>"
	"<title>Web Terminal - MIMXRT1180-EVK</title>"
	"<style>"
	"body{font-family:monospace;margin:0;padding:20px;background:#0d1117;color:#c9d1d9;}"
	"h1{color:#58a6ff;margin-bottom:10px;}"
	".terminal{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:15px;max-width:800px;}"
	"#output{background:#0d1117;border:1px solid #30363d;border-radius:4px;padding:10px;"
	"height:400px;overflow-y:auto;white-space:pre-wrap;font-size:14px;margin-bottom:10px;}"
	".input-line{display:flex;align-items:center;}"
	".prompt{color:#7ee787;margin-right:8px;}"
	"#cmd{flex:1;background:#21262d;border:1px solid #30363d;border-radius:4px;"
	"padding:8px;color:#c9d1d9;font-family:monospace;font-size:14px;}"
	"#cmd:focus{outline:none;border-color:#58a6ff;}"
	"button{background:#238636;color:white;border:none;padding:8px 16px;border-radius:4px;"
	"margin-left:8px;cursor:pointer;font-size:14px;}"
	"button:hover{background:#2ea043;}"
	".help{margin-top:15px;padding:10px;background:#161b22;border-radius:4px;font-size:12px;}"
	".help b{color:#58a6ff;}"
	"a{color:#58a6ff;}"
	"</style></head><body>"
	"<h1>Web Terminal</h1>"
	"<p><a href=\"/\">Back to Dashboard</a></p>"
	"<div class=\"terminal\">"
	"<div id=\"output\">Welcome to MIMXRT1180-EVK Web Terminal\\n"
	"Type 'help' for available commands\\n\\n</div>"
	"<div class=\"input-line\">"
	"<span class=\"prompt\">my_app_rt1180:~$</span>"
	"<input type=\"text\" id=\"cmd\" placeholder=\"Enter command...\" autofocus>"
	"<button onclick=\"sendCmd()\">Send</button>"
	"</div></div>"
	"<div class=\"help\">"
	"<b>Available commands:</b> help, status, led on, led off, led toggle, "
	"uptime, version, threads, net status, reboot, "
	"raw_eth status, raw_eth send goose &lt;mac&gt;, raw_eth send rstp"
	"</div>"
	"<script>"
	"const output=document.getElementById('output');"
	"const cmdInput=document.getElementById('cmd');"
	"cmdInput.addEventListener('keypress',function(e){if(e.key==='Enter')sendCmd();});"
	"function sendCmd(){"
	"const cmd=cmdInput.value.trim();"
	"if(!cmd)return;"
	"output.textContent+='> '+cmd+'\\n';"
	"cmdInput.value='';"
	"fetch('/exec?cmd='+encodeURIComponent(cmd))"
	".then(r=>r.text())"
	".then(t=>{output.textContent+=t+'\\n';output.scrollTop=output.scrollHeight;})"
	".catch(e=>{output.textContent+='Error: '+e+'\\n';});"
	"}"
	"</script></body></html>";

static char response_buf[4096];
static char cmd_output[2048];

/* URL decode helper */
static void url_decode(char *dst, const char *src, size_t dst_size)
{
	size_t i = 0;
	while (*src && i < dst_size - 1) {
		if (*src == '%' && src[1] && src[2]) {
			char hex[3] = {src[1], src[2], 0};
			dst[i++] = (char)strtol(hex, NULL, 16);
			src += 3;
		} else if (*src == '+') {
			dst[i++] = ' ';
			src++;
		} else {
			dst[i++] = *src++;
		}
	}
	dst[i] = '\0';
}

/* Execute web command and return output */
static int execute_web_cmd(const char *cmd, char *output, size_t output_size)
{
	int len = 0;

	if (strcmp(cmd, "help") == 0) {
		len = snprintf(output, output_size,
			"Available commands:\n"
			"  help                          - Show this help\n"
			"  status                        - Show system status\n"
			"  led on                        - Turn LED on\n"
			"  led off                       - Turn LED off\n"
			"  led toggle                    - Toggle LED\n"
			"  uptime                        - Show uptime\n"
			"  version                       - Show version info\n"
			"  threads                       - List kernel threads\n"
			"  net status                    - Show network status\n"
			"  reboot                        - Reboot the system\n"
			"  raw_eth status                - Raw Ethernet RX/TX counters\n"
			"  raw_eth send goose <mac>      - Send test GOOSE frame\n"
			"  raw_eth send rstp             - Send test RSTP BPDU");
	} else if (strcmp(cmd, "status") == 0) {
		len = snprintf(output, output_size,
			"==== System Status ====\n"
			"Board: %s\n"
			"LED: %s\n"
			"Button presses: %d\n"
			"Uptime: %lld ms\n"
			"Network: %s\n"
			"IP: %s",
			CONFIG_BOARD,
			led_on ? "ON" : "OFF",
			button_press_count,
			k_uptime_get(),
			network_ready ? "Ready" : "Not ready",
			ip_addr_str);
	} else if (strcmp(cmd, "led on") == 0) {
		led_on = 1;
		gpio_pin_set_dt(&led, 1);
		len = snprintf(output, output_size, "LED turned ON");
	} else if (strcmp(cmd, "led off") == 0) {
		led_on = 0;
		gpio_pin_set_dt(&led, 0);
		len = snprintf(output, output_size, "LED turned OFF");
	} else if (strcmp(cmd, "led toggle") == 0) {
		led_on = !led_on;
		gpio_pin_set_dt(&led, led_on);
		len = snprintf(output, output_size, "LED toggled (now %s)",
			       led_on ? "ON" : "OFF");
	} else if (strcmp(cmd, "uptime") == 0) {
		int64_t uptime_ms = k_uptime_get();
		int secs = (int)(uptime_ms / 1000);
		int mins = secs / 60;
		int hours = mins / 60;
		len = snprintf(output, output_size,
			"Uptime: %d:%02d:%02d (%lld ms)",
			hours, mins % 60, secs % 60, uptime_ms);
	} else if (strcmp(cmd, "version") == 0) {
		len = snprintf(output, output_size,
			"Zephyr RTOS v%d.%d.%d\n"
			"Board: %s\n"
			"App: My Custom Zephyr App for RT1180",
			KERNEL_VERSION_MAJOR, KERNEL_VERSION_MINOR, KERNEL_PATCHLEVEL,
			CONFIG_BOARD);
	} else if (strcmp(cmd, "threads") == 0) {
		len = snprintf(output, output_size,
			"Kernel Threads:\n"
			"  main - Main thread\n"
			"  idle - Idle thread\n"
			"  https_server - HTTPS server\n"
			"  http_redirect - HTTP redirect\n"
			"  shell_uart - Shell backend\n"
			"  logging - Logging thread\n"
			"(Use serial shell for detailed thread info)");
	} else if (strcmp(cmd, "net status") == 0 || strcmp(cmd, "net") == 0) {
		len = snprintf(output, output_size,
			"Network Status:\n"
			"  State: %s\n"
			"  IP Address: %s\n"
			"  Netmask: 255.255.255.0\n"
			"  Gateway: 192.168.0.1\n"
			"  HTTPS: Port 443\n"
			"  HTTP: Port 80 (redirect)",
			network_ready ? "Connected" : "Disconnected",
			ip_addr_str);
	} else if (strcmp(cmd, "reboot") == 0) {
		len = snprintf(output, output_size, "Rebooting in 2 seconds...");
	} else if (strcmp(cmd, "raw_eth status") == 0) {
		len = raw_eth_status_str(output, output_size);
	} else if (strncmp(cmd, "raw_eth send goose ", 19) == 0) {
		const char *mac_str = cmd + 19;
		uint8_t dst[6];
		unsigned int b[6];
		if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
			   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
			for (int i = 0; i < 6; i++) {
				dst[i] = (uint8_t)b[i];
			}
			int ret = raw_eth_test_send_goose(dst);
			if (ret > 0) {
				len = snprintf(output, output_size,
					"GOOSE test frame sent to %s (%d bytes)",
					mac_str, ret);
			} else {
				len = snprintf(output, output_size,
					"goose_send failed: %d", ret);
			}
		} else {
			len = snprintf(output, output_size,
				"Usage: raw_eth send goose <xx:xx:xx:xx:xx:xx>");
		}
	} else if (strcmp(cmd, "raw_eth send rstp") == 0) {
		int ret = raw_eth_test_send_rstp();
		len = snprintf(output, output_size,
			ret > 0 ? "RSTP BPDU test frame sent to 01:80:C2:00:00:00 (%d bytes)"
				: "rstp_send_bpdu failed: %d",
			ret);
	} else if (cmd[0] == '\0') {
		len = 0;
	} else {
		len = snprintf(output, output_size,
			"Unknown command: '%s'\nType 'help' for available commands", cmd);
	}

	return len;
}

static void handle_client(int client_fd)
{
	char recv_buf[512];
	int len;

	len = zsock_recv(client_fd, recv_buf, sizeof(recv_buf) - 1, 0);
	if (len <= 0) {
		return;
	}
	recv_buf[len] = '\0';

	int led_state = led_on;

	if (strstr(recv_buf, "GET /terminal") != NULL) {
		zsock_send(client_fd, terminal_html, sizeof(terminal_html) - 1, 0);
	} else if (strstr(recv_buf, "GET /exec?cmd=") != NULL) {
		char *cmd_start = strstr(recv_buf, "GET /exec?cmd=") + 14;
		char *cmd_end = strstr(cmd_start, " ");
		if (cmd_end) {
			*cmd_end = '\0';
		}
		char decoded_cmd[128];
		url_decode(decoded_cmd, cmd_start, sizeof(decoded_cmd));

		execute_web_cmd(decoded_cmd, cmd_output, sizeof(cmd_output));

		len = snprintf(response_buf, sizeof(response_buf),
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/plain\r\n"
			"Access-Control-Allow-Origin: *\r\n"
			"Connection: close\r\n\r\n%s", cmd_output);
		zsock_send(client_fd, response_buf, len, 0);

		if (strcmp(decoded_cmd, "reboot") == 0) {
			k_msleep(2000);
			sys_reboot(SYS_REBOOT_COLD);
		}
	} else if (strstr(recv_buf, "GET /api/status") != NULL) {
		len = snprintf(response_buf, sizeof(response_buf), json_template,
			       led_state ? "on" : "off",
			       button_press_count,
			       k_uptime_get(),
			       CONFIG_BOARD);
		zsock_send(client_fd, response_buf, len, 0);
	} else if (strstr(recv_buf, "GET /led/on") != NULL) {
		gpio_pin_set_dt(&led, 1);
		LOG_INF("LED turned ON via HTTPS");
		zsock_send(client_fd, redirect_response, sizeof(redirect_response) - 1, 0);
	} else if (strstr(recv_buf, "GET /led/off") != NULL) {
		gpio_pin_set_dt(&led, 0);
		LOG_INF("LED turned OFF via HTTPS");
		zsock_send(client_fd, redirect_response, sizeof(redirect_response) - 1, 0);
	} else if (strstr(recv_buf, "GET /led/toggle") != NULL) {
		gpio_pin_toggle_dt(&led);
		LOG_INF("LED toggled via HTTPS");
		zsock_send(client_fd, redirect_response, sizeof(redirect_response) - 1, 0);
	} else {
		led_state = led_on;
		len = snprintf(response_buf, sizeof(response_buf), html_template,
			       led_state ? "led-on" : "led-off",
			       led_state ? "ON" : "OFF",
			       button_press_count,
			       k_uptime_get() / 1000);
		zsock_send(client_fd, response_buf, len, 0);
	}
}

static void https_server_thread(void *p1, void *p2, void *p3)
{
	int server_fd, client_fd;
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	int ret;

	LOG_INF("HTTPS server thread starting...");

	k_msleep(3000);
	while (!network_ready) {
		k_msleep(500);
		struct net_if *iface = net_if_get_default();
		if (iface && net_if_is_up(iface)) {
			network_ready = true;
		}
	}

ret = tls_credential_add(TLS_TAG, TLS_CREDENTIAL_SERVER_CERTIFICATE,
				 server_cert_der, sizeof(server_cert_der));
	if (ret < 0) {
		LOG_ERR("Failed to add server cert: %d", ret);
		return;
	}

	ret = tls_credential_add(TLS_TAG, TLS_CREDENTIAL_PRIVATE_KEY,
				 server_key_der, sizeof(server_key_der));
	if (ret < 0) {
		LOG_ERR("Failed to add private key: %d", ret);
		return;
	}

	LOG_INF("TLS credentials registered");

	server_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (server_fd < 0) {
		LOG_ERR("Failed to create TLS socket: %d", errno);
		return;
	}

	int role = TLS_DTLS_ROLE_SERVER;
	ret = zsock_setsockopt(server_fd, SOL_TLS, TLS_DTLS_ROLE,
			       &role, sizeof(role));
	if (ret < 0) {
		LOG_ERR("Failed to set TLS_ROLE: %d", errno);
		zsock_close(server_fd);
		return;
	}

	int verify = TLS_PEER_VERIFY_NONE;
	ret = zsock_setsockopt(server_fd, SOL_TLS, TLS_PEER_VERIFY,
			       &verify, sizeof(verify));
	if (ret < 0) {
		LOG_WRN("Failed to set TLS_PEER_VERIFY: %d", errno);
	}

	sec_tag_t sec_tag_list[] = { TLS_TAG };
	ret = zsock_setsockopt(server_fd, SOL_TLS, TLS_SEC_TAG_LIST,
			       sec_tag_list, sizeof(sec_tag_list));
	if (ret < 0) {
		LOG_ERR("Failed to set TLS_SEC_TAG_LIST: %d", errno);
		zsock_close(server_fd);
		return;
	}

	int opt = 1;
	zsock_setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(HTTPS_PORT);

	if (zsock_bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("Failed to bind HTTPS: %d", errno);
		zsock_close(server_fd);
		return;
	}

	if (zsock_listen(server_fd, 3) < 0) {
		LOG_ERR("Failed to listen HTTPS: %d", errno);
		zsock_close(server_fd);
		return;
	}

	LOG_INF("====================================");
	LOG_INF("HTTPS server listening on port %d", HTTPS_PORT);
	LOG_INF("Open https://%s/ in your browser", ip_addr_str);
	LOG_INF("(Accept the self-signed certificate warning)");
	LOG_INF("====================================");

	while (1) {
		client_fd = zsock_accept(server_fd, (struct sockaddr *)&addr, &addr_len);
		if (client_fd < 0) {
			LOG_ERR("HTTPS Accept failed: %d", errno);
			k_msleep(100);
			continue;
		}

		LOG_INF("HTTPS client connected");
		handle_client(client_fd);
		zsock_close(client_fd);
	}
}

static void http_redirect_thread(void *p1, void *p2, void *p3)
{
	int server_fd, client_fd;
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);

	k_msleep(4000);

	server_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server_fd < 0) {
		LOG_ERR("Failed to create HTTP socket: %d", errno);
		return;
	}

	int opt = 1;
	zsock_setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(HTTP_PORT);

	if (zsock_bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("Failed to bind HTTP: %d", errno);
		zsock_close(server_fd);
		return;
	}

	if (zsock_listen(server_fd, 3) < 0) {
		LOG_ERR("Failed to listen HTTP: %d", errno);
		zsock_close(server_fd);
		return;
	}

	LOG_INF("HTTP redirect server on port %d -> HTTPS", HTTP_PORT);

	static char redirect_https[256];
	snprintf(redirect_https, sizeof(redirect_https),
		 "HTTP/1.1 301 Moved Permanently\r\n"
		 "Location: https://%s/\r\n"
		 "Connection: close\r\n\r\n", ip_addr_str);

	while (1) {
		client_fd = zsock_accept(server_fd, (struct sockaddr *)&addr, &addr_len);
		if (client_fd < 0) {
			k_msleep(100);
			continue;
		}

		char buf[128];
		zsock_recv(client_fd, buf, sizeof(buf), 0);
		zsock_send(client_fd, redirect_https, strlen(redirect_https), 0);
		zsock_close(client_fd);
	}
}

/* ==================== Shell Commands ==================== */

static int cmd_led_on(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	gpio_pin_set_dt(&led, 1);
	shell_print(sh, "LED is ON");
	return 0;
}

static int cmd_led_off(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	gpio_pin_set_dt(&led, 0);
	shell_print(sh, "LED is OFF");
	return 0;
}

static int cmd_led_toggle(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	gpio_pin_toggle_dt(&led);
	shell_print(sh, "LED toggled");
	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "==== Application Status ====");
	shell_print(sh, "Board: %s", CONFIG_BOARD);
	shell_print(sh, "Button presses: %d", button_press_count);
	shell_print(sh, "LED state: %s", led_on ? "ON" : "OFF");
	shell_print(sh, "Uptime: %lld ms", k_uptime_get());
	shell_print(sh, "Network: %s", network_ready ? "Ready" : "Not ready");
	if (network_ready) {
		shell_print(sh, "IP Address: %s", ip_addr_str);
		shell_print(sh, "HTTPS: https://%s/", ip_addr_str);
		shell_print(sh, "HTTP:  http://%s/ (redirects to HTTPS)", ip_addr_str);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(led_cmds,
	SHELL_CMD(on, NULL, "Turn LED on", cmd_led_on),
	SHELL_CMD(off, NULL, "Turn LED off", cmd_led_off),
	SHELL_CMD(toggle, NULL, "Toggle LED", cmd_led_toggle),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(led, &led_cmds, "LED control commands", NULL);
SHELL_CMD_REGISTER(status, NULL, "Show application status", cmd_status);

/* ==================== Network Event Handler ==================== */

static void net_mgmt_handler(struct net_mgmt_event_callback *cb,
			     uint64_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		struct net_if_config *cfg = net_if_get_config(iface);

		if (cfg && cfg->ip.ipv4) {
			struct in_addr *addr = &cfg->ip.ipv4->unicast[0].ipv4.address.in_addr;
			net_addr_ntop(AF_INET, addr, ip_addr_str, sizeof(ip_addr_str));
			LOG_INF("Network ready! IP: %s", ip_addr_str);
			network_ready = true;
		}
	}
}

/* ==================== Main ==================== */

int main(void)
{
	int ret;

	printk("\n=============================================\n");
	printk("  My Custom Zephyr App - MIMXRT1180-EVK\n");
	printk("  Board: %s\n", CONFIG_BOARD);
	printk("  Features: GPIO, Shell, HTTPS Server, RAW Eth\n");
	printk("==============================================\n\n");

	/* Initialize LED */
	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED device not ready");
		return -1;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure LED: %d", ret);
		return -1;
	}
	LOG_INF("LED initialized");

	/* Initialize Button */
	if (!gpio_is_ready_dt(&button)) {
		LOG_ERR("Button device not ready");
		return -1;
	}

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure button: %d", ret);
		return -1;
	}

	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure button interrupt: %d", ret);
		return -1;
	}

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);
	LOG_INF("Button initialized with interrupt");

	/* Setup network event callback */
	net_mgmt_init_event_callback(&mgmt_cb, net_mgmt_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&mgmt_cb);

	LOG_INF("Starting network...");
	LOG_INF("Static IP: %s", ip_addr_str);

	/* Check if network is already configured (static IP case) */
	struct net_if *iface = net_if_get_default();
	if (iface) {
		struct net_if_config *cfg = net_if_get_config(iface);
		if (cfg && cfg->ip.ipv4 && cfg->ip.ipv4->unicast[0].ipv4.is_used) {
			struct in_addr *addr = &cfg->ip.ipv4->unicast[0].ipv4.address.in_addr;
			net_addr_ntop(AF_INET, addr, ip_addr_str, sizeof(ip_addr_str));
			LOG_INF("Network ready! IP: %s", ip_addr_str);
			network_ready = true;
		}
	}

	/* Start HTTPS server thread */
	k_thread_create(&https_thread_data, https_stack,
			K_THREAD_STACK_SIZEOF(https_stack),
			https_server_thread, NULL, NULL, NULL,
			HTTP_PRIORITY, 0, K_NO_WAIT);

	/* Start HTTP redirect server */
	k_thread_create(&http_thread_data, http_stack,
			K_THREAD_STACK_SIZEOF(http_stack),
			http_redirect_thread, NULL, NULL, NULL,
			HTTP_PRIORITY, 0, K_NO_WAIT);

	/* Start raw Ethernet framework (GOOSE/RSTP/HSR/PRP) */
	raw_eth_start();

	/* Main loop */
	while (1) {
		k_msleep(10000);
	}

	return 0;
}
