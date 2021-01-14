/* SPDX-License-Identifier: MIT */

#ifndef UART_H
#define UART_H

void uart_init(void);

void uart_putbyte(u8 c);
u8 uart_getbyte(void);

void uart_putchar(u8 c);
u8 uart_getchar(void);

void uart_write(const void *buf, size_t count);
size_t uart_read(void *buf, size_t count);

void uart_puts(const char *s);

void uart_setbaud(int baudrate);

void uart_flush(void);

#endif
