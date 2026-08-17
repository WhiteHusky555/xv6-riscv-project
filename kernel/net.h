// Ethernet frame layout, shared between the kernel driver
// (virtio_net.c) and user-space test/diagnostic programs.
//
// This is link-layer only: xv6 has no IP stack. A "frame" as seen by
// net_tx()/net_rx() (and the netsend/netrecv syscalls) is exactly
// what goes on the wire, starting at the Ethernet header -- the
// driver prepends/strips the virtio_net_hdr itself.

#define ETH_ADDR_LEN 6

// standard untagged Ethernet MTU (1500) + 14-byte header, plus a
// little slack for an optional 802.1Q tag. also used to size the
// driver's DMA and software receive buffers.
#define NET_MAXFRAME 1522

#define ETH_TYPE_ARP 0x0806
#define ETH_TYPE_IP  0x0800

struct eth_hdr {
  uint8  dst[ETH_ADDR_LEN];
  uint8  src[ETH_ADDR_LEN];
  uint16 type;   // network byte order (big-endian)
} __attribute__((packed));

// minimal ARP packet (RFC 826) for IPv4 over Ethernet, used by
// user/nettest.c to sanity-check the driver end to end.
#define ARP_HTYPE_ETHER 1
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

struct arp_hdr {
  uint16 htype;         // hardware type: ARP_HTYPE_ETHER
  uint16 ptype;         // protocol type: ETH_TYPE_IP
  uint8  hlen;           // hardware address length: 6
  uint8  plen;           // protocol address length: 4
  uint16 op;             // ARP_OP_REQUEST / ARP_OP_REPLY
  uint8  sha[ETH_ADDR_LEN]; // sender hardware address
  uint8  spa[4];            // sender protocol address
  uint8  tha[ETH_ADDR_LEN]; // target hardware address
  uint8  tpa[4];            // target protocol address
} __attribute__((packed));
