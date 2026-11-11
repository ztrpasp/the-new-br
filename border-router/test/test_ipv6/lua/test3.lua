package.path = package.path ..";?.lua;/home/zeusnet/pktgen-dpdk/?.lua"

require "Pktgen"

local M = {}

function M.setup()
    local br_port0_mac_addr = "f8:f2:1e:b1:f6:50"
    local br_port1_mac_addr = "f8:f2:1e:b1:f6:51"
    local br_port2_mac_addr = "f8:f2:1e:b1:f6:52"
    local br_port3_mac_addr = "f8:f2:1e:b1:f6:53"
    local br_port4_mac_addr = "3c:fd:fe:9e:a8:ba"
    local br_port5_mac_addr = "3c:fd:fe:9e:a8:b8"

    local pktgen_port0_mac_addr = "f8:f2:1e:b1:f0:d0"
    local pktgen_port1_mac_addr = "f8:f2:1e:b1:f0:d1"
    local pktgen_port2_mac_addr = "f8:f2:1e:b1:f0:d2"
    local pktgen_port3_mac_addr = "f8:f2:1e:b1:f0:d3"
    local pktgen_port4_mac_addr = "f8:f2:1e:b1:f0:d4"
    local pktgen_port5_mac_addr = "f8:f2:1e:b1:f0:d5"

    pktgen.set_mac("0", "dst", br_port0_mac_addr)
    pktgen.set_mac("1", "dst", br_port1_mac_addr)
    pktgen.set_mac("2", "dst", br_port2_mac_addr)
    pktgen.set_mac("3", "dst", br_port3_mac_addr)
    pktgen.set_mac("4", "dst", br_port4_mac_addr)
    pktgen.set_mac("5", "dst", br_port5_mac_addr)

    pktgen.set_mac("0", "src", pktgen_port0_mac_addr)
    pktgen.set_mac("1", "src", pktgen_port1_mac_addr)
    pktgen.set_mac("2", "src", pktgen_port2_mac_addr)
    pktgen.set_mac("3", "src", pktgen_port3_mac_addr)
    pktgen.set_mac("4", "src", pktgen_port4_mac_addr)
    pktgen.set_mac("5", "src", pktgen_port5_mac_addr)
    pktgen.garp(0, "enable")
    pktgen.garp(1, "enable")
    pktgen.garp(2, "enable")
    pktgen.garp(3, "enable")
    pktgen.garp(4, "enable")
    pktgen.garp(5, "enable")
end

local function average(list)
    local r = 0
    local cnt = 0
    for k, v in pairs(list) do
        r = r + v
        cnt = cnt + 1
    end
    return r / cnt
end

local function numberic_list_to_string(list, listname, divisor)
    local dash_len = (6 * #list + 1 - string.len(listname)) // 2
    local list_header = "+" .. string.rep("-", dash_len) .. listname .. string.rep("-", dash_len) .. "+"
    local s = list_header .. "\n| "
    for k, v in pairs(list) do
        s = s .. string.format("%.3f ", v / divisor)
    end
    s = s .. "|\n" .. "+" .. string.rep("-", string.len(list_header) - 2) .. "+\n"
    return s
end

function M.test(send_port, rcv_port, test_name, time)

    if (time == nil) then
        time = 5
    end
 
    local pktgen_init_time = 1
    local br_init_time = 4
    print("Executing ".. test_name .. ".lua\n")

    local prompt = string.format("wait for pktgen initialization ...")
    printf("\r" .. string.rep(" ", 50) .. "\r" .. prompt)
    pktgen.delay(1000 * pktgen_init_time)
    
    pktgen.start(send_port)

    for i = time + br_init_time, time, -1 do
        local prompt = string.format("wait %d seconds ...", i)
        printf("\r" .. string.rep(" ", 50) .. "\r" .. prompt)
        pktgen.delay(1000)
    end

    -- lists for rate
    local rx_pps_list = {}
    local rx_bps_list = {}
    local tx_pps_list = {}
    local tx_bps_list = {}

    -- number of sended/received packets/bytes at time t0
    local port_stats_port = pktgen.portStats("all", "port")
    local rx_pkts_t0 = port_stats_port[rcv_port]["ipackets"]
    local rx_bytes_t0 = port_stats_port[rcv_port]["ibytes"]
    local tx_pkts_t0 = port_stats_port[send_port]["opackets"]
    local tx_bytes_t0 = port_stats_port[send_port]["obytes"]

    -- Take a sample of stats every 1 second 
    for i = 1, time, 1 do
        local port_stats_rate = pktgen.portStats("all", "rate")
        local rx_pps = port_stats_rate[rcv_port]["pkts_rx"]
        local rx_bps = port_stats_rate[rcv_port]["ibytes"] * 8
        local tx_pps = port_stats_rate[send_port]["pkts_tx"]
        local tx_bps = port_stats_rate[send_port]["obytes"] * 8
        table.insert(rx_pps_list, rx_pps)
        table.insert(rx_bps_list, rx_bps)
        table.insert(tx_pps_list, tx_pps)
        table.insert(tx_bps_list, tx_bps)
        local prompt = string.format("wait %d seconds ...", (time - i))
        printf("\r" .. string.rep(" ", 30) .. "\r" .. prompt)
        pktgen.delay(1000)
    end

    -- number of sended/received packets/bytes at time t1 = t0 + time 
    port_stats_port = pktgen.portStats("all", "port")
    local rx_pkts_t1 = port_stats_port[rcv_port]["ipackets"]
    local rx_bytes_t1 = port_stats_port[rcv_port]["ibytes"]
    local tx_pkts_t1 = port_stats_port[send_port]["opackets"]
    local tx_bytes_t1 = port_stats_port[send_port]["obytes"]

    -- average tx/rx rate/bytes
    local rx_pps = (rx_pkts_t1 - rx_pkts_t0) / time
    local rx_bps = (rx_bytes_t1 - rx_bytes_t0) * 8 / time
    local tx_pps = (tx_pkts_t1 - tx_pkts_t0) / time
    local tx_bps = (tx_bytes_t1 - tx_bytes_t0) * 8 / time

    local file = io.open("./result3.txt", "a")
    local test_id = test_name .. " " .. os.date("%Y-%m-%d %H:%M:%S")
    local out_str = "begin " .. test_id .. "\n"
    out_str = out_str .. string.format("tx pps = %.4f Mpps\n", tx_pps / 1e6)
    out_str = out_str .. string.format("tx bps = %.4f Gbps\n", tx_bps / 1e9)
    out_str = out_str .. string.format("rx pps = %.4f Mpps\n", rx_pps / 1e6)
    out_str = out_str .. string.format("rx bps = %.4f Gbps\n", rx_bps / 1e9)
    out_str = out_str .. numberic_list_to_string(tx_pps_list, "tx_pps_list", 1e6)
    out_str = out_str .. numberic_list_to_string(tx_bps_list, "tx_bps_list", 1e9)
    out_str = out_str .. numberic_list_to_string(rx_pps_list, "rx_pps_list", 1e6)
    out_str = out_str .. numberic_list_to_string(rx_bps_list, "rx_bps_list", 1e9)
    out_str = out_str .. "end " .. test_id .. "\n\n"
    file:write(out_str)
    file:close()
    print("\n" .. out_str)
    pktgen.stop("all")
    pktgen.pause("Done executing ".. test_name .. ".lua\n", 2000)
    pktgen.quit()
end

-- inbound GET packet -> RM of AS1
-- GET: AS102 -> AS101 -> (port 4)BR(port 5) -> RM of AS1

-- port 4(192.168.0.1) -> BR port 4(192.168.0.2) ->
-- BR port 5(10.0.0.1) -> port 5(RM: 10.0.0.102)
function M.igp2rm()
    local send_port = 4
    local rcv_port = 5
    M.setup()
    pktgen.set_ipaddr(send_port, "src", "192.168.0.1/32")
    pktgen.set_ipaddr(send_port, "dst", "192.168.0.2")
    pktgen.set_ipaddr(rcv_port, "src", "10.0.0.102/32")
    pktgen.set_ipaddr(rcv_port, "dst", "10.0.0.1")
    M.test(send_port, rcv_port, "igp2rm")
end


-- outbound GET packet -> ebr (external peer)
-- GET: AS2-100 -> (port 5)BR(port 4) -> ebr

-- port 5(RM: 10.0.0.102) -> BR port 5(10.0.0.1) ->
-- BR port 4(192.168.0.2) -> port 4(192.168.0.1)
function M.ogp2br()
    local send_port = 5
    local rcv_port = 4
    M.setup()
    pktgen.set_ipaddr(send_port, "src", "10.0.0.102/32")
    pktgen.set_ipaddr(send_port, "dst", "10.0.0.1")
    pktgen.set_ipaddr(rcv_port, "src", "192.168.0.1/32")
    pktgen.set_ipaddr(rcv_port, "dst", "192.168.0.2")
    M.test(send_port, rcv_port, "ogp2br")
end

-- inbound DATA packet, to other-domain
-- GET: AS2-100 -> (port 5)BR(port 4) -> AS101 -> AS102
-- DATA: AS102 -> AS101 -> (port 4)BR(port 5) -> AS2-100

-- port 4(192.168.0.1) -> BR port 4(192.168.0.2) ->
-- BR port 5(10.0.0.1) -> port 5(10.0.0.2-100)
function M.idp2br()
    local send_port = 4
    local rcv_port = 5
    M.setup()
    pktgen.set_ipaddr(send_port, "src", "192.168.0.1/32")
    pktgen.set_ipaddr(send_port, "dst", "192.168.0.2")
    pktgen.set_ipaddr(rcv_port, "src", "10.0.0.0/24")
    pktgen.set_ipaddr(rcv_port, "dst", "10.0.0.1")
    M.test(send_port, rcv_port, "idp2br")
end

-- inbound DATA packet, to in-domain proxies
-- GET: Proxies of AS1 -> irouter -> (port 5)BR(port 4) -> AS101 -> AS102
-- DATA: AS102 -> AS101 -> (port 4)BR(port 5) -> irouter

-- port 4(192.168.0.1) -> BR port 4(192.168.0.2) ->
-- BR port 5(10.0.0.1) -> port 5(10.0.0.101)
function M.idp2h()
    local send_port = 4
    local rcv_port = 5
    M.setup()
    pktgen.set_ipaddr(send_port, "src", "192.168.0.1/32")
    pktgen.set_ipaddr(send_port, "dst", "192.168.0.2")
    pktgen.set_ipaddr(rcv_port, "src", "10.0.0.101/32")
    pktgen.set_ipaddr(rcv_port, "dst", "10.0.0.1")
    M.test(send_port, rcv_port, "idp2p")
end

-- outbound data packet from in-domain proxy
-- GET: AS102 -> AS101 -> (port 4)BR(port 5) -> Proxies in AS1
-- DATA: Proxies in AS1 -> irouter -> (port 5)BR(port 4) -> ebr

-- port 5(irouter_ip_addr AS1) -> BR port 5(10.0.0.1) ->
-- BR port 4(192.168.0.2) -> port 4(ebr: 192.168.0.1)
function M.odpfhi()
    local send_port = 5
    local rcv_port = 4
    M.setup()
    pktgen.set_ipaddr(send_port, "src", "10.0.0.101/32")
    pktgen.set_ipaddr(send_port, "dst", "10.0.0.1")
    pktgen.set_ipaddr(rcv_port, "src", "192.168.0.1/32")
    pktgen.set_ipaddr(rcv_port, "dst", "192.168.0.2")
    M.test(send_port, rcv_port, "odpfhi")
end

-- outbound data packet from in-domain proxy, no reserved PID
-- GET: AS102 -> AS101 -> (port 4)BR(port 5) -> Proxies in AS1
-- DATA: Proxies in AS1 -> irouter -> (port 5)BR(port 4) -> ebr

-- port 5(irouter_ip_addr AS1) -> BR port 5(10.0.0.1) ->
-- BR port 4(192.168.0.2) -> port 4(ebr: 192.168.0.1)

-- Note: Type command "odc disable" in BR CLI before testing.
function M.odpfhi_nrpid()
    local send_port = 5
    local rcv_port = 4
    M.setup()
    pktgen.set_ipaddr(send_port, "src", "10.0.0.6/32")
    pktgen.set_ipaddr(send_port, "dst", "10.0.0.5")
    pktgen.set_ipaddr(rcv_port, "src", "192.168.0.1/32")
    pktgen.set_ipaddr(rcv_port, "dst", "192.168.0.2")
    M.test(send_port, rcv_port, "odpfhi_nrpid")
end

-- outbound data packet from other-domain proxy
-- GET: AS102 -> AS101 -> (port 4)BR(port 5) -> AS2-100
-- DATA: AS2-100 -> (port 5)BR(port 4) -> ebr

-- port 5(10.0.0.2-100) -> BR port 5(10.0.0.1) ->
-- BR port 4(192.168.0.2) -> port 4(192.168.0.1)
function M.odpfho()
    local send_port = 5
    local rcv_port = 4
    M.setup()
    pktgen.range.src_ip(send_port, "start", "10.0.0.2")
    pktgen.range.src_ip(send_port, "inc", "0.0.0.1")
    pktgen.range.src_ip(send_port, "min", "10.0.0.2")
    pktgen.range.src_ip(send_port, "max", "10.0.0.100")
    pktgen.set_ipaddr(send_port, "dst", "10.0.0.1")
    pktgen.set_ipaddr(rcv_port, "src", "192.168.0.1/32")
    pktgen.set_ipaddr(rcv_port, "dst", "192.168.0.2")
    M.test(send_port, rcv_port, "odpfho")
end

-- outbound data packet from other-domain proxy, no reserved PID
-- GET: AS102 -> AS101 -> (port 4)BR(port 5) -> AS2-100
-- DATA: AS2-100 -> (port 5)BR(port 4) -> ebr

-- port 5(10.0.0.2-100) -> BR port 5(10.0.0.1) ->
-- BR port 4(192.168.0.2) -> port 4(192.168.0.1)

-- Note: Type command "odc disable" in BR CLI before testing.
function M.odpfho_nrpid()
    local send_port = 5
    local rcv_port = 4
    M.setup()
    pktgen.range.src_ip(send_port, "start", "10.0.0.2")
    pktgen.range.src_ip(send_port, "inc", "0.0.0.1")
    pktgen.range.src_ip(send_port, "min", "10.0.0.2")
    pktgen.range.src_ip(send_port, "max", "10.0.0.100")
    pktgen.set_ipaddr(send_port, "dst", "10.0.0.1")
    pktgen.set_ipaddr(rcv_port, "src", "192.168.0.1/32")
    pktgen.set_ipaddr(rcv_port, "dst", "192.168.0.2")
    M.test(send_port, rcv_port, "odpfho_nrpid")
end

return M
