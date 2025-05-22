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

#include <folly/coro/safe/SafeTask.h>
#include <folly/coro/safe/detail/AsyncClosureBindings.h>
#include <folly/detail/tuple.h>

#if FOLLY_HAS_IMMOVABLE_COROUTINES
FOLLY_PUSH_WARNING
FOLLY_DETAIL_LITE_TUPLE_ADJUST_WARNINGS

// DANGER: Do NOT touch this implementation without understanding the contract,
// at least at the level of the tl;dr in `safe/AsyncClosure.h`, and in full
// depth if you're changing `safe_alias` measurements.

namespace folly::coro::detail {

void async_closure_set_cancel_token(
    async_closure_private_t priv, auto&& arg, const CancellationToken& ctok) {
  if constexpr ( // DO NOT USE: for AsyncObject only
      requires { arg.privateHackSetParentCancelToken(arg, priv, ctok); }) {
    arg.privateHackSetParentCancelToken(arg, priv, ctok);
  } else if constexpr ( //
      requires {
        {
          arg.get_lref().setParentCancelToken(priv, ctok)
        } -> std::same_as<void>;
      }) {
    arg.get_lref().setParentCancelToken(priv, ctok);
  }
}

auto async_closure_make_cleanup_tuple(
    async_closure_private_t priv, auto&& arg, const exception_wrapper* err) {
  // `co_cleanup` is allowed to return `Task<void>` or a tuple of them.
  auto to_lite_tuple = []<typename T>(T task) {
    static_assert(
        noexcept_awaitable_v<T> && std::is_void_v<semi_await_result_t<T>>,
        "`co_cleanup()` must return a `noexcept`-awaitable `void` coro. "
        "Simply change your return type to `AsNoexcept<Task<void>>`.");
    return lite_tuple::tuple{std::move(task)};
  };
  if constexpr (has_async_object_private_hack_co_cleanup<decltype(arg)>) {
    return arg.privateHack_co_cleanup(std::move(arg), priv, err);
  } else {
    using ArgT = typename std::remove_reference_t<decltype(arg)>::capture_type;
    if constexpr (has_async_closure_co_cleanup_with_error<ArgT>) {
      return to_lite_tuple(std::move(arg.get_lref()).co_cleanup(priv, err));
    } else if constexpr (has_async_closure_co_cleanup_error_oblivious<ArgT>) {
      return to_lite_tuple(std::move(arg.get_lref()).co_cleanup(priv));
    } else {
      return lite_tuple::tuple{};
    }
  }
}

template <typename T>
concept has_result_after_cleanup = requires(
    lift_unit_t<T> t, async_closure_private_t priv) {
  std::move(t).result_after_cleanup(priv);
};

auto async_closure_outer_coro_result(async_closure_private_t priv, auto r) {
  if constexpr (has_result_after_cleanup<
                    std::remove_reference_t<decltype(r)>>) {
    return std::move(r).result_after_cleanup(priv);
  } else {
    (void)priv;
    return r;
  }
}

template <bool SetCancelTok, typename ResultT, safe_alias OuterSafety>
auto async_closure_make_outer_coro(
    async_closure_private_t priv, auto inner_mover, auto storage_ptr) {
  return lite_tuple::apply(
      [&](auto... reversed_noexcept_cleanups) {
        return async_closure_outer_coro<SetCancelTok, ResultT, OuterSafety>(
            priv,
            // Doesn't downgrade safety, since movers are library-internal
            // "unsafe" types that don't expose the inner type's `safe_alias`.
            std::move(inner_mover),
            std::move(storage_ptr),
            // We don't require a `SafeTask` for `co_cleanup` because the coro
            // cannot outlive the object (or `exception_ptr*`) it references.
            manual_safe_val(std::move(reversed_noexcept_cleanups))...);
      },
      // Contract: `co_cleanup()`s are awaited sequentially right-to-left, in
      // the reverse of the construction order.  All cleanups finish before any
      // of the destructors; those also run right-to-left.
      //
      // Implementation notes:
      //   - `bad_alloc` safety: make the tasks before awaiting the inner coro.
      //   - This "apply" is outside of `async_closure_outer_coro` because
      //     that saves us a coro frame allocation.
      lite_tuple::reverse_apply( // Merge `co_cleanup` tuples from all the args
          [&](auto&... args) {
            return lite_tuple::tuple_cat(async_closure_make_cleanup_tuple(
                priv, args, storage_ptr->inner_err_ptr())...);
          },
          storage_ptr->storage_tuple_like()));
}

template <
    bool SetCancelTok,
    typename ResultT,
    safe_alias OuterSafety,
    typename OuterResT = drop_unit_t<decltype(async_closure_outer_coro_result(
        std::declval<async_closure_private_t>(),
        std::declval<lift_unit_t<ResultT>&&>()))>>
std::conditional_t<
    OuterSafety >= safe_alias::closure_min_arg_safety,
    SafeTask<OuterSafety, OuterResT>,
    NowTask<OuterResT>>
async_closure_outer_coro(
    async_closure_private_t priv,
    auto inner_mover,
    auto storage_ptr,
    auto... reversed_noexcept_cleanups) {
  auto& inner_err = *storage_ptr->inner_err_ptr();
  if constexpr (kIsDebug) {
    inner_err.reset(); // Clear `BUG_co_cleanup_must_not_copy_error`
  }

  // Pass our cancellation token to args that want it for cleanup.  The user
  // code can throw -- e.g. `CancellationToken::merge()` may allocate.
  if constexpr (SetCancelTok) {
    const auto& ctok = co_await co_current_cancellation_token;
    inner_err = try_and_catch([&]() {
      lite_tuple::apply(
          [&](auto&&... args) {
            (async_closure_set_cancel_token(priv, args, ctok), ...);
          },
          storage_ptr->storage_tuple_like());
    });
  }

  // Await the inner task (unless some `setParentCancelToken` failed)
  Try<ResultT> res;
  if (!inner_err) {
    // NOTE: Here and below, assume that the semi-awaitable `co_viaIfAsync`
    // machinery for `Task` (or other `inner` type) is non-throwing.
    // I would love a `static_assert(noexcept(...))` to prove this, but that
    // requires plumbing `noexcept(noexcept(...))` annotations through more
    // of `ViaIfAsync.h`.
    res = co_await co_awaitTry(std::move(inner_mover)());
    if (res.hasException()) {
      inner_err = std::move(res.exception());
    }
  }

  // We took the cleanup tasks as a pack to let us await them without making an
  // extra coro frame.
  (co_await std::move(reversed_noexcept_cleanups.get()), ...);

  if (FOLLY_LIKELY(res.hasValue())) {
    if constexpr (std::is_void_v<ResultT>) {
      co_return;
    } else {
      co_return async_closure_outer_coro_result(priv, std::move(res).value());
    }
  } else if (FOLLY_LIKELY(res.hasException())) {
    co_yield co_error(std::move(inner_err));
  } else { // should never happen
    co_yield co_error(UsingUninitializedTry{});
  }
  (void)storage_ptr; // This param keeps the stored args alive
}

// E.g. maps <0, 2, 1, 0, 2> to <0, 2, 3, 3> -- see Test.cpp
template <auto Sum, auto...>
inline constexpr auto cumsum_except_last = vtag<>;
template <auto Sum, auto Head, auto... Tail>
inline constexpr auto cumsum_except_last<Sum, Head, Tail...> =
    []<auto... Vs>(vtag_t<Vs...>) {
      return vtag<Sum, Vs...>;
    }(cumsum_except_last<Sum + Head, Tail...>);

// When returned from `bind_captures_to_closure`, this wraps a coroutine
// instance.  This reconciles two goals:
//  - Let tests cover the `is_safe()` logic.
//  - `static_assert()` the closure's safety before releasing it.
//
// Closure safety checks follow the model of `SafeTask.h` -- and actually
// reuse most of that implementation by requiring the inner coro to be a
// `SafeTask`.
//
// Note that we don't check whether the callable passed into `async_closure`
// is stateless, and we don't need to -- it is executed eagerly, and may be
// a coroutine wrapper.  The coro callable underlying the inner `SafeTask`
// will have been verified to be stateless.
//
// Future: An `AsyncGenerator` closure flavor is possible, just think about
// safety assertions on the yielded type, and review
// https://fburl.com/asyncgenerator_delegation
template < // inner coro safety is measured BEFORE re-wrapping it!
    safe_alias OuterSafety,
    safe_alias InnerSafety,
    typename OuterMover>
class async_closure_wrap_coro {
 private:
  OuterMover outer_mover_;

 protected:
  template <auto>
  friend auto bind_captures_to_closure(auto&&, auto);
  explicit async_closure_wrap_coro(
      vtag_t<OuterSafety, InnerSafety>, OuterMover outer_mover)
      : outer_mover_(std::move(outer_mover)) {}

 public:
  // Don't allow closures with `unsafe*` args.
  static constexpr bool has_safe_args =
      (OuterSafety >= safe_alias::closure_min_arg_safety);

  // The reason we need `SafeTask` here is that it have already detected any
  // by-reference arguments (impossible to detect otherwise), stateful
  // coros, and unsafe return types.
  static constexpr bool is_inner_coro_safe =
      (InnerSafety >= safe_alias::unsafe_closure_internal);

  // KEEP IN SYNC with `release_outer_coro`. Separate for testing.
  static consteval bool is_safe() {
    return has_safe_args && is_inner_coro_safe;
  }

  // Delay the `static_assert`s so we can test `bind_captures_to_closure`
  // on unsafe inputs.
  auto release_outer_coro() && {
    // KEEP IN SYNC with `is_safe`.
    static_assert(
        has_safe_args,
        "Args passed into `async_closure()` must have `safe_alias_of_v` of "
        "at least `shared_cleanup`. `NowTask` and `async_now_closure()` do "
        "not have this constraint. If you need a closure, use `manual_safe_*` "
        "to work around this, and comment with a proof of why your usage is "
        "memory-safe.");
    static_assert(
        is_inner_coro_safe,
        "`async_closure` currently only supports `SafeTask` as the inner coro.");
    return std::move(outer_mover_)();
  }
};

// The compiler cannot deduce that `async_closure_outer_stored_arg` cannot
// occur when `storage_ptr` is `nullopt_t`.  This helper function just
// delays instantiation of `storage_ptr->`.
template <size_t Idx>
decltype(auto) get_from_storage_ptr(auto& p) {
  return lite_tuple::get<Idx>(p->storage_tuple_like());
}

template <bool Debug = kIsDebug> // ODR safeguard
inline auto async_closure_default_inner_err() {
  if constexpr (Debug) {
    // If you see this diagnostic, check that your `co_cleanup` does not
    // inadvertently copy the `exception_wrapper` parameter before creating the
    // coro frame.  Store the provided pointer instead.
    struct BUG_co_cleanup_must_not_copy_error : std::exception {};
    return make_exception_wrapper<BUG_co_cleanup_must_not_copy_error>();
  } else {
    return exception_wrapper{};
  }
}

template <typename... Ts>
struct async_closure_storage {
  explicit async_closure_storage(capture_private_t priv, auto&&... as)
      : inner_err_(async_closure_default_inner_err()),
        // Curly braces guarantee that in-place construction is left-to-right
        storage_tuple_{Ts{priv, static_cast<decltype(as)>(as)}...} {}

  // We go through getters so that `AsyncObject` can reuse closure machinery.
  // Note that we only need lvalue refs to the storage tuple, meaning that
  // returning a ref-to-a-tuple is as good as a tuple-of-refs here.
  // We return an rvalue ref for compatibility with the latter scenario.
  auto&& storage_tuple_like() { return storage_tuple_; }
  auto* inner_err_ptr() { return &inner_err_; }

  // For `bad_alloc` safety, we must create the cleanup coros before awaiting
  // the inner coro.  This preallocated exception (which is passed to the
  // cleanup coros by-reference) further enables us to create the cleanup coros
  // before we even create the outer coro.  That avoids an extra coro frame
  // that would otherwise be need to await a cleanup tuple.
  exception_wrapper inner_err_;
  lite_tuple::tuple<Ts...> storage_tuple_;
};

template <size_t StorageI, typename Bs>
decltype(auto) async_closure_bind_inner_coro_arg(
    capture_private_t priv, Bs& bs, auto& storage_ptr) {
  if constexpr (is_instantiation_of_v<async_closure_outer_stored_arg, Bs>) {
    // "own": arg was already moved into `storage_ptr`.
    auto& storage_ref = get_from_storage_ptr<StorageI>(storage_ptr);
    static_assert(
        std::is_same_v<
            typename Bs::storage_type,
            std::remove_reference_t<decltype(storage_ref)>>);
    // `SharedCleanupClosure=true` preserves the `after_cleanup_ref_` prefix of
    // the storage type.
    return storage_ref.template to_capture_ref</*shared*/ true>(priv);
  } else if constexpr (
      // "own": Move stored `as_capture()` into inner coro.
      is_instantiation_of_v<async_closure_inner_stored_arg, Bs> ||
      // `scheduleSelfClosure` / `scheduleScopeClosure` self-references.
      is_instantiation_of_v<async_closure_scope_self_ref_hack, Bs>) {
    return typename Bs::storage_type{priv, std::move(bs.bindWrapper_)};
  } else if constexpr (is_any_capture<Bs>) {
    // "pass": Move `capture<Ref>` into the inner coro.
    static_assert(std::is_reference_v<typename Bs::capture_type>);
    return std::move(bs);
  } else { // "regular": Non-`capture` binding.
    static_assert(is_instantiation_of_v<async_closure_regular_arg, Bs>);
    // We don't inspect `storage_type` here -- `detail/AsyncClosureBindings.h`
    // should have ensured that `bind_info_t` was in a default, no-op state.
    return std::move(bs).bindWrapper_.what_to_bind();
  }
}

// Eagerly construct -- but do not await -- an `async_closure`:
//   - Resolve bindings.
//   - Construct & store args for the user-supplied inner coro.
//   - For ensuring cleanup in the face of `bad_alloc`, pre-allocate the
//     outer task & `co_cleanup` tasks, if needed.
//   - Create the inner coro, passing it `capture` references, or -- if
//     there are no `co_cleanup` args and no outer coro -- quack-alike
//     owning wrappers.
//   - Marks the final user-facing task with the `safe_alias` that
//     describes the memory-safety of the closure's arguments.
//   - Returns the task inside a wrapper that statically checks the memory
//     safety of the return & `make_inner_coro` types when
//     `release_outer_coro()` is called.
//
// NB: Due to the "omit outer coro" optimization, `release_outer_coro()`
// will in some cases return a no-overhead wrapper around the coro returned
// by `make_inner_coro()`.
//
// Rationale: "Eager" is the only option matching user expectations, since
// regular coroutine args are bound eagerly too.  Implementation-wise, all
// `lang/Bindings.h` logic has to be resolved within the current statement,
// since the auxiliary reference-bearing objects aren't valid beyond that.
template <auto Cfg>
auto bind_captures_to_closure(auto&& make_inner_coro, auto safeties_and_binds) {
  auto& [arg_safeties, b_tup] = safeties_and_binds;

  // If some arguments require outer-coro storage, construct them in-place
  // on a `unique_ptr<tuple<>>`.  Without an outer coro, this stores `nullopt`.
  //
  // Rationale: Storing on-heap allows the outer coro own the arguments,
  // while simultaneously providing stable pointers to be passed into the
  // inner coro.
  //
  // Future: With a custom coro class, it should be possible to store the
  // argument tuple ON the coro frame, saving one allocation.
  auto storage_ptr = lite_tuple::apply(
      []<typename... SAs>(SAs... sas) {
        if constexpr (sizeof...(SAs) == 0) {
          return std::nullopt; // Signals "no outer closure" to the caller
        } else {
          // (2) Construct all the storage args in-place in one tuple.
          return std::make_unique<
              async_closure_storage<typename SAs::storage_type...>>(
              capture_private_t{}, std::move(sas.bindWrapper_)...);
        }
      },
      // (1) Collect the args that need storage on the outer coro.
      lite_tuple::apply(
          [](auto&... bs) {
            return lite_tuple::tuple_cat([]<typename B>(B& b) {
              if constexpr ( //
                  is_instantiation_of_v<async_closure_outer_stored_arg, B>) {
                return lite_tuple::tuple{std::move(b)};
              } else {
                return lite_tuple::tuple{};
              }
            }(bs)...);
          },
          b_tup));

  auto raw_inner_coro = lite_tuple::apply(
      [&]<typename... Bs>(Bs&... bs) {
        return [&]<size_t... ArgIs, size_t... StorageIs>(
                   std::index_sequence<ArgIs...>, vtag_t<StorageIs...>) {
          return make_inner_coro(
              // Unpack `Bs`, `ArgIs`, and `StorageIs` jointly
              [&]() -> decltype(auto) {
                if constexpr (Cfg.is_invoke_member && ArgIs == 0) {
                  // We have a `FOLLY_INVOKE_MEMBER`.  It accesses the
                  // member function via `.`, but this arg is expected to be
                  // `co_cleanup_capture<>` or `AsyncObjectPtr<>`, so we
                  // "magically" dereference it here.
                  //
                  // On safety: Below, we assert that it it made a
                  // `MemberTask<T>`, which `inner_rewrapped` will
                  // implicitly unwrap & mark with a higher safety level.
                  // `MemberTask` provides only a minimal safety
                  // attestation, namely (besides arg 1, the implicit object
                  // param), none of its args are taken by-reference.  This
                  // is fine, since for `OuterSafety`, we will have
                  // accounted for all the args' safety levels.
                  return *async_closure_bind_inner_coro_arg<StorageIs, Bs>(
                      capture_private_t{}, bs, storage_ptr);
                } else {
                  return async_closure_bind_inner_coro_arg<StorageIs, Bs>(
                      capture_private_t{}, bs, storage_ptr);
                }
              }()...);
        }(std::index_sequence_for<Bs...>{},
               cumsum_except_last< // `StorageIs` indexes into `storage_ptr`
                   (size_t)0,
                   is_instantiation_of_v<
                       async_closure_outer_stored_arg,
                       Bs>...>);
      },
      b_tup);

  // Compute the safety of the arguments being passed by the caller.
  constexpr safe_alias OuterSafety = Cfg.force_shared_cleanup // making NowTask
      ? safe_alias::unsafe
      : folly::least_safe_alias(decltype(arg_safeties){});
  // Also check that the coroutine function's signature looks safe.
  constexpr safe_alias InnerSafety =
      safe_task_traits<decltype(raw_inner_coro)>::arg_safety;

  // This converts `raw_inner_task` into a "task mover" that can be plumbed
  // down to, and used by, `async_closure_outer_coro()`.  We do 3 tricks here:
  //   - Wrap all tasks into a "mover" to handle immovables like `NowTask`.
  //   - For `ClosureTask`, we'll internally LIE about its safety to let it be
  //     `co_await`ed. Per below, that's OK thanks to `async_closure_wrap_coro`.
  //   - For `SafeTask` closures with the "no outer coro" optimization, we set
  //     the inner coro's safety to `OuterSafety`, for reasons explained below.
  auto inner_mover = [&]() {
    // The first branch is always taken for safe/movable `async_closure()`
    // invocations.  For `async_now_closure()`, this branch is taken iff the
    // inner coro is a `ClosureTask` or other `SafeTask`.
    if constexpr (InnerSafety >= safe_alias::unsafe_closure_internal) {
      // In the presence of stored `capture`s, `InnerSafety` (as measured by
      // `safe_alias_of_v` on the inner coro) is not what we want.  That's
      // because `Captures.h` marks owned captures as `unsafe_closure_internal`
      // to discourage them being moved out of the closure.  Instead, we set
      // safety based on `vtag_safety_of_async_closure_args` (`OuterSafety`).
      //
      // `ClosureTask` cannot be `co_await`ed, so clip to `>= min_arg_safety`.
      // This is OK since `async_closure_wrap_coro` will later enforce:
      //   OuterSafety >= closure_min_arg_safety
      constexpr auto newSafety =
          std::max(OuterSafety, safe_alias::closure_min_arg_safety);
      return mustAwaitImmediatelyUnsafeMover(
          std::move(raw_inner_coro).template withNewSafety<newSafety>());
    } else { // The "new safety" rewrite doesn't apply to unsafe tasks!
      return mustAwaitImmediatelyUnsafeMover(std::move(raw_inner_coro));
    }
  }();

  using ResultT = semi_await_result_t<decltype(std::move(inner_mover)())>;

  // We require this calling convention because the `is_invoke_member`
  // branch above dereferences the 1st arg.  That is only sensible if
  // we KNOW that the arg is the implicit object parameter, which
  // would not be true e.g.  if the user passed something like this:
  //   [](int num, auto me) { return me->addNumber(num); }
  static_assert(
      std::is_same_v<MemberTask<ResultT>, decltype(raw_inner_coro)> ==
          Cfg.is_invoke_member,
      "To use `MemberTask<>` coros with `async_closure`, you must pass "
      "the callable as `FOLLY_INVOKE_MEMBER(memberName)`, and pass the "
      "instance's `capture`/`AsyncObjectPtr`/... as the first argument.");

  auto outer_mover = [&] {
    if constexpr (std::is_same_v<decltype(storage_ptr), std::nullopt_t>) {
      // No outer coro is needed, so we can return the inner one.
      static_assert(
          !has_result_after_cleanup<ResultT>,
          "Cannot `co_return *after_cleanup()` without a cleanup arg");
      return std::move(inner_mover);
    } else {
      return mustAwaitImmediatelyUnsafeMover(
          async_closure_make_outer_coro<
              /*cancelTok*/ true,
              ResultT,
              OuterSafety>(
              async_closure_private_t{},
              std::move(inner_mover),
              std::move(storage_ptr)));
    }
  }();

  if constexpr (Cfg.force_shared_cleanup) {
    return toNowTask(std::move(outer_mover)());
  } else {
    return async_closure_wrap_coro{
        vtag<OuterSafety, InnerSafety>, std::move(outer_mover)};
  }
}

} // namespace folly::coro::detail

FOLLY_POP_WARNING
#endif
