#ifndef _BR_ARP_H_
#define _BR_ARP_H_

#include <stdint.h>
#include <rte_ether.h>
#include "../br_conf.h"
#include "../br_tables.h"

void send_arp_request(struct br_conf *br_conf, uint16_t portid,
    uint32_t sender_ipv4_addr, struct rte_ether_addr *sender_hw_addr,
    uint32_t target_ipv4_addr);

void process_arp_packet(struct rte_mbuf *pkt, struct br_conf *br_conf, struct br_tables *br_tables);

#endif