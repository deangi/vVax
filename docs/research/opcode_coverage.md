# Opcode coverage (generated)

- vVax implemented: **174**
- Tier A unique (static): **254**
- Tier B allowlist (MicroVAX integer/base): **174**
- Tier B missing: **0**
- Tier A∩B missing (presence gate): **0**
- Tier A deferred (float/CIS/packed/…): **80**

Presence gate: **Tier B empty**. Remaining Tier A hits are Tier C/D (F/D float, packed decimal, heavy CIS, G/H) unless a boot path proves otherwise.

## Blobs

- `xxboot_lba0_15`: 8192 bytes
- `/boot`: 76256 bytes
- `/netbsd`: 4103704 bytes

## Tier B missing

_none_

## Tier A deferred (Tier C/D candidates, by frequency)

- `0x42` SUBF2 [BASE] (count=9457)
- `0x20` ADDP4 [PACKD] (count=6555)
- `0x65` MULD3 [BSDFL] (count=3955)
- `0x74` EMODD [EMONL] (count=2989)
- `0x72` MNEGD [BSDFL] (count=2611)
- `0x73` TSTD [BSDFL] (count=2541)
- `0x6F` ACBD [EMONL] (count=2437)
- `0x76` CVTDF [BSDFL] (count=2417)
- `0x69` CVTDW [BSDFL] (count=2362)
- `0x61` ADDD3 [BSDFL] (count=2334)
- `0x64` MULD2 [BSDFL] (count=2272)
- `0x6E` CVTLD [BSDFL] (count=2186)
- `0x6C` CVTBD [BSDFL] (count=1989)
- `0x40` ADDF2 [BASE] (count=1658)
- `0x63` SUBD3 [BSDFL] (count=1536)
- `0x24` CVTPT [PACKD] (count=1428)
- `0x70` MOVD [BSDFL] (count=1411)
- `0x6D` CVTWD [BSDFL] (count=1288)
- `0x25` MULP [PACKD] (count=1251)
- `0x75` POLYD [EMONL] (count=1241)
- `0x08` CVTPS [PACKD] (count=1217)
- `0x66` DIVD2 [BSDFL] (count=1013)
- `0x68` CVTDB [BSDFL] (count=820)
- `0x46` DIVF2 [BASE] (count=805)
- `0x62` SUBD2 [BSDFL] (count=775)
- `0x67` DIVD3 [BSDFL] (count=766)
- `0x2A` SCANC [BASE] (count=712)
- `0x50` MOVF [BASE] (count=555)
- `0x3A` LOCC [BASE] (count=501)
- `0x44` MULF2 [BASE] (count=495)
- `0x6B` CVTRDL [BSDFL] (count=473)
- `0x4E` CVTLF [BASE] (count=451)
- `0x54` EMODF [EMONL] (count=451)
- `0x4C` CVTBF [BASE] (count=429)
- `0x43` SUBF3 [BASE] (count=425)
- `0x56` CVTFD [BSDFL] (count=425)
- `0x23` SUBP6 [PACKD] (count=422)
- `0x49` CVTFW [BASE] (count=414)
- `0x2E` MOVTC [EMONL] (count=399)
- `0x48` CVTFB [BASE] (count=394)
- `0x45` MULF3 [BASE] (count=388)
- `0x41` ADDF3 [BASE] (count=384)
- `0x09` CVTSP [PACKD] (count=365)
- `0x4D` CVTWF [BASE] (count=358)
- `0x34` MOVP [PACKD] (count=342)
- `0x52` MNEGF [BASE] (count=320)
- `0x2D` CMPC5 [BASE] (count=313)
- `0x38` EDITPC [EMONL] (count=313)
- `0x53` TSTF [BASE] (count=295)
- `0x36` CVTPL [PACKD] (count=289)
- `0x60` ADDD2 [BSDFL] (count=288)
- `0x2B` SPANC [BASE] (count=256)
- `0x4F` ACBF [EMONL] (count=241)
- `0xFC` XFC [BASE] (count=207)
- `0x2F` MOVTUC [EMONL] (count=206)
- `0xF8` ASHP [PACKD] (count=202)
- `0x22` SUBP4 [PACKD] (count=186)
- `0x55` POLYF [EMONL] (count=180)
- `0x26` CVTTP [PACKD] (count=166)
- `0x4B` CVTRFL [BASE] (count=165)
- `0x27` DIVP [PACKD] (count=162)
- `0x0B` CRC [EMONL] (count=160)
- `0x71` CMPD [BSDFL] (count=152)
- `0x29` CMPC3 [BASE] (count=150)
- `0x35` CMPP3 [PACKD] (count=143)
- `0x4A` CVTFL [BASE] (count=130)
- `0x51` CMPF [BASE] (count=123)
- `0x6A` CVTDL [BSDFL] (count=121)
- `0x47` DIVF3 [BASE] (count=109)
- `0x21` ADDP6 [PACKD] (count=99)
- `0x37` CMPP4 [PACKD] (count=93)
- `0x39` MATCHC [EMONL] (count=86)
- `0x3B` SKPC [BASE] (count=74)
- `0xF9` CVTLP [PACKD] (count=42)
- `0x132` CVTDH [EXTAC] (count=25)
- `0x16C` CVTBH [EXTAC] (count=18)
- `0x150` MOVG [BSGFL] (count=1)
- `0x152` MNEGG [BSGFL] (count=1)
- `0x156` CVTGH [EXTAC] (count=1)
- `0x169` CVTHW [EXTAC] (count=1)
