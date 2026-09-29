// symbols.cpp — 见 symbols.h

#include "symbols.h"

namespace pdscope { namespace pd {

const uint8_t DEC4B5B[32] = {
  0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x13, 0x14,
  0x10, 0x01, 0x04, 0x05, 0x10, 0x16, 0x06, 0x07,
  0x10, 0x12, 0x08, 0x09, 0x02, 0x03, 0x0A, 0x0B,
  0x11, 0x15, 0x0C, 0x0D, 0x0E, 0x0F, 0x00, 0x10,
};

// 下标 0x00…0x16，每项是 [长名, 短名]
const char* SYM_NAME[23][2] = {
  {"0x0", "0"}, {"0x1", "1"}, {"0x2", "2"}, {"0x3", "3"},
  {"0x4", "4"}, {"0x5", "5"}, {"0x6", "6"}, {"0x7", "7"},
  {"0x8", "8"}, {"0x9", "9"}, {"0xA", "A"}, {"0xB", "B"},
  {"0xC", "C"}, {"0xD", "D"}, {"0xE", "E"}, {"0xF", "F"},
  {"ERROR", "X"}, {"SYNC-1", "S1"}, {"SYNC-2", "S2"}, {"SYNC-3", "S3"},
  {"RST-1", "R1"}, {"RST-2", "R2"}, {"EOP", "#"},
};

std::string symName(uint8_t s) {
  if (s < 23) return SYM_NAME[s][1];
  return "??";
}

// 注意：sequence 用符号「值」（0x11…0x16），不是下标。
const std::vector<OrderedSet> SOP_ORDERED_SETS = {
  {{SYM_SYNC1, SYM_SYNC1, SYM_SYNC1, SYM_SYNC2}, "SOP", "SOP", "Port Partner", "port"},
  {{SYM_SYNC1, SYM_SYNC1, SYM_SYNC3, SYM_SYNC3}, "SOP'", "SOP'", "Cable Plug (near end)", "cable"},
  {{SYM_SYNC1, SYM_SYNC3, SYM_SYNC1, SYM_SYNC3}, "SOP''", "SOP''", "Cable Plug (far end)", "cable"},
  {{SYM_SYNC1, SYM_RST2, SYM_RST2, SYM_SYNC3}, "SOP' Debug", "SOP'D", "Cable Plug (near end, debug)", "cable"},
  {{SYM_SYNC1, SYM_RST2, SYM_SYNC3, SYM_SYNC2}, "SOP'' Debug", "SOP''D", "Cable Plug (far end, debug)", "cable"},
  {{SYM_RST1, SYM_SYNC1, SYM_RST1, SYM_SYNC3}, "Cable Reset", "CRST", "Cable Plug", "cable"},
  {{SYM_RST1, SYM_RST1, SYM_RST1, SYM_RST2}, "Hard Reset", "HRST", "Port Partner", "port"},
};

const std::vector<std::vector<uint8_t>> SOP_SEQUENCES = []() {
  std::vector<std::vector<uint8_t>> v;
  for (const auto& s : SOP_ORDERED_SETS)
    v.push_back({s.sequence[0], s.sequence[1], s.sequence[2], s.sequence[3]});
  return v;
}();

std::string sopKey(const uint8_t sym[4]) {
  std::string k;
  for (int i = 0; i < 4; i++) {
    if (i) k += ',';
    k += std::to_string(sym[i]);
  }
  return k;
}

const OrderedSet* findOrderedSetByKey(const std::string& key) {
  for (const auto& s : SOP_ORDERED_SETS)
    if (sopKey(s.sequence) == key) return &s;
  return nullptr;
}

const OrderedSet* findOrderedSetByName(const std::string& name) {
  for (const auto& s : SOP_ORDERED_SETS)
    if (s.name == name) return &s;
  return nullptr;
}

OrderedSetMatch matchOrderedSet(const std::vector<uint8_t>& symbols) {
  const OrderedSet* best = nullptr;
  int bestMatched = 0;
  for (const auto& s : SOP_ORDERED_SETS) {
    int same = 0;
    for (size_t i = 0; i < symbols.size() && i < 4; i++)
      if (symbols[i] == s.sequence[i]) same++;
    if (!best || same > bestMatched) {
      best = &s;
      bestMatched = same;
    }
  }
  return {best, bestMatched, 4};
}

}}  // namespace pdscope::pd
