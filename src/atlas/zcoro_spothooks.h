#pragma once

#include "zexception.h"
#include "zlog.h"

#include <folly/Executor.h>
#include <folly/Function.h>
#include <folly/OperationCancelled.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Task.h>

#include <exception>
#include <limits>
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

    const auto isCancellation = [](const std::exception_ptr& ex) -> bool {
      if (!ex) {
        return false;
      }
      try {
        std::rethrow_exception(ex);
      }
      catch (const ZCancellationException&) {
        return true;
      }
      catch (const folly::OperationCancelled&) {
        return true;
      }
      catch (...) {
        return false;
      }
    };

    std::exception_ptr firstNonCancellation;
    size_t firstNonCancellationIndex = std::numeric_limits<size_t>::max();
    std::exception_ptr firstCancellation;
    size_t firstCancellationIndex = std::numeric_limits<size_t>::max();
    for (size_t i = 0; i < results.size(); ++i) {
      if (!results[i].hasException()) {
        continue;
      }
      auto ex = results[i].exception().to_exception_ptr();
      if (isCancellation(ex)) {
        if (!firstCancellation) {
          firstCancellation = std::move(ex);
          firstCancellationIndex = i;
        }
        continue;
      }
      if (!firstNonCancellation) {
        firstNonCancellation = std::move(ex);
        firstNonCancellationIndex = i;
      }
    }

    if (firstNonCancellation) {
      if (firstNonCancellationIndex < local.size() && !local[firstNonCancellationIndex].debugLabel.empty()) {
        LOG(ERROR) << "Coro hook failed at spot barrier: label='" << local[firstNonCancellationIndex].debugLabel << "'";
      }
      std::rethrow_exception(firstNonCancellation);
    }

    if (firstCancellation) {
      if (firstCancellationIndex < local.size() && !local[firstCancellationIndex].debugLabel.empty()) {
        LOG(INFO) << "Coro hook cancelled at spot barrier: label='" << local[firstCancellationIndex].debugLabel << "'";
      }
      std::rethrow_exception(firstCancellation);
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
