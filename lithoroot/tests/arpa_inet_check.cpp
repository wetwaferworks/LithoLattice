#include <arpa/inet.h>
static_assert(sizeof(in_addr)==4);
extern "C" int lk_inet_check() {
  int bad=0;
  if (htons(0x1234)!=0x3412) ++bad;
  if (htonl(0x11223344u)!=0x44332211u) ++bad;
  if (ntohs(htons(0xBEEF))!=0xBEEF) ++bad;
  if (ntohl(htonl(0xDEADBEEFu))!=0xDEADBEEFu) ++bad;
  unsigned char b[4]; uint32_t n=htonl(0x7F000001u); __builtin_memcpy(b,&n,4);
  if (!(b[0]==127&&b[1]==0&&b[2]==0&&b[3]==1)) ++bad;
  return bad;
}
