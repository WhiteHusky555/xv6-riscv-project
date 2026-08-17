//
// nettest: exercises the virtio-net Ethernet driver (kernel/virtio_net.c)
// end to end, without any IP stack -- xv6 only has raw link-layer access
// via netsend()/netrecv()/netmac().
//
// 1. fetch our own MAC address (proves the device was found and
//    initialized).
// 2. fire off a burst of broadcast frames back-to-back (tx stress:
//    exercises descriptor allocation/free and the sleep/wakeup path
//    net_tx() takes when all NUM/2 tx chains are in flight).
// 3. send a real ARP request ("who has 10.0.2.2?") and wait for a
//    reply. qemu's "user" (SLIRP) networking answers ARP for its
//    virtual gateway unconditionally, with no DHCP/IP configuration
//    needed on our side, which makes it a convenient way to test the
//    full tx -> device -> host -> device -> rx -> interrupt path.
//
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/net.h"
#include "user/user.h"

#define FRAME_MIN 60 // classic Ethernet minimum frame length (no FCS)
#define ARP_LEN   (sizeof(struct eth_hdr) + sizeof(struct arp_hdr))

#define ARP_TIMEOUT_TRIES 100 // ~100 * 20 ticks -> generous, ARP replies are near-instant
#define ARP_POLL_TICKS    2

static const uint8 broadcast_mac[ETH_ADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8 gateway_ip[4] = {10, 0, 2, 2};  // qemu "user" netdev's virtual router
static const uint8 our_ip[4]     = {10, 0, 2, 15}; // any address in the guest's /24 will do

// virtio_net_hdr is stripped by the driver already; ARP fields are
// transmitted in network (big-endian) byte order, so 16-bit fields
// need an explicit swap on this little-endian machine. htons/ntohs
// are the same bit operation, hence one helper for both directions.
static uint16
bswap16(uint16 v)
{
  return (uint16)((v >> 8) | (v << 8));
}

static void
print_mac(const char *label, const uint8 *mac)
{
  printf("%s %x:%x:%x:%x:%x:%x\n", label,
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// send `count` broadcast frames back to back, without waiting for one
// to finish before starting the next -- net_tx() only returns once
// the device confirms the frame is sent, but user-level "back to
// back" here still means each call queues promptly behind the last,
// forcing descriptor reuse (NUM=8 tx descriptors, 2 per frame -> at
// most 4 frames in flight at a time in the driver, even though this
// loop asks for more).
static int
tx_stress(uint8 *my_mac, int count)
{
  char frame[FRAME_MIN];
  struct eth_hdr *eth = (struct eth_hdr *) frame;
  int i, ok;

  memset(frame, 0, sizeof(frame));
  memmove(eth->dst, broadcast_mac, ETH_ADDR_LEN);
  memmove(eth->src, my_mac, ETH_ADDR_LEN);
  eth->type = bswap16(0x88b5); // IEEE 802 "local experimental" ethertype: nobody need answer

  ok = 0;
  for(i = 0; i < count; i++){
    frame[sizeof(struct eth_hdr)] = (char) i; // vary the payload a little
    int n = netsend(frame, sizeof(frame));
    if(n == sizeof(frame)){
      ok++;
    } else {
      printf("nettest: tx_stress: netsend #%d failed (returned %d)\n", i, n);
    }
  }
  return ok;
}

// build and send a broadcast "who has 10.0.2.2?" ARP request.
static int
send_arp_request(uint8 *my_mac)
{
  char frame[FRAME_MIN];
  struct eth_hdr *eth = (struct eth_hdr *) frame;
  struct arp_hdr *arp = (struct arp_hdr *) (frame + sizeof(*eth));

  memset(frame, 0, sizeof(frame));

  memmove(eth->dst, broadcast_mac, ETH_ADDR_LEN);
  memmove(eth->src, my_mac, ETH_ADDR_LEN);
  eth->type = bswap16(ETH_TYPE_ARP);

  arp->htype = bswap16(ARP_HTYPE_ETHER);
  arp->ptype = bswap16(ETH_TYPE_IP);
  arp->hlen = ETH_ADDR_LEN;
  arp->plen = 4;
  arp->op = bswap16(ARP_OP_REQUEST);
  memmove(arp->sha, my_mac, ETH_ADDR_LEN);
  memmove(arp->spa, our_ip, 4);
  memset(arp->tha, 0, ETH_ADDR_LEN); // unknown -- that's what we're asking
  memmove(arp->tpa, gateway_ip, 4);

  int n = netsend(frame, sizeof(frame));
  if(n != sizeof(frame)){
    printf("nettest: netsend(ARP request) failed (returned %d)\n", n);
    return -1;
  }
  printf("nettest: sent ARP request for 10.0.2.2 (%d bytes)\n", n);
  return 0;
}

// poll netrecv() until we see an ARP reply that answers our request
// specifically (ignoring any other broadcast traffic that might show
// up on the segment), or until we give up.
static int
wait_arp_reply(uint8 *my_mac, uint8 *gw_mac)
{
  static char buf[NET_MAXFRAME]; // too big for the 1-page user stack; keep it static
  int tries;

  for(tries = 0; tries < ARP_TIMEOUT_TRIES; tries++){
    int n = netrecv(buf, sizeof(buf));

    if(n < 0){
      printf("nettest: netrecv error\n");
      return 0;
    }

    if(n >= (int) ARP_LEN){
      struct eth_hdr *eth = (struct eth_hdr *) buf;
      struct arp_hdr *arp = (struct arp_hdr *) (buf + sizeof(*eth));

      if(bswap16(eth->type) == ETH_TYPE_ARP &&
         bswap16(arp->op) == ARP_OP_REPLY &&
         memcmp(arp->spa, gateway_ip, 4) == 0 &&
         memcmp(arp->tha, my_mac, ETH_ADDR_LEN) == 0){
        memmove(gw_mac, arp->sha, ETH_ADDR_LEN);
        return 1;
      }
      // some other frame (not the reply we're after) -- ignore it.
    }

    pause(ARP_POLL_TICKS);
  }
  return 0;
}

int
main(void)
{
  uint8 mac[ETH_ADDR_LEN];
  uint8 gw_mac[ETH_ADDR_LEN];
  int fail = 0;

  if(netmac((char *) mac) < 0){
    printf("nettest: FAIL: no NIC found "
           "(check QEMUOPTS has -netdev/-device virtio-net-device)\n");
    exit(1);
  }
  print_mac("nettest: our mac", mac);

  int ok = tx_stress(mac, 16);
  printf("nettest: tx stress: %d/16 sends completed\n", ok);
  if(ok != 16)
    fail = 1;

  if(send_arp_request(mac) < 0){
    fail = 1;
  } else if(wait_arp_reply(mac, gw_mac)){
    print_mac("nettest: PASS -- gateway replied, mac", gw_mac);
  } else {
    printf("nettest: FAIL -- no ARP reply from 10.0.2.2 within timeout\n");
    fail = 1;
  }

  if(fail){
    printf("nettest: FAILED\n");
    exit(1);
  }
  printf("nettest: PASSED\n");
  exit(0);
}
