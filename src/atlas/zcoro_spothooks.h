#pragma once

#include "zlog.h"

#include <folly/Executor.h>
#include <folly/Function.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Task.h>

#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nim {

// Hash helper for enum-class keys.
template<typename Enum>
struct ZEnumClassHash
{
  static_assert(std::is_enum_v<Enum>, "ZEnumClassHash requires an enum type");
  size_t operator()(Enum e) const noexcept
  {
    return static_cast<size_t>(e);
  }
};

// Register coroutine hooks at named "spots" and execute them with a barrier.
//
// This is intended to express sequencing constraints (e.g. Vulkan frame-slot
// reuse safe points) without hand-rolled callback chains or detached coroutines.
//
// Design goals:
// - Hooks choose their own executors (render thread, CPU pool, IO pool, ...).
// - reach(spot) is a barrier: it returns only after all hooks for that spot
//   finish.
// - Exceptions are not swallowed. We drain all hooks for correctness, then
//   rethrow the first exception so the caller can handle cancellation (or crash
//   on invariant violations).
template<typename Spot,
         typename Context,
         typename SpotHash = std::conditional_t<std::is_enum_v<Spot>, ZEnumClassHash<Spot>, std::hash<Spot>>>
class ZCoroSpotHooks final
{
public:
  using Hook = folly::Function<folly::coro::Task<void>(Context&)>;

  void registerHook(Spot spot, folly::Executor::KeepAlive<> ex, Hook hook, std::string_view debugLabel = {})
  {
    CHECK(ex) << "ZCoroSpotHooks::registerHook requires a valid executor";
    CHECK(hook) << "ZCoroSpotHooks::registerHook requires a non-empty hook";
    std::scoped_lock g(_state->mu);
    _state->hooks[spot].push_back(Item{std::move(ex), std::move(hook), std::string(debugLabel)});
  }

  // Barrier: returns only after all hooks for this spot finish.
  folly::coro::Task<void> reach(Spot spot, Context& ctx)
  {
    std::vector<Item> local;
    {
      std::scoped_lock g(_state->mu);
      auto it = _state->hooks.find(spot);
      if (it == _state->hooks.end() || it->second.empty()) {
        co_return;
      }
      local = std::move(it->second);
      it->second.clear();
    }

    std::vector<folly::coro::TaskWithExecutor<void>> tasks;
    tasks.reserve(local.size());
    for (auto& item : local) {
      tasks.push_back(folly::coro::co_withExecutor(std::move(item.ex), item.hook(ctx)));
    }

    auto results = co_await folly::coro::collectAllTryRange(std::move(tasks));
    CHECK(results.size() == local.size()) << "collectAllTryRange result size mismatch";

    std::exception_ptr firstException;
    for (size_t i = 0; i < results.size(); ++i) {
      if (!results[i].hasException()) {
        continue;
      }
      if (!firstException) {
        firstException = results[i].exception().to_exception_ptr();
        if (!local[i].debugLabel.empty()) {
          LOG(ERROR) << "Coro hook failed at spot barrier: label='" << local[i].debugLabel << "'";
        }
      }
    }

    if (firstException) {
      std::rethrow_exception(firstException);
    }
    co_return;
  }

private:
  struct Item
  {
    folly::Executor::KeepAlive<> ex;
    Hook hook;
    std::string debugLabel;
  };

  struct State
  {
    std::mutex mu;
    std::unordered_map<Spot, std::vector<Item>, SpotHash> hooks;
  };

  std::shared_ptr<State> _state = std::make_shared<State>();
};

} // namespace nim
