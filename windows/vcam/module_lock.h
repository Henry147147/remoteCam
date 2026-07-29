// Keeps rc-vcam.dll loaded for as long as any object it created is alive.
//
// COM asks a DLL whether it can be unloaded via DllCanUnloadNow, and unloads it when
// the answer is yes. The answer is only correct if every object living in the module is
// counted -- an object that does not register itself is an object COM does not know
// about, and the Frame Server will happily unmap the code that object's threads are
// currently executing. The symptom is an access violation deep inside svchost.exe with
// no RemoteCam frame on the stack, because the frames belong to a module that no longer
// exists.
//
// Anything allocated in this DLL and handed out across the COM boundary holds one of
// these for its whole lifetime.

#ifndef RC_VCAM_MODULE_LOCK_H
#define RC_VCAM_MODULE_LOCK_H

namespace rcvcam {

void moduleAddRef();
void moduleRelease();
long moduleRefCount();

class ModuleLock {
 public:
  ModuleLock() { moduleAddRef(); }
  ~ModuleLock() { moduleRelease(); }

  // Non-copyable: the count tracks objects, and a copied lock would either
  // double-count or, worse, be moved from and release a count it never took.
  ModuleLock(const ModuleLock&) = delete;
  ModuleLock& operator=(const ModuleLock&) = delete;
};

}  // namespace rcvcam

#endif  // RC_VCAM_MODULE_LOCK_H
