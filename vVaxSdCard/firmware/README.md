# Firmware / ROM blobs (user-supplied)

vVax **does not require** proprietary KA630/VMB ROM for NetBSD:

- Default path: host loads NetBSD **xxboot** (MSCP LBA 0–15) and runs a
  FROM750-style handoff (`vax_boot`) with a magic R6 disk-read stub.
- Configure boot unit with `[disks] boot=a` (or `b`) in `/vaxconfig.ini`.

Optional: place MicroVAX II–class firmware here for a future ROM-map path
(KA630 console / VMB dumps). **Do not commit copyrighted DEC firmware.**

Typical Open SIMH study often uses ROM images alongside disk media; the
NetBSD-on-MSCP MVP uses the open on-disk bootstrap instead.
