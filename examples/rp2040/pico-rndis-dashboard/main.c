// Copyright (c) 2022 Cesanta Software Limited
// All rights reserved

#include "hardware/uart.h"
#include "mongoose.h"
#include "net.h"
#include "pico/stdlib.h"
#include "tusb.h"

#define ERRORTIMEOUT "2;:"

static struct mg_tcpip_if *s_ifp;

const uint8_t tud_network_mac_address[6] = {2, 2, 0x84, 0x6A, 0x96, 0};

static void blink_cb(void *arg) {  // Blink periodically
  gpio_put(PICO_DEFAULT_LED_PIN, !gpio_get_out_level(PICO_DEFAULT_LED_PIN));
  (void) arg;
}

static void cb(struct mg_connection *con, int ev, void *ev_data,
               void *fn_data) {
  bool valid_response_received = false;

  if (ev == MG_EV_READ) {
    struct mg_iobuf *r = &con->recv;
    // transmit over hardware uart
    for (size_t i = 0; i < r->len; i++) {
      uart_putc(uart1, r->buf[i]);
    }

    // make local variables
    unsigned char buff[100] = {0};
    size_t buffindex = 0;

    // send error response if no response was received after 50ms from main
    // controller
    if (uart_is_readable_within_us(uart1, 100000)) {
      while (!(uart_getc(uart1) == '$')) {
        ;
        ;
      }
    } else {
      mg_send(con, ERRORTIMEOUT, 11);
      for (int i = 0; i < 100; i++) {
        buff[i] = 0;
      }
      return;
    }

    // Get characters until ':' is encountered
    while (!valid_response_received && buffindex < sizeof(buff) - 1) {
      char c = uart_getc(uart1);
      if (c == ':') {
        valid_response_received = true;  // Set the flag when : is encountered
        buff[buffindex++] = c;
      } else {
        buff[buffindex++] = c;
      }
    }

    r->len = 0;

    buff[buffindex] = '\0';  // End string
    MG_INFO(("received: %s", buff));
    mg_send(con, buff, buffindex);
  }
}

bool tud_network_recv_cb(const uint8_t *buf, uint16_t len) {
  mg_tcpip_qwrite((void *) buf, len, s_ifp);
  // MG_INFO(("RECV %hu", len));
  // mg_hexdump(buf, len);
  tud_network_recv_renew();
  return true;
}

void tud_network_init_cb(void) {
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
  // MG_INFO(("SEND %hu", arg));
  memcpy(dst, ref, arg);
  return arg;
}

static size_t usb_tx(const void *buf, size_t len, struct mg_tcpip_if *ifp) {
  if (!tud_ready()) return 0;
  while (!tud_network_can_xmit(len)) tud_task();
  tud_network_xmit((void *) buf, len);
  (void) ifp;
  return len;
}

static bool usb_up(struct mg_tcpip_if *ifp) {
  (void) ifp;
  return tud_inited() && tud_ready() && tud_connected();
}

static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_dta) {
  if (ev == MG_EV_HTTP_MSG) return mg_http_reply(c, 200, "", "ok\n");
}

int main(void) {
  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
  stdio_init_all();

  struct mg_mgr mgr;  // Initialise Mongoose event manager
  mg_mgr_init(&mgr);  // and attach it to the interface

  mg_timer_add(&mgr, 500, MG_TIMER_REPEAT, blink_cb, &mgr);

  struct mg_tcpip_driver driver = {.tx = usb_tx, .up = usb_up};
  struct mg_tcpip_if mif = {.mac = {2, 0, 1, 2, 3, 0x77},
                            .ip = mg_htonl(MG_U32(192, 168, 20, 206)),
                            .mask = mg_htonl(MG_U32(255, 255, 255, 0)),
                            .enable_dhcp_server = true,
                            .driver = &driver,
                            .recv_queue.size = 4096};
  s_ifp = &mif;
  mg_tcpip_init(&mgr, &mif);
  tusb_init();

  MG_INFO(("Initialising application..."));
  // remove dashboard for now
  // web_init(&mgr);

  uart_init(uart1, 100000);
  gpio_set_function(4, GPIO_FUNC_UART);
  gpio_set_function(5, GPIO_FUNC_UART);
  uart_set_hw_flow(uart1, false, false);
  uart_set_format(uart1, 8, 1, UART_PARITY_NONE);
  uart_set_fifo_enabled(uart1, true);

  MG_INFO(("Starting event loop"));
  mg_listen(&mgr, "tcp://192.168.20.206:8888", cb, NULL);  // Setup listener
  for (;;) {
    mg_mgr_poll(&mgr, 1);
    tud_task();
  }

  return 0;
}
