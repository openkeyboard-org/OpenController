#!/usr/bin/env python3
"""Read the receiver's link-encryption telemetry (CMD_CRYPT_DIAG v2) and bond.

The v1 payload exported only the verified count and the per-reason drop array,
both of which count frames that already reached rf_crypt_rx(). A frame lost
before that -- never received, misclassified, refused by the FIFO, or discarded
by the session-mint flush -- left every counter at zero, which is exactly what
"the keyboard transmitted nothing" looks like. v2 adds the pre-verify sink
counters that separate those cases.
"""
import struct
import sys

VID, PID = 0x0C45, 0xFEFE
IFACE = 4

# Order must track rf_crypt_status_t in rf_crypt.h exactly -- the array is
# indexed by the enum, so a mislabelled entry silently renames a failure mode.
REASONS = ["OK", "DROP_SHAPE", "DROP_INACTIVE", "DROP_MAC", "DROP_REPLAY",
           "FAULT_ENGINE"]


def find_hidraw(vid, pid, iface):
    import glob
    import os
    for path in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        try:
            uevent = open(os.path.join(path, "device/uevent")).read()
            phys = [l for l in uevent.splitlines() if l.startswith("HID_PHYS")]
            if f"{vid:08X}" not in uevent.replace("HID_ID=0003:0000", "HID_ID=0003:"):
                pass
            if f"{vid:08X}:0000{pid:04X}" not in uevent:
                continue
            if not phys or not phys[0].endswith(f"input{iface}"):
                continue
            return "/dev/" + os.path.basename(path)
        except OSError:
            continue
    return None


def txn(fd, cmd, body=b"", timeout=1.0):
    """One IAP request/response, matching the CLI's build_packet:
    [report-id 0][cmd][len][body...][checksum] padded to 65 B, where
    checksum = (cmd + len + sum(body)) & 0xFF. The device ignores unchecksummed
    packets silently, so omitting it just times out."""
    import os
    import select
    body = bytes(body)
    pkt = bytearray(65)
    pkt[0] = 0x00
    pkt[1] = cmd
    pkt[2] = len(body)
    pkt[3:3 + len(body)] = body
    pkt[3 + len(body)] = (cmd + len(body) + sum(body)) & 0xFF
    os.write(fd, bytes(pkt))
    r, _, _ = select.select([fd], [], [], timeout)
    if not r:
        return None
    return os.read(fd, 64)


def main():
    import os
    dev = find_hidraw(VID, PID, IFACE)
    if not dev:
        print("no dongle IAP interface found")
        return 1
    fd = os.open(dev, os.O_RDWR)

    # CRYPT_DIAG is served unarmed, but BondRead needs the dispatcher armed:
    # handshake, then GetDevInfo(1) which sets iap_armed.
    txn(fd, 0x5A, b"WCH@HFD")
    txn(fd, 0x84, bytes([0x01, 0x00, 0x00, 0x00]))

    r = txn(fd, 0x94)
    if not r or r[0] != 0x94:
        print(f"CRYPT_DIAG: no/bad reply: {r.hex() if r else None}")
        os.close(fd)
        return 1

    n = r[1]
    ok = struct.unpack_from("<I", r, 2)[0]
    reasons = struct.unpack_from("<6I", r, 6)
    # Two live layouts, told apart by length (the dongle's status profile
    # byte is the other detector): the BENCH profile serves the v4 62-byte
    # payload with the pre-verify sink forensics; the PRODUCT profile serves
    # 38 bytes -- ok + reasons + the stale-abort hardening telemetry
    # (aes_redo / announce_retry / boot KAT).
    # Exact lengths, not thresholds: 62 = bench v4, 42 = product (since
    # plain_drop was promoted 2026-08-22), 38 = product before that. A `>=`
    # ladder matched a bench reply as product and mislabelled its fields.
    kind = {62: "bench v4", 42: "product", 38: "product (pre-plain_drop)"}.get(n, "v1")
    print(f"payload {n} B  ({kind})")
    print(f"  verified (ok)      {ok}")
    for i, name in enumerate(REASONS):
        if i == 0:
            continue
        print(f"  drop {name:<14} {reasons[i]}")
    if n == 62:
        conn_rx, enc_shape, fifo_full, flush_drop, plain_drop = \
            struct.unpack_from("<5I", r, 30)
        len_max, len_max_tag = r[50], r[51]
        print("  --- pre-verify sink ---")
        print(f"  conn_rx (enc bond) {conn_rx}")
        print(f"  len_max seen       {len_max}  tag 0x{len_max_tag:02X}")
        print(f"  enc_shape accepted {enc_shape}")
        print(f"  fifo_full refused  {fifo_full}")
        print(f"  flush_drop         {flush_drop}")
        print(f"  plain_drop         {plain_drop}")
    elif n in (38, 42):
        aes_redo, announce_retry = struct.unpack_from("<2I", r, 30)
        kat_run, kat_fail = r[38], r[39]
        print("  --- stale-abort hardening ---")
        print(f"  aes_redo caught    {aes_redo}")
        print(f"  announce_retry     {announce_retry}")
        print(f"  boot KAT           {'FAIL' if kat_fail else ('ok' if kat_run else 'not run')}")
        if n == 42:
            # A refused plaintext downgrade on an active encrypted bond. Product
            # builds had no counter for this at all before 2026-08-22, so an
            # active downgrade attempt read as RF flakiness.
            print(f"  plain_drop refused {struct.unpack_from('<I', r, 40)[0]}")

    r = txn(fd, 0x88)
    if r and r[0] == 0x88 and r[1] >= 44:
        # [0]=ack [1]=len [2]=status (bit1 = link key redacted) [3..]=record.
        status = r[2]
        rec = r[3:3 + r[1]]
        magic, ver, flags, ci, aa, to, rsv = struct.unpack_from("<IBBHIHH", rec, 0)
        peer = rec[22:28]
        print(f"bond:  status 0x{status:02X}"
              f"{'  (key redacted)' if status & 0x02 else ''}")
        print(f"  magic {magic:08X} version {ver}")
        print(f"  flags 0x{flags:02X} "
              f"(capable={'y' if flags & 1 else 'n'} "
              f"key={'y' if flags & 2 else 'n'}) "
              f"-> enc_active={'YES' if (flags & 3) == 3 else 'NO'}")
        print(f"  session AA 0x{aa:08X}  peer {peer.hex(':')}")
    else:
        print(f"bond: unexpected reply {r.hex() if r else None}")

    os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
