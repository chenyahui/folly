/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <folly/coro/TaskWrapper.h>
#include <folly/coro/safe/SafeAlias.h>

#if FOLLY_HAS_IMMOVABLE_COROUTINES

/// `NowTask<T>` quacks like `Task<T>` but is immovable, and must be
/// `co_await`ed in the same expression that created it.
///
/// Using `NowTask` by default brings considerable safety benefits.  With
/// `Task`, the following would be anti-patterns that cause dangling reference
/// bugs, but with `NowTask`, C++ lifetime extension rules ensure that they
/// simply work.
///   - Pass-by-reference into coroutines.
///   - Ephemeral coro lambdas with captures.
///   - Coro lambdas with capture-by-reference.
///
/// Notes:
///   - (subject to change) Unlike `SafeTask`, `NowTask` does NOT check
///     `safe_alias_of_v` for the return type `T`.  `NowTask` is essentially an
///     immediate async function -- it satisfies the structured concurrency
///     maxim of "lexical scope drives both control flow & lifetime".  That
///     lowers the odds that returned pointers/references are unexpectedly
///     invalid.  The one failure mode I can think of is that the
///     pointed-to-data gets invalidated by a concurrent thread of execution,
///     but in that case the program almost certainly has a data race --
///     regardless of the lifetime bug -- and that requires runtime
///     instrumentation (like TSAN) to detect in present-day C++.

namespace folly::coro {

template <safe_alias, typename>
class BackgroundTask;

template <typename T = void>
class NowTask;

template <typename T = void>
class NowTaskWithExecutor;

namespace detail {
template <typename T>
struct NowTaskWithExecutorCfg : DoesNotWrapAwaitable {
  using InnerTaskWithExecutorT = TaskWithExecutor<T>;
  using WrapperTaskT = NowTask<T>;
};
template <typename T>
using NowTaskWithExecutorBase =
    AddMustAwaitImmediately<TaskWithExecutorWrapperCrtp<
        NowTaskWithExecutor<T>,
        detail::NowTaskWithExecutorCfg<T>>>;
} // namespace detail

template <typename T>
class FOLLY_NODISCARD NowTaskWithExecutor final
    : public detail::NowTaskWithExecutorBase<T> {
 protected:
  using detail::NowTaskWithExecutorBase<T>::NowTaskWithExecutorBase;

  template <safe_alias, typename>
  friend class BackgroundTask; // for `unwrapTaskWithExecutor`, remove later
};

namespace detail {
template <typename T>
class NowTaskPromise final
    : public TaskPromiseWrapper<T, NowTask<T>, TaskPromise<T>> {};
template <typename T>
struct NowTaskCfg : DoesNotWrapAwaitable {
  using ValueT = T;
  using InnerTaskT = Task<T>;
  using TaskWithExecutorT = NowTaskWithExecutor<T>;
  using PromiseT = NowTaskPromise<T>;
};
template <typename T>
using NowTaskBase =
    AddMustAwaitImmediately<TaskWrapperCrtp<NowTask<T>, detail::NowTaskCfg<T>>>;
} // namespace detail

template <safe_alias, typename>
class SafeTask;

template <typename T>
class FOLLY_CORO_TASK_ATTRS NowTask final : public detail::NowTaskBase<T> {
 protected:
  using detail::NowTaskBase<T>::NowTaskBase;

  template <typename U> // can construct
  friend auto toNowTask(Task<U>);
  template <safe_alias S, typename U> // can construct
  friend auto toNowTask(SafeTask<S, U>);
  template <typename U> // can construct & `unwrapTask`
  friend auto toNowTask(NowTask<U>);
};

// NB: `toNowTask(SafeTask)` is in `SafeTask.h` to avoid circular deps.
template <typename T>
auto toNowTask(Task<T> t) {
  return NowTask<T>{std::move(t)};
}
template <typename T>
auto toNowTask(NowTask<T> t) {
  return NowTask<T>{std::move(t).unwrapTask()};
}

} // namespace folly::coro

template <typename T>
struct folly::safe_alias_for<::folly::coro::NowTask<T>>
    : safe_alias_constant<safe_alias::unsafe> {};

#endif
