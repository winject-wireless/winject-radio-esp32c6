#ifndef WINJECT_UART_CONSOLE_H_
#define WINJECT_UART_CONSOLE_H_

// Service commands on the C6's UART0 (P2 header on the Waveshare kit), one per
// line:
//   dl    reboot into the ROM serial downloader (reflash without the IO9 strap)
// The m-plane itself only runs over SDIO.
void uart_console_start();

#endif  // WINJECT_UART_CONSOLE_H_
