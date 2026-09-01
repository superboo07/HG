# Graphics vtable initialization oracle

Date: 2026-08-27  
Game build: `SLUS-21075-v1.01`  
ELF SHA-256: `3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8`  
ISO SHA-256: `3f8c658eee1b35147672c597c0dbe8e36552172abf83571c52753eec9a50bcca`  
Oracle: PCSX2 2.6.3, isolated portable profile, SCPH-39001 BIOS, slot 28012

## Question

At the native early-graphics indirect call from `0x001B71FC`, should the object
at `0x004F1920` have a null first word, or is native initialization missing or
corrupting a vtable pointer?

## Reproducible observation

Cold-boot the verified owned image in the isolated PCSX2 profile and sample
32-bit guest words at `0x004F1920`, `0x004F1924`, `0x004F1928`,
`0x004F192C`, and `0x004F1930` through PINE at approximately 100 ms intervals
for twelve seconds. The retained raw observation is the ignored local artifact
`build/oracle-vtable-4f1920.jsonl`.

`0x004F1920` begins at zero, changes to `0x0046AC50` between samples 1 and 2
(94-204 ms after sampling begins), and remains `0x0046AC50` through sample 119
at 11.954 seconds. `0x004F1930` later becomes `0x20000000`. Therefore the null
native indirect target is incorrect; skipping it is not an oracle-supported
compatibility behavior.

## Native differential and resolution

The reusable `PS2X_TRACE_GUEST_WRITE` diagnostic showed native guest code
correctly storing `0x0046AC50` at PC `0x0020E578`. The DBCMAN compatibility
service then left a packed request word in response word 2. Guest transport
`0x001EF840` interpreted that word as a payload length and copied zeros from
its fixed 0x90-byte RPC buffer beginning at vibration-profile destination
`0x004F1782`, overrunning through `0x004F1920` at PC `0x001EF91C`.

The DBCMAN service now completes unsupported commands with an explicit
zero-length payload. A focused regression verifies that response word 2 is
zero and no payload bytes are fabricated. The next native cold boot preserved
the vtable and advanced beyond indirect call `0x001B71FC`.
