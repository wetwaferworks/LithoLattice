#include "WorkerScratch.h"

namespace litholattice {

cWorkerScratch& cWorkerScratch::forThisThread() {
  static thread_local cWorkerScratch s;
  return s;
}

}  // namespace litholattice
