"""Window-z test: trigger measurement windows on slaves directly over the wire
('M<seg>' to UDP 5000, reply to sender) and record z per window.
Usage: python wintest.py <n_windows> <segs> <out.csv> <ip> [<ip> ...]
All listed nodes get the same broadcast-like trigger (sent back to back)."""
import socket, sys, time, random

n_win = int(sys.argv[1]); segs = int(sys.argv[2]); out = sys.argv[3]; ips = sys.argv[4:]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("", 0))
s.settimeout(0.5)
seq = random.randint(10**8, 2 * 10**8)
f = open(out, "w")
f.write("i;t;ip;z;h1;h2;wsig;raw\n")
t0 = time.time()
for i in range(n_win):
    seq += 1
    msg = f"EL1 {seq} M{segs}".encode()
    for ip in ips:
        s.sendto(msg, (ip, 5000))
    got = {}
    deadline = time.time() + 15
    resend = time.time() + 6
    while len(got) < len(ips) and time.time() < deadline:
        if time.time() > resend:
            for ip in ips:
                if ip not in got:
                    s.sendto(msg, (ip, 5000))
            resend = time.time() + 6
        try:
            data, addr = s.recvfrom(512)
        except socket.timeout:
            continue
        txt = data.decode(errors="replace").strip()
        parts = txt.split(" ", 2)
        if len(parts) < 3 or parts[0] != "EL1" or int(parts[1]) != seq:
            continue
        got[addr[0]] = parts[2]
    for ip in ips:
        r = got.get(ip, "")
        z = h1 = h2 = wsig = ""
        if r.startswith("Z:"):
            body = r[2:]
            fields = body.split(",")
            z = fields[0]
            nums = [x for x in fields[1:] if not x.startswith("wsig=")]
            if len(nums) >= 2:
                h1, h2 = nums[0], nums[1]
            for x in fields:
                if x.startswith("wsig="):
                    wsig = x[5:]
        f.write(f"{i};{time.time()-t0:.2f};{ip};{z};{h1};{h2};{wsig};{r}\n")
    f.flush()
    if i % 25 == 0:
        print(i, round(time.time() - t0, 1), {k: v[:40] for k, v in got.items()}, flush=True)
# release the slaves' session latch
seq += 1
for ip in ips:
    s.sendto(f"EL1 {seq} A".encode(), (ip, 5000))
print("done", round(time.time() - t0, 1), flush=True)
