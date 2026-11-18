#include <thread>
#include <vector>
#include <atomic>
std::atomic<int> g;
void w(int k, int* o){ *o = k*2; g.fetch_add(1); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
extern "C" int run_thr(){ int out[4]{}; std::vector<std::thread> v; for(int i=0;i<4;++i) v.emplace_back(w, i, &out[i]); for(auto& t: v) t.join(); auto id = std::this_thread::get_id(); (void)id; std::thread m([&]{ g.fetch_add(10); }); m.join(); std::this_thread::sleep_for(std::chrono::microseconds(50)); return g.load()==14 && out[3]==6 ? 0 : 1; }
