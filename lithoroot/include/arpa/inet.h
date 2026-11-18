/* LithoRoot <arpa/inet.h>: byte-order conversions (wasm32 is little-endian, network
 * order is big-endian) and the IPv4 address types. inet_pton/inet_ntop are declared
 * for the sockets stage; the web build has no BSD sockets yet (netinet/in.h,
 * sys/socket.h and poll.h belong to that stage), so they are not defined here. */
#ifndef LITHOROOT_ARPA_INET_H
#define LITHOROOT_ARPA_INET_H
#include <stdint.h>
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "LithoRoot arpa/inet.h assumes a little-endian target (wasm32)"
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;
#ifndef LITHOROOT_SOCKLEN_T
#define LITHOROOT_SOCKLEN_T
typedef unsigned int socklen_t;
#endif
#ifndef LITHOROOT_IN_ADDR
#define LITHOROOT_IN_ADDR
struct in_addr { in_addr_t s_addr; };
#endif
#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46
static inline uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint16_t htons(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t ntohl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint16_t ntohs(uint16_t x) { return __builtin_bswap16(x); }
int inet_pton(int af, const char* src, void* dst);
const char* inet_ntop(int af, const void* src, char* dst, socklen_t size);
#ifdef __cplusplus
}
#endif
#endif
