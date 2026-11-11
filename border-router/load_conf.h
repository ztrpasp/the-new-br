#ifndef __BR_LOAD_CONF_H__
#define __BR_LOAD_CONF_H__

#include "br_tables.h"

void load_odct(struct br_conf *br_conf, struct br_tables *br_tables);
void load_nid(struct br_conf *br_conf, struct br_tables *br_tables);
void load_rsa_keypair(struct br_conf *br_conf);
void load_config(struct br_conf *br_conf, struct br_tables *br_tables);

#endif