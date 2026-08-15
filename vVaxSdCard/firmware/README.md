# Firmware / ROM blobs (user-supplied)

Place MicroVAX II–class firmware here, for example:

- KA630 console / boot ROM dump
- VMB / secondary bootstrap bits if required by your bring-up path

**Do not commit copyrighted DEC firmware to a public repository.**
This folder is a placeholder; the sketch looks for paths you configure later.

Typical Open SIMH study often uses ROM images alongside disk media; vVax will
load whatever paths `/vaxconfig.ini` (or future keys) name once the CPU ROM
map is implemented.
