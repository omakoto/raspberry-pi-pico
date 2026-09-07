/*
 * lwIP Configuration for nsbackend-pico.
 * Configured for FreeRTOS, BSD Sockets, and lwIP mDNS responder.
 */

#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// OS and Socket support
#define NO_SYS                          0
#define LWIP_SOCKET                     1
#define LWIP_NETCONN                    1
#define LWIP_NETIF_API                  1

// Thread Priorities and Sizes
#define TCPIP_THREAD_PRIO               (configMAX_PRIORITIES - 2)
#define TCPIP_THREAD_STACKSIZE          2048
#define DEFAULT_THREAD_STACKSIZE        1024
#define LWIP_FREERTOS_THREAD_STACKSIZE_IS_STACKWORDS 1

// Mailbox Queue Sizes for OS sockets and netconn
#define DEFAULT_RAW_RECVMBOX_SIZE       8
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_TCP_RECVMBOX_SIZE       8
#define DEFAULT_ACCEPTMBOX_SIZE         8
#define TCPIP_MBOX_SIZE                 32
#define LWIP_TIMEVAL_PRIVATE            0
// Ensure lwIP uses newlib's reentrant per-thread errno (*__errno()) so socket errors
// (such as EWOULDBLOCK / EAGAIN) are consistently visible across lwIP and C++ application code.
#ifdef LWIP_PROVIDE_ERRNO
#undef LWIP_PROVIDE_ERRNO
#endif
#define LWIP_ERRNO_STDINCLUDE           1

// Socket Options
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define SO_REUSE                        1
#define LWIP_TCP_KEEPALIVE              1

// Memory Configuration
#define MEM_ALIGNMENT                   4

#if defined(PICO_RP2040) && PICO_RP2040
// RP2040 has 264KB total SRAM (256KB main RAM). Keep lwIP static memory pools
// compact so they fit comfortably alongside the 160KB FreeRTOS heap and task stacks.
#define MEM_SIZE                        (8 * 1024)
#define MEMP_NUM_PBUF                   16
#define MEMP_NUM_RAW_PCB                2
#define MEMP_NUM_UDP_PCB                4
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_PCB_LISTEN         2
#define MEMP_NUM_TCP_SEG                16
#define MEMP_NUM_SYS_TIMEOUT            16
#define MEMP_NUM_NETBUF                 4
#define MEMP_NUM_NETCONN                4
#define MEMP_NUM_TCPIP_MSG_API          8
#define MEMP_NUM_TCPIP_MSG_INPKT        16

#define PBUF_POOL_SIZE                  12
#define PBUF_POOL_BUFSIZE               1536

// Protocols
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     1
#define LWIP_IGMP                       1
#define LWIP_DNS                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1

#define TCP_MSS                         1460
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#else
// RP2350 has 512KB SRAM; allocate larger pools for maximum network buffer headroom.
#define MEM_SIZE                        (16 * 1024)
#define MEMP_NUM_PBUF                   32
#define MEMP_NUM_RAW_PCB                4
#define MEMP_NUM_UDP_PCB                8
#define MEMP_NUM_TCP_PCB                16
#define MEMP_NUM_TCP_PCB_LISTEN         8
#define MEMP_NUM_TCP_SEG                32
#define MEMP_NUM_SYS_TIMEOUT            16
#define MEMP_NUM_NETBUF                 8
#define MEMP_NUM_NETCONN                8
#define MEMP_NUM_TCPIP_MSG_API          16
#define MEMP_NUM_TCPIP_MSG_INPKT        32

#define PBUF_POOL_SIZE                  32
#define PBUF_POOL_BUFSIZE               1536

// Protocols
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     1
#define LWIP_IGMP                       1
#define LWIP_DNS                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1

#define TCP_MSS                         1460
#define TCP_WND                         (8 * TCP_MSS)
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#endif

#define LWIP_CHECKSUM_CTRL_PER_NETIF    0

// mDNS Responder Configuration
#define LWIP_MDNS_RESPONDER             1
#define LWIP_NUM_NETIF_CLIENT_DATA      (LWIP_MDNS_RESPONDER)
#define MDNS_MAX_SERVICES               2

#define SYS_LIGHTWEIGHT_PROT            1

#endif /* _LWIPOPTS_H */
