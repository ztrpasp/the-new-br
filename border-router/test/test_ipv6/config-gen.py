#!/usr/bin/python3

from color_packet import *
import os
import sqlite3

db_file = "br.sqlite"
if os.path.exists(db_file):
    os.remove(db_file)

num_odc_entry_in_db = 0

def save_odct(odc_entries):
    num_inserted = 0
    conn = sqlite3.connect(db_file)
    conn.execute("CREATE TABLE IF NOT EXISTS odct("
        "pnid TEXT NOT NULL,"
        "lsid TEXT NOT NULL,"
        "cnid TEXT NOT NULL,"
        "pids TEXT NOT NULL,"
        "rn TEXT NOT NULL,"
        "PRIMARY KEY(pnid, lsid, cnid, pids));")
    for odc_entry in odc_entries:
        try:
            conn.execute("INSERT INTO odct values(?,?,?,?,?);", odc_entry)
            num_inserted += 1
        except sqlite3.IntegrityError as e:
            print(e)
    conn.commit()
    conn.close()
    return num_inserted
    
def save_nid(nid_entries):
    num_inserted = 0
    conn = sqlite3.connect(db_file)
    conn.execute("CREATE TABLE IF NOT EXISTS nid("
        "nid TEXT NOT NULL,"
        "ip_addr TEXT NOT NULL,"
        "sk TEXT NOT NULL,"
        "PRIMARY KEY(nid));"
    )
    for nid_entry in nid_entries:
        try:
            # print(nid_entries)
            conn.execute("INSERT INTO nid values(?,?,?);", nid_entry)
            num_inserted += 1
        except sqlite3.IntegrityError as e:
            print(e)
    conn.commit()
    conn.close()
    return num_inserted

random.seed(12345678)

AS = {
    id: AutonomousSystem(id=id, rm=ResourceManager(ip_address=(10, 11, 1, 1))) for id in range(1, 103)
}

NUM_NID_ENTRY = 100
NUM_ODC_ENTRY = 100
NUM_SID_PER_PROXY = 1
NUM_SID_IN_TARGET_AS = NUM_NID_ENTRY * NUM_SID_PER_PROXY

# Proxy in AS1
ip_address = IPAddress(1, 0, 0, 9)
for _ in range(NUM_NID_ENTRY):
    Proxy(of=AS[1], ip_address=ip_address, num_sid=NUM_SID_PER_PROXY)
    ip_address = ip_address + 1

# Proxy in AS2-102: one proxy in each AS
for id in range(2, 103):
    Proxy(of=AS[id], num_sid=10)

OUTSIDE_PORTID = 0
INSIDE_PORTID = 1
TBR_OUTSIDE_PORTID = 4
TBR_INSIDE_PORTID = 5

# BR in AS1
br_1_1 = BorderRouter(of=AS[1])
for id in range(0, 4):
    br_1_1.add(Port(id=id, ip_address=(10, 100, id + 1, 1), mask_length=24))
br_1_1.add(Port(id=TBR_OUTSIDE_PORTID, ip_address=(192, 168, 0, 2), mask_length=24)) # outside port
br_1_1.add(Port(id=TBR_INSIDE_PORTID, ip_address=(10, 0, 0, 1), mask_length=24))
for i in range(2, 101):
    br_name = "br_1_" + str(i)
    locals()[br_name] = BorderRouter(of=AS[1])
    locals()[br_name].add(Port(id=OUTSIDE_PORTID))
    locals()[br_name].add(Port(id=INSIDE_PORTID, ip_address=(10, 0, 0, i), mask_length=24))
for i in range(1, 12):
    br_1_1.add_iproute(IPRoute(dest=(10, i, 1, 0), mask_length=16, next_hop=(10, 0, 0, 101)))
br_1_1.add_iproute(IPRoute(dest=(1, 0, 0, 0), mask_length=8, next_hop=(10, 0, 0, 101)))

# BR in AS2-AS100
for id in range(2, 101):
    br_name = "br_%d_0" % id
    locals()[br_name] = BorderRouter(of=AS[id])
    locals()[br_name].add(Port(id=OUTSIDE_PORTID))

# BR in AS101
br_101_0 = BorderRouter(of=AS[101])
br_101_0.add(Port(id=OUTSIDE_PORTID)) # outside
br_101_0.add(Port(id=INSIDE_PORTID))
br_101_1 = BorderRouter(of=AS[101])
br_101_1.add(Port(id=OUTSIDE_PORTID, ip_address=(192, 168, 0, 1))) # outside
br_101_1.add(Port(id=INSIDE_PORTID))

# BR in AS102
br_102_0 = BorderRouter(of=AS[102])
br_102_0.add(Port(id=OUTSIDE_PORTID))

# Path AS101-AS102
px_101_102 = PX.random()
InterdomainPath(from_br=br_101_0, from_port_id=OUTSIDE_PORTID, to_br=br_102_0, to_port_id=OUTSIDE_PORTID, to_as=AS[102], px=px_101_102)
InterdomainPath(from_br=br_102_0, from_port_id=OUTSIDE_PORTID, to_br=br_101_0, to_port_id=OUTSIDE_PORTID, to_as=AS[101], px=px_101_102)

# Path AS101-1
px_1_101 = PX.random()
InterdomainPath(from_br=br_1_1, from_port_id=TBR_OUTSIDE_PORTID, to_br=br_101_1, to_port_id=OUTSIDE_PORTID, to_as=AS[101], px=px_1_101)
InterdomainPath(from_br=br_101_1, from_port_id=OUTSIDE_PORTID, to_br=br_1_1, to_port_id=TBR_OUTSIDE_PORTID, to_as=AS[1], px=px_1_101)

# Path AS1-AS2, AS1-AS3, ..., AS1-AS100
for id in range(2, 101):
    br_in_as1 = locals()["br_1_%d" % id]
    br_in_asx = locals()["br_%d_0" % id]
    px = PX.random()
    InterdomainPath(from_br=br_in_as1, from_port_id=OUTSIDE_PORTID, to_br=br_in_asx, to_port_id=OUTSIDE_PORTID, to_as=AS[id], px=px)
    InterdomainPath(from_br=br_in_asx, from_port_id=OUTSIDE_PORTID, to_br=br_in_as1, to_port_id=OUTSIDE_PORTID, to_as=AS[1], px=px)

# Indirect path
IIPGen.gen_iip_for_br_pair(br1=br_101_0, br1_port_id=INSIDE_PORTID, br2=br_101_1, br2_port_id=INSIDE_PORTID)
IIPGen.gen_iip_for_target_br(target_br=br_1_1, target_br_port_id=TBR_INSIDE_PORTID, peer_br_port_id=INSIDE_PORTID)

# save target border router's configuration file
consumer_nid_list = AS[102].nid_list + AS[101].nid_list
NUM_CONSUMER = len(consumer_nid_list)

with open("config-br", "w") as f:
    f.write(br_1_1.cfg_str())

num_nid_entries_in_db = save_nid(br_1_1.nid_entries())

if not os.path.exists("./pcap"):
    os.mkdir("./pcap")

# inbound GET packet
path = [AS[102], br_102_0, br_101_0, AS[101], br_101_1, br_1_1, AS[1]]
pkts = TrafficGen.gen_get_traffic(target_br=br_1_1, target_port_id=TBR_OUTSIDE_PORTID, path=path, num_pkts=20)
wrpcap("./pcap/igp2rm.pcap", pkts)

# outbound GET packet
pkts = []
for id in range(2, 101):
    path = [AS[id], locals()["br_%d_0" % id], locals()["br_1_%d" % id], AS[1], br_1_1, br_101_1, AS[101]]
    pkts += TrafficGen.gen_get_traffic(target_br=br_1_1, target_port_id=TBR_INSIDE_PORTID, path=path, num_pkts=20)
wrpcap("./pcap/ogp2br.pcap", pkts)

# outbound data packet from in-domain proxy
odc_dict = dict()
path = [AS[1], br_1_1, br_101_1, AS[101], br_101_0, br_102_0, AS[102]]
pkts, odc_entries = TrafficGen.gen_data_traffic(target_br=br_1_1, target_port_id=TBR_INSIDE_PORTID, 
    path=path, odc_dict=odc_dict, num_pkts=20, pld_size=1000, back_pid=True)
wrpcap("./pcap/odpfhi.pcap", pkts)
num_odc_entry_in_db += save_odct(odc_entries)

# outbound data packet from in-domain proxy
path = [AS[1], br_1_1, br_101_1, AS[101], br_101_0, br_102_0, AS[102]]
pkts, odc_entries = TrafficGen.gen_data_traffic(target_br=br_1_1, target_port_id=TBR_INSIDE_PORTID, 
    path=path, odc_dict=odc_dict, num_pkts=20, pld_size=1000, back_pid=False)
wrpcap("./pcap/odpfhi_nrpid.pcap", pkts)
num_odc_entry_in_db += save_odct(odc_entries)

# outbound data packet from other-domain proxy
pkts = []
for id in range(2, 101):
    path = [AS[id], locals()["br_%d_0" % id], locals()["br_1_%d" % id], AS[1], br_1_1, br_101_1, 
        AS[101], br_101_0, br_102_0, AS[102]]
    _pkts, _ = TrafficGen.gen_data_traffic(target_br=br_1_1, target_port_id=TBR_INSIDE_PORTID, 
        path=path, odc_dict=None, num_pkts=20, pld_size=1000, back_pid=True)
    pkts.extend(_pkts)
wrpcap("./pcap/odpfho.pcap", pkts)

# outbound data packet from other-domain proxy, no reserved PID
pkts = []
for id in range(2, 101):
    path = [AS[id], locals()["br_%d_0" % id], locals()["br_1_%d" % id], AS[1], br_1_1, br_101_1, AS[101], br_101_0, br_102_0, AS[102]]
    _pkts, _ = TrafficGen.gen_data_traffic(target_br=br_1_1, target_port_id=TBR_INSIDE_PORTID, 
        path=path, odc_dict=None, num_pkts=20, pld_size=1000, back_pid=False)
    pkts.extend(_pkts)
wrpcap("./pcap/odpfho_nrpid.pcap", pkts)

# inbound DATA packet, to other-domain
pkts = []
for id in range(2, 101):
    path = [AS[102], br_102_0, br_101_0, AS[101], br_101_1, br_1_1, AS[1], locals()["br_1_%d" % id], locals()["br_%d_0" % id], AS[id]]
    _pkts, _ = TrafficGen.gen_data_traffic(target_br=br_1_1, target_port_id=TBR_OUTSIDE_PORTID, 
        path=path, odc_dict=None, num_pkts=20, pld_size=1000, back_pid=False)
    pkts.extend(_pkts)
wrpcap("./pcap/idp2br.pcap", pkts)

# inbound DATA packet, to in-domain proxies
path = [AS[102], br_102_0, br_101_0, AS[101], br_101_1, br_1_1, AS[1]]
pkts, _ = TrafficGen.gen_data_traffic(target_br=br_1_1, target_port_id=TBR_OUTSIDE_PORTID, 
    path=path, odc_dict=None, num_pkts=20, pld_size=1000, back_pid=False)
wrpcap("./pcap/idp2h.pcap", pkts)

# populate ODC entries
print("Number of valid ODC entries: %d, number of random ODC entries: %d" % (num_odc_entry_in_db, NUM_ODC_ENTRY - num_odc_entry_in_db))
odc_entries = []
while len(odc_entries) < NUM_ODC_ENTRY - num_odc_entry_in_db:
    pnid = str(NID.random())
    lsid = str(LSID.random())
    cnid = str(NID.random())
    pids_len = random.randint(1,10)
    pids = ""
    for i in range(pids_len):
        pids += "%08X" % random.getrandbits(32)
    rn = "%08X" % random.getrandbits(32)
    odc_entries.append((pnid, lsid, cnid, pids, rn))

num_odc_entry_in_db += save_odct(odc_entries)

config_summary = "Number of NID table entries: %d\n" % num_nid_entries_in_db
config_summary += "Number of SID in target AS: %d\n" % NUM_SID_IN_TARGET_AS
config_summary += "Number of ODC table entries: %d\n" % num_odc_entry_in_db
print(config_summary)