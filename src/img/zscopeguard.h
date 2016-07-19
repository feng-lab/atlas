#ifndef ZSCOPEGUARD_H
#define ZSCOPEGUARD_H

#include <functional>

namespace nim {

// http://stackoverflow.com/questions/10270328/the-simplest-and-neatest-c11-scopeguard
class scope_guard {
public:
    template<class Callable>
    scope_guard(Callable && undo_func)
      : f(std::forward<Callable>(undo_func))
    {}

    scope_guard(scope_guard && other)
      : f(std::move(other.f))
    {
        other.f = nullptr;
    }

    ~scope_guard()
    {
        if(f) f(); // must not throw
    }

    void dismiss() throw() { f = nullptr; }

    scope_guard(const scope_guard&) = delete;
    void operator = (const scope_guard&) = delete;

private:
    std::function<void()> f;
};

} // namespace nim

#endif // ZSCOPEGUARD_H
