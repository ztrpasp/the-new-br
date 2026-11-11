#! /usr/bin/env python3

import subprocess
import copy

def split_arg_list(arg_list):
    result = []
    for arg in arg_list:
        result.extend(arg.split())
    return result

def run_test():
    arg_list = [
        "sudo -E",
        "/usr/local/bin/pktgen",
        "-l 0-12", # cores
        "-n 4", # nrank
        "--proc-type auto",
        "--log-level 1",
        "--file-prefix pg",
        "-a 03:00.0", # allowlist
        "-a 03:00.1",
        "-a 03:00.2",
        "-a 03:00.3",
        "-a 0c:00.0",
        "-a 0c:00.1",
        "--",
        "-T", # color terminal output
        "-P", # promiscuous mode
        "-j", # Enable jumbo frames of 9600 bytes
        "-m [1-3].[0-1]",
        "-m [4-6].[2-3]",
        "-m [7-9].4",
        "-m [10-12].5"
    ]

    tests = [
        ('igp2rm', 4),
        ('ogp2br', 5),
        ('idp2br', 4),
        ('idp2h', 4),
        ('odpfhi', 5),
        ('odpfhi_nrpid', 5),
        ('odpfho', 5),
        ('odpfho_nrpid', 5),
    ]

    for test_name, send_port in tests:
        # set pcap and lua files
        arg_list_tmp = copy.deepcopy(arg_list)
        arg_list_tmp.append("-s %d:./pcap/%s.pcap" % (send_port, test_name))
        arg_list_tmp.append("-f ./lua/%s.lua" % test_name)
        print(" ".join(arg_list_tmp))
        args = split_arg_list(arg_list_tmp)
        subprocess.call(args)
        subprocess.call(['stty', 'sane'])

def main():
    run_test()

if __name__ == "__main__":
    main()
