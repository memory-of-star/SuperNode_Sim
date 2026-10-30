#pragma once
#include "ns3/core-module.h"
namespace ns3 {
inline uint64_t SueQuantumPs=1;
inline uint64_t SueRoundPs(uint64_t ps){return ((ps+SueQuantumPs-1)/SueQuantumPs)*SueQuantumPs;}
inline Time SuePicoSeconds(uint64_t ps){return PicoSeconds(SueRoundPs(ps));}
}
