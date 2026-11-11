import re

with open("result3-nidt-odcd-idhvd.txt", "r") as f:
    lines = f.readlines()

s = ""
for line in lines:
    s += line
s = s.replace("\n", " ")

size_list = [10, 100, 1000, 10000, 100000, 1000000, 10000000]

p0 = r"NUM_NID_ENTRY = # BEGIN"
p1 = (r".*?begin igp2rm.*?rx pps = (\d+\.\d+) Mpps rx bps = (\d+\.\d+) Gbps.*?end igp2rm" +
    r".*?begin ogp2br.*?rx pps = (\d+\.\d+) Mpps rx bps = (\d+\.\d+) Gbps.*?end ogp2br" +
    r".*?begin idp2br.*?rx pps = (\d+\.\d+) Mpps rx bps = (\d+\.\d+) Gbps.*?end idp2br" +
    r".*?begin idp2h.*?rx pps = (\d+\.\d+) Mpps rx bps = (\d+\.\d+) Gbps.*?end idp2h" +
    r".*?begin odpfhi.*?rx pps = (\d+\.\d+) Mpps rx bps = (\d+\.\d+) Gbps.*?end odpfhi" +
    r".*?begin odpfhi_nrpid.*?rx pps = (\d+\.\d+) Mpps rx bps = (\d+\.\d+) Gbps.*?end odpfhi_nrpid" +
    r".*?begin odpfho.*?rx pps = (\d+\.\d+) Mpps rx bps = (\d+\.\d+) Gbps.*?end odpfho" +
    r".*?begin odpfho_nrpid.*?rx pps = (\d+\.\d+) Mpps rx bps = (\d+\.\d+) Gbps.*?end odpfho_nrpid")
p2 = r".*?NUM_NID_ENTRY = # END"

start = 0
for size in size_list:
    print("size of NIDT = %d" % size)
    pattern = re.compile(p0.replace("#", str(size)) + p1 + p2.replace("#", str(size)))
    match = pattern.search(s, start)
    print("Mpps:")
    for i in range(1, 2 * 8 + 1, 2):
        print(match.group(i), end=" ")
    print("\nGbps:")
    for i in range(2, 2 * 8 + 1, 2):
        print(match.group(i), end=" ")
    print()
    start = match.end(16)