#include "utils/save-efficiency.hpp"
#include <cassert>
#include <vector>
#include <random>
#include <limits>
#include <iostream>
struct Packet { int dts; int serial; bool operator==(const Packet &p) const { return dts==p.dts && serial==p.serial; } };
int main() {
 using namespace ReplayBufferPro;
 assert(canAcceptClipRequest(0)); assert(canAcceptClipRequest(1)); assert(canAcceptClipRequest(2));
 assert(!canAcceptClipRequest(3)); assert(!canAcceptClipRequest(4));
 assert(!canAcceptClipRequest(std::numeric_limits<std::size_t>::max()));
 assert(hasFullClipDuration(600.0,600));
 assert(hasFullClipDuration(601.8,600));
 assert(hasFullClipDuration(599.96,600));
 assert(!hasFullClipDuration(599.90,600));
 assert(!hasFullClipDuration(540.0,600));
 assert(!hasFullClipDuration(0.0,600));
 assert(!hasFullClipDuration(std::numeric_limits<double>::infinity(),600));
 assert(!hasFullClipDuration(std::numeric_limits<double>::quiet_NaN(),600));
 assert(!hasFullClipDuration(600.0,0));
 std::mt19937 rng(742);
 auto less=[](const Packet &a,const Packet &b){return a.dts<b.dts;};
 for(int n=0;n<512;++n) {
  std::vector<Packet> a;
  for(int i=0;i<n;++i) a.push_back({static_cast<int>(rng()%23),i});
  auto expected=a; std::stable_sort(expected.begin(),expected.end(),less);
  stableSortIfNeeded(a.begin(),a.end(),less); assert(a==expected);
  const auto sorted=a; stableSortIfNeeded(a.begin(),a.end(),less); assert(a==sorted);
 }
 std::cout << "6 admission, 9 full-duration checks and 1024 stable-order comparisons passed\n";
}
