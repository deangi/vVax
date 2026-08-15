#pragma once
#ifndef HOST_LIB_TERM_PERSONALITY_H
#define HOST_LIB_TERM_PERSONALITY_H

// Pluggable console parser. vpdp1170 / v8088 use VT100.
// vZ80 defaults to ADM-3A (WordStar); INI terminal= may select VT100.

enum HostTermPersonality {
  HOST_TERM_VT100 = 0,
  HOST_TERM_ADM3A = 1,
};

#endif  // HOST_LIB_TERM_PERSONALITY_H
