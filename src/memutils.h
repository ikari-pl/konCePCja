#ifndef MEMUTILS_H
#define MEMUTILS_H

namespace memutils
{
  template<typename koncpc_cb>
  class scope_exit {
    koncpc_cb cb;
    bool upd {true};
  public:
    scope_exit(koncpc_cb _cb) : cb(std::move(_cb)) {}
    scope_exit(const scope_exit &) = delete;
    scope_exit& operator=(const scope_exit &) = delete;
    scope_exit(scope_exit &&d) : cb(d.cb), upd(d.upd)
    {
      d.upd = false;
    }
    ~scope_exit()
    {
      if (upd)
        cb();
    }
  };
}

#endif

