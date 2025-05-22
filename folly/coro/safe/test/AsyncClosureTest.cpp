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

#include <folly/coro/GtestHelpers.h>
#include <folly/coro/Noexcept.h>
#include <folly/coro/Timeout.h>
#include <folly/coro/safe/AsyncClosure.h>
#include <folly/fibers/Semaphore.h>

#if FOLLY_HAS_IMMOVABLE_COROUTINES

using namespace folly;
using namespace folly::coro;
using namespace folly::bindings;
using namespace std::literals::chrono_literals;

CO_TEST(AsyncClosure, invalid_co_cleanup) {
  auto checkCleanup = []<typename T>(tag_t<T>) {
    return async_closure(capture_in_place<T>(), [](auto) -> ClosureTask<void> {
      co_return;
    });
  };

  struct ValidCleanup : NonCopyableNonMovable {
    AsNoexcept<Task<void>> co_cleanup(async_closure_private_t) { co_return; }
  };
  co_await checkCleanup(tag<ValidCleanup>);

  struct InvalidCleanupNonVoid : NonCopyableNonMovable {
    AsNoexcept<Task<int>> co_cleanup(async_closure_private_t) { co_return 1; }
  };
#if 0 // Manual test -- this uses `static_assert` for better UX.
  co_await checkCleanup(tag<InvalidCleanupNonVoid>);
#endif

  struct InvalidCleanupLacksNoexcept : NonCopyableNonMovable {
    Task<void> co_cleanup(async_closure_private_t) { co_return; }
  };
#if 0 // Manual test -- this uses `static_assert` for better UX.
  co_await checkCleanup(tag<InvalidCleanupLacksNoexcept>);
#endif

  struct InvalidCleanupIsMovable {
    AsNoexcept<Task<void>> co_cleanup(async_closure_private_t) { co_return; }
  };
#if 0 // Manual test -- this failure escapes `is_detected_v`.
  co_await checkCleanup(tag<InvalidCleanupIsMovable>);
#endif
}

static_assert(std::is_same_v<
              decltype(folly::coro::detail::cumsum_except_last<0, 2, 1, 3>),
              const vtag_t<0, 2, 3>>);

ClosureTask<int> intTask(int x) {
  co_return x;
}
struct StatelessIntCallable {
  ClosureTask<int> operator()(int x) { co_return x; }
};
struct StatelessGenericCallable {
  ClosureTask<int> operator()(auto x) { co_return x; }
};

// We can't directly test `async_closure*` for unsafe inputs, since that
// would trigger `static_assert`s in `release_outer_coro()`.  Instead, test
// `is_safe()` which verifies the same conditions.
template <bool ForceOuter>
void checkSafety() {
  constexpr int x = 42;

  auto safeWrap = [](auto fn, auto&& bargs) {
    return folly::coro::detail::
        async_closure_impl<ForceOuter, /*EmitNowTask*/ false>(
            std::move(bargs), std::move(fn));
  };

  // Check safe usage, with various levels of arg safety.
  // Covers: fn ptrs, plain & generic lambdas, callable & generic callables.
  safe_alias_constant<safe_alias::maybe_value> kValue;
  auto checkIsSafe = [&](auto arg_safety, auto fn, auto bargs) {
    auto s = safeWrap(std::move(fn), std::move(bargs));
    static_assert(s.is_safe());
    static_assert(
        folly::coro::detail::safe_task_traits<
            decltype(std::move(s).release_outer_coro())>::arg_safety ==
        arg_safety.value);
  };

  checkIsSafe(kValue, intTask, bound_args{5});
  checkIsSafe(kValue, StatelessIntCallable{}, bound_args{5});
  checkIsSafe(kValue, StatelessGenericCallable{}, bound_args{5});
  checkIsSafe(kValue, []() -> ClosureTask<int> { co_return 5; }, bound_args{});
  checkIsSafe(kValue, []() -> ClosureTask<void> { co_return; }, bound_args{});
  checkIsSafe(
      kValue, [](int x) -> ClosureTask<int> { co_return x; }, bound_args{5});
  checkIsSafe(
      kValue, [](auto) -> ClosureTask<void> { co_return; }, bound_args{5});
  checkIsSafe(
      safe_alias_constant<safe_alias::co_cleanup_safe_ref>{},
      [](auto) -> ClosureTask<void> { co_return; },
      bound_args{manual_safe_ref<safe_alias::co_cleanup_safe_ref>(x)});
  checkIsSafe(
      safe_alias_constant<safe_alias::after_cleanup_ref>{},
      [](auto) -> ClosureTask<void> { co_return; },
      bound_args{manual_safe_ref<safe_alias::after_cleanup_ref>(x)});

  auto checkIsUnsafe = [&](auto fn, auto bargs) {
    auto s = safeWrap(std::move(fn), std::move(bargs));
    static_assert(!s.is_safe());
  };
  // Only `SafeTask` is allowed as the inner coro.
  checkIsUnsafe([]() -> Task<int> { co_return 5; }, bound_args{});
  checkIsUnsafe([]() -> Task<void> { co_return; }, bound_args{});
  checkIsUnsafe([](int x) -> Task<int> { co_return x; }, bound_args{5});
  checkIsUnsafe([](auto) -> Task<void> { co_return; }, bound_args{5});
  // Don't allow passing in `unsafe*` args externally.
  checkIsUnsafe(
      [](auto) -> ClosureTask<void> { co_return; },
      bound_args{manual_safe_ref<safe_alias::unsafe_closure_internal>(x)});
}

TEST(AsyncClosure, safetyNoOuter) {
  checkSafety</*force outer*/ false>();
}
TEST(AsyncClosure, safety) {
  checkSafety</*force outer*/ true>();
}

inline constexpr async_closure_config ForceOuter{.force_outer_coro = true};
inline constexpr async_closure_config NoForceOuter{.force_outer_coro = false};

// Checks that `async_closure` returns the `SafeTask` we expect.
template <typename ExpectedT, async_closure_config Cfg = NoForceOuter>
constexpr auto asyncClosureCheckType(auto fn, auto bargs) {
  auto t = async_closure<Cfg>(
      // Actually, safe because `bargs` is by-value
      folly::bindings::ext::bound_args_unsafe_move::from(std::move(bargs)),
      std::move(fn));
  static_assert(std::is_same_v<decltype(t), ExpectedT>);
  return std::move(t);
}

template <async_closure_config Cfg>
Task<void> checkNoArgs() {
  auto res = co_await asyncClosureCheckType<ValueTask<int>, Cfg>(
      []() -> ClosureTask<int> { co_return 7; }, bound_args{});
  EXPECT_EQ(7, res);
}

CO_TEST(AsyncClosure, noArgsNoOuter) {
  co_await checkNoArgs<NoForceOuter>();
}
CO_TEST(AsyncClosure, noArgs) {
  co_await checkNoArgs<ForceOuter>();
}

namespace {
static bool ran_returnsVoid;
}

template <async_closure_config Cfg>
Task<void> checkReturnsVoid() {
  ran_returnsVoid = false;
  co_await asyncClosureCheckType<ValueTask<void>, Cfg>(
      []() -> ClosureTask<void> {
        ran_returnsVoid = true;
        co_return;
      },
      bound_args{});
  EXPECT_TRUE(ran_returnsVoid);
}

CO_TEST(AsyncClosure, returnsVoidNoOuter) {
  co_await checkReturnsVoid<NoForceOuter>();
}
CO_TEST(AsyncClosure, returnsVoid) {
  co_await checkReturnsVoid<ForceOuter>();
}

template <async_closure_config Cfg>
Task<void> checkPlainArgs() {
  int thirtySix = 36; // test passing l-values
  auto res = co_await asyncClosureCheckType<ValueTask<int>, Cfg>(
      [](int x, auto yPtr, const auto z) -> ClosureTask<int> {
        ++x;
        int r = x + *yPtr + z;
        yPtr.reset();
        // Plain args have plain types
        static_assert(std::is_same_v<std::unique_ptr<int>, decltype(yPtr)>);
        co_return r;
      },
      bound_args{thirtySix, std::make_unique<int>(1200), 100});
  EXPECT_EQ(1337, res);
}

CO_TEST(AsyncClosure, plainArgsNoOuter) {
  co_await checkPlainArgs<NoForceOuter>();
}
CO_TEST(AsyncClosure, plainArgsOuter) {
  co_await checkPlainArgs<ForceOuter>();
}

ClosureTask<std::string> funcTemplate(auto hi) {
  *hi += "de-and-seek";
  co_return std::move(*hi);
}

CO_TEST(AsyncClosure, callFuncTemplate) {
  auto res = co_await asyncClosureCheckType<ValueTask<std::string>>(
      // As of 2024, C++ lacks an "overload set" type, and thus can't
      // directly deduce `funcTemplate` (see P3360R0 pr P3312R0).
      FOLLY_INVOKE_QUAL(funcTemplate),
      bound_args{capture_in_place<std::string>("hi")});
  EXPECT_EQ("hide-and-seek", res);
}

// With `as_capture()`, immovable objects get auto-promoted to
// `capture_heap<>` iff the closure's outer coro is elided.
struct ImmovableString : private NonCopyableNonMovable {
  explicit ImmovableString(std::string s) : s_(std::move(s)) {}
  std::string s_;
};

// When needed, closure callbacks can have explicit & readable type signatures.
// Unfortunately, the signature depends on whether the closure has an outer
// coro wrapping the inner one.
ClosureTask<std::string> funcNoOuter(capture_heap<ImmovableString> hi) {
  hi->s_ += "de-and-seek";
  co_return std::move(hi->s_);
}
ClosureTask<std::string> funcWithOuter(capture<ImmovableString&> hi) {
  hi->s_ += "de-and-seek";
  co_return std::move(hi->s_);
}

CO_TEST(AsyncClosure, callFunctionNoOuter) {
  auto res = co_await asyncClosureCheckType<ValueTask<std::string>>(
      funcNoOuter, bound_args{capture_in_place<ImmovableString>("hi")});
  EXPECT_EQ("hide-and-seek", res);
}

CO_TEST(AsyncClosure, callFunctionWithOuter) {
  auto res = co_await asyncClosureCheckType<ValueTask<std::string>, ForceOuter>(
      funcWithOuter, bound_args{capture_in_place<ImmovableString>("hi")});
  EXPECT_EQ("hide-and-seek", res);
}

CO_TEST(AsyncClosure, simpleCancellation) {
  EXPECT_THROW(
      co_await timeout(
          async_closure(
              bound_args{},
              []() -> ClosureTask<void> {
                folly::fibers::Semaphore stuck{0}; // a cancellable baton
                co_await stuck.co_wait();
              }),
          200ms),
      folly::FutureTimeout);
}

struct InPlaceOnly : folly::NonCopyableNonMovable {
  explicit InPlaceOnly(bool* made, int n) : n_(n) {
    if (made) {
      *made = true;
    }
  }
  int n_;
};

void assertArgConst(auto& arg) {
  static_assert(std::is_const_v<std::remove_reference_t<decltype(*arg)>>);
  static_assert(
      std::is_const_v<std::remove_pointer_t<decltype(arg.operator->())>>);
}

template <async_closure_config Cfg>
Task<void> checkInPlaceArgs() {
  bool made = false;
  auto res = co_await asyncClosureCheckType<ValueTask<int>, Cfg>(
      [](int a, auto b, auto c, auto d) -> ClosureTask<int> {
        static_assert(
            std::is_same_v<
                decltype(b),
                std::conditional_t<
                    Cfg.force_outer_coro,
                    capture<int&>,
                    capture<int>>>);
        *b += 100;
        static_assert(
            std::is_same_v<
                decltype(c),
                std::conditional_t<
                    Cfg.force_outer_coro,
                    capture<const InPlaceOnly&>,
                    capture_heap<const InPlaceOnly>>>);
        assertArgConst(c); // `const` underlying type
        assertArgConst(d); // marked `constant`
        co_return a + *b + c->n_ + *d;
      },
      bound_args{
          30, // a
          // Test both const and non-const `AsyncOuterClosurePtr`s
          as_capture(1000), // b
          capture_in_place<const InPlaceOnly>(&made, 7), // c
          as_capture(constant(200))}); // d
  EXPECT_EQ(1337, res);
  EXPECT_TRUE(made);
}

CO_TEST(AsyncClosure, inPlaceArgsNoOuter) {
  co_await checkInPlaceArgs<NoForceOuter>();
}
CO_TEST(AsyncClosure, inPlaceArgs) {
  co_await checkInPlaceArgs<ForceOuter>();
}

// Tests that, with an outer coro, the user can specify `const auto`
// args on the inner task, and they work as expected.
//
// IIUC this can't work generically for the "no outer coro" scenario, since
// args need to be copied or moved into the inner coro, and non-copyable,
// `const` classes are not movable.  In `checkInPlaceArgs()`, you can see
// the workaround of passing a `const` (or equivalenly `constant()`) arg.
CO_TEST(AsyncClosureTest, constAutoArgWithOuterCoro) {
  bool made = false;
  auto res = co_await asyncClosureCheckType<ValueTask<int>, ForceOuter>(
      [](const auto a) -> ClosureTask<int> {
        static_assert(
            std::is_same_v<decltype(a), const capture<const InPlaceOnly&>>);
        assertArgConst(a);
        co_return a->n_;
      },
      bound_args{as_capture(
          make_in_place<
// Manual test: When set to 0, this should fail to compile because the `const
// auto` above requires (via `FOLLY_MOVABLE_AND_DEEP_CONST_LREF_COPYABLE`) the
// inner type to be `const`.
#if 1
              const
#endif
              InPlaceOnly>(&made, 7))});
  EXPECT_EQ(7, res);
  EXPECT_TRUE(made);
}

// A simple test pair showing the "move-in" vs "by-ref" behavior of the "no
// outer coro" optimization. The `nestedRefs*` tests elaborate on this.
CO_TEST(AsyncClosure, noOuterCoroGetsCaptureValue) {
  co_await async_closure(as_capture(1337), [](auto n) -> ClosureTask<void> {
    static_assert(std::is_same_v<decltype(n), capture<int>>);
    co_return;
  });
}
CO_TEST(AsyncClosure, outerCoroGetsCaptureRef) {
  co_await async_closure<ForceOuter>(
      as_capture(1337), [](auto n) -> ClosureTask<void> {
        static_assert(std::is_same_v<decltype(n), capture<int&>>);
        co_return;
      });
}

CO_TEST(AsyncClosure, nestedRefsWithOuterCoro) {
  auto res = co_await asyncClosureCheckType<ValueTask<int>, ForceOuter>(
      [](auto x, const auto y, const auto z) -> ClosureTask<int> {
        static_assert(std::is_same_v<decltype(x), capture<int&>>);
        static_assert(
            std::is_same_v<
                decltype(y),
                const capture<const std::unique_ptr<int>&>>);
        assertArgConst(y);
        static_assert(
            std::is_same_v<
                decltype(z),
                const capture_indirect<const std::unique_ptr<int>&>>);
        *x += 100;
        co_await asyncClosureCheckType<CoCleanupSafeTask<void>>(
            [](auto x2, auto y2, auto z2) -> ClosureTask<void> {
              static_assert(std::is_same_v<decltype(x2), capture<int&>>);
              static_assert(
                  std::is_same_v<
                      decltype(y2),
                      capture<const std::unique_ptr<int>&>>);
              assertArgConst(y2);
              static_assert(
                  std::is_same_v<
                      decltype(z2),
                      capture_indirect<const std::unique_ptr<int>&>>);
              *x2 += 100; // ref remains non-const -- C++ arg semantics
              co_return;
            },
            bound_args{x, y, z});
        // Can also pass `capture<Ref>`s into a bare SafeTask.
        co_await [](auto x3, auto y3, auto z3) -> CoCleanupSafeTask<void> {
          static_assert(std::is_same_v<decltype(x3), capture<int&>>);
          static_assert(
              std::is_same_v<
                  decltype(y3),
                  capture<const std::unique_ptr<int>&>>);
          assertArgConst(y3);
          static_assert(
              std::is_same_v<
                  decltype(z3),
                  capture_indirect<const std::unique_ptr<int>&>>);
          *x3 += 100; // ref remains non-const -- C++ arg semantics
          co_return;
        }(x, y, z);
        co_return *x + **y + *z;
      },
      bound_args{
          as_capture(
              make_in_place<int>(1000), constant(std::make_unique<int>(23))),
          as_capture_indirect(constant(std::make_unique<int>(14)))});
  EXPECT_EQ(1337, res);
}

// Like `ImmovableString`, this helps us detect when the outer coro was elided
struct ImmovableInt : private NonCopyableNonMovable {
  explicit ImmovableInt(int n) : n_(std::move(n)) {}
  int n_;
};

// We want this to be as similar as possible to `nestedRefsWithOuterCoro` --
// after all, "no outer coro" is supposed to be a "mostly transparent"
// optimization. Therefore, the main differences are:
//   - Split `x` into `w` and `x` to cover both heap and non-heap behaviors.
//   - `capture`s move into the inner coro, and therefore cannot:
//     * Write `const auto y` or `const auto z`, which would need a copy ctor
//     * Use `constant()` around `std::make_unique()` (prevents move).
//   - Correspondingly, we have to drop the `const`ness asserts.
//   - To pass `capture<Val>` into a bare `SafeTask`, we now have to
//     explicitly declare the its argument types, to use the implicit
//     conversion from `capture<Val>` to `capture<Val&>`.
CO_TEST(AsyncClosure, nestedRefsWithoutOuterCoro) {
  auto res = co_await asyncClosureCheckType<ValueTask<int>, NoForceOuter>(
      [](auto w, auto x, auto y, auto z) -> ClosureTask<int> {
        // Only the immovable type gets promoted to `capture_heap`.
        static_assert(std::is_same_v<decltype(w), capture<int>>);
        static_assert(std::is_same_v<decltype(x), capture_heap<ImmovableInt>>);
        static_assert(
            std::is_same_v<
                decltype(z),
                capture_indirect<std::unique_ptr<const int>>>);
        x->n_ += 100;
        co_await asyncClosureCheckType<CoCleanupSafeTask<void>>(
            [](auto w2, auto y2, auto z2) -> ClosureTask<void> {
              static_assert(std::is_same_v<decltype(w2), capture<int&>>);
              static_assert(
                  std::is_same_v<decltype(y2), capture<std::unique_ptr<int>&>>);
              static_assert(
                  std::is_same_v<
                      decltype(z2),
                      capture_indirect<std::unique_ptr<const int>&>>);
              *w2 += 100; // ref remains non-const -- C++ arg semantics
              co_return;
            },
            bound_args{w, y, z});
        // Can pass implicitly converted `capture<Ref>`s into a SafeTask
        co_await
            [](capture<ImmovableInt&> x3,
               capture<std::unique_ptr<int>&> y3,
               capture_indirect<std::unique_ptr<const int>&>)
                -> CoCleanupSafeTask<void> {
              x3->n_ += 50;
              *(*y3) += 50;
              co_return;
            }(x, y, z);
        co_return *w + x->n_ + **y + *z;
      },
      bound_args{
          as_capture(
              make_in_place<int>(700),
              make_in_place<ImmovableInt>(300),
              std::make_unique<int>(23)),
          // Can't use `constant()` here because we can't move a `const
          // unique_ptr`.
          as_capture_indirect(std::make_unique<const int>(14))});
  EXPECT_EQ(1337, res);
}

struct ErrorObliviousHasCleanup : NonCopyableNonMovable {
  explicit ErrorObliviousHasCleanup(int* p) : cleanBits_(p) {}
  int* cleanBits_;
  AsNoexcept<Task<void>> co_cleanup(async_closure_private_t) {
    *cleanBits_ += 3;
    co_return;
  }
};

CO_TEST(AsyncClosure, errorObliviousCleanup) {
  int cleanBits = 0;
  co_await async_closure(
      capture_in_place<ErrorObliviousHasCleanup>(&cleanBits),
      [](auto) -> ClosureTask<void> { co_return; });
  EXPECT_EQ(3, cleanBits);
}

struct HasCleanup : NonCopyableNonMovable {
  explicit HasCleanup(auto* p) : optCleanupErrPtr_(p) {}
  std::optional<exception_wrapper>* optCleanupErrPtr_;
  // If the closure (not other cleanups!) exited with an exception, each
  // `co_cleanup` gets to see it.
  AsNoexcept<Task<void>> co_cleanup(
      async_closure_private_t, const exception_wrapper* ew) {
    *optCleanupErrPtr_ = *ew;
    co_return;
  }
};

CO_TEST(AsyncClosure, cleanupAfterSuccess) {
  std::optional<exception_wrapper> optCleanErr;
  co_await async_closure(
      capture_in_place<HasCleanup>(&optCleanErr),
      [](auto) -> ClosureTask<void> { co_return; });
  EXPECT_FALSE(optCleanErr->has_exception_ptr());
}

CO_TEST(AsyncClosure, cleanupAfterError) {
  struct MagicError : std::exception {
    explicit MagicError(int m) : magic_(m) {}
    int magic_;
  };

  std::optional<exception_wrapper> optCleanErr;
  auto res = co_await co_awaitTry(async_closure(
      as_capture(make_in_place<HasCleanup>(&optCleanErr)),
      [](auto) -> ClosureTask<void> {
        co_yield folly::coro::co_error{MagicError{111}};
      }));
  EXPECT_EQ(111, optCleanErr->get_exception<MagicError>()->magic_);
  EXPECT_EQ(111, res.tryGetExceptionObject<MagicError>()->magic_);
}

struct CustomDerefCleanupProxy : NonCopyableNonMovable {
  explicit CustomDerefCleanupProxy(int y) : y_(y) {}
  auto operator->() { return static_cast<CustomDerefCleanupProxy*>(this); }
  int y_;
};

struct CustomDerefCleanup : HasCleanup {
  explicit CustomDerefCleanup(auto* p) : HasCleanup(p) {}
  using KindT = folly::coro::ext::capture_proxy_kind;
  template <KindT Kind, folly::coro::ext::const_or_not<CustomDerefCleanup> T>
  friend auto capture_proxy(folly::coro::ext::capture_proxy_tag<Kind>, T&) {
    if constexpr (Kind == KindT::lval_ref) {
      return CustomDerefCleanupProxy{101 + 1000 * std::is_const_v<T>};
    } else if constexpr (Kind == KindT::lval_ptr) {
      return CustomDerefCleanupProxy{202 + 1000 * std::is_const_v<T>};
    } else if constexpr (Kind == KindT::rval_ref) {
      return CustomDerefCleanupProxy{303 + 1000 * std::is_const_v<T>};
    } else if constexpr (Kind == KindT::rval_ptr) {
      return CustomDerefCleanupProxy{404 + 1000 * std::is_const_v<T>};
    } else {
      static_assert(false);
    }
  }
};

template <typename CleanupT>
Task<void> check_pass_cleanup_arg_to_subclosure(auto validate_ref) {
  std::optional<exception_wrapper> optCleanErr;
  co_await async_closure(
      bound_args{capture_in_place<CleanupT>(&optCleanErr), validate_ref},
      [](auto c, auto validate_ref2) -> ClosureTask<void> {
        validate_ref2(c);
        static_assert(
            std::is_same_v<decltype(c), co_cleanup_capture<CleanupT&>>);
        co_await async_closure(
            bound_args{c, validate_ref2},
            [](auto c2, auto validate_ref3) -> ClosureTask<void> {
              validate_ref3(c2);
              static_assert(
                  std::is_same_v<decltype(c2), co_cleanup_capture<CleanupT&>>);
              co_return;
            });
      });
  EXPECT_FALSE(optCleanErr->has_exception_ptr());
}

CO_TEST(AsyncClosure, passCleanupArgToSubclosure) {
  co_await check_pass_cleanup_arg_to_subclosure<HasCleanup>([](auto&) {});
}
// Check that the "custom dereferencing" code doesn't break the automatic
// passing of `capture` refs to child closures.
CO_TEST(AsyncClosure, passCustomDerefCleanupArgToSubclosure) {
  co_await check_pass_cleanup_arg_to_subclosure<CustomDerefCleanup>(
      [](auto& c) {
        EXPECT_EQ(101, (*c).y_);
        EXPECT_EQ(202, c->y_);
        EXPECT_EQ(404, std::move(c)->y_);

        EXPECT_EQ(1101, (*std::as_const(c)).y_);
        EXPECT_EQ(1202, std::as_const(c)->y_);
        EXPECT_EQ(1404, std::move(std::as_const(c))->y_);
      });
}

TEST(AsyncClosure, nonSafeTaskIsNotAwaited) {
  bool awaited = false;
  auto lambda = [&]() -> Task<void> {
    awaited = true;
    co_return;
  };
  // We can't `release_outer_coro()` on either since they have a
  // `static_assert` -- but `checkIsUnsafe` above checks the logic.
  folly::coro::detail::async_closure_impl<
      /*ForceOuter*/ false,
      /*EmitNowTask*/ false>(bound_args{}, lambda);
  folly::coro::detail::async_closure_impl<
      /*ForceOuter*/ true,
      /*EmitNowTask*/ false>(bound_args{}, lambda);
  EXPECT_FALSE(awaited);
}

// This test explores the anti-pattern of `async_closure` calling
// `FOLLY_INVOKE_MEMBER(operator())` on a lambda.  The behavior is analogous to
// `co_invoke`, in that it gives you a task that owns both the `lambda` and its
// arguments.  It also has the usual `async_closure` safety checks on the
// arguments.  While tempting, it would be a BAD IDEA to add this syntax sugar:
//   invoke_async_closure(
//       bound_args{arg1, arg2},
//       [&z](auto a1, auto a2) -> MemberTask<T> {...})
// Why not add `invoke_async_closure` as above?  Simply put, this is a
// "less-safe" pattern, in that it makes it easy for users to create `SafeTask`
// instances that hide unsafe reference captures.  Prefer to tell people to use
// `async_now_closure(bound_args{a1, a2}, ...)` with `Task`/`NowTask` lambdas.
CO_TEST(AsyncClosure, memberTaskLambda) {
  int z = 1300; // Goal: ASAN failures if the lambda is destroyed
  auto lambda = [&z](auto x, auto y) -> MemberTask<int> {
    co_return x + *y + z;
  };
  // BAD: To be coherent with regular `folly/coro/safe` safety guarantees,
  // the `t` below should be emitted as an immovable `NowTask`.  Otherwise,
  // one can imagine lifetime errors involving the `&z` capture.
  //
  // Unfortunately, we can't fix this in C++20.  This is an instance of
  // "aliasing hidden in structures" `SafeAlias.h` problem -- there's no way
  // for us to know that the lambda contains unsafe members on the inside.
  //
  // Won't compile without `std::move`, the assert is:
  //   ... has to be an r-value, so that the closure can take ownership ...
  // Won't compile without `force_outer_coro`, the assert is:
  //   ... you want the `MemberTask` closure to own the object ...
  auto t = async_closure<ForceOuter>(
      bound_args{as_capture(std::move(lambda)), 30, as_capture(7)},
      FOLLY_INVOKE_MEMBER(operator()));
  EXPECT_EQ(1337, co_await std::move(t));
  EXPECT_EQ(
      1337,
      co_await async_closure<ForceOuter>(
          bound_args{
              as_capture([&z](auto x, auto y) -> MemberTask<int> {
                co_return x + *y + z;
              }),
              30,
              as_capture(7)},
          FOLLY_INVOKE_MEMBER(operator())));
}

struct HasMemberTask {
  int z = 1300; // Goal: ASAN failures if the class is destroyed
  MemberTask<int> task(auto x, auto y) { co_return x + *y + z; }
};

CO_TEST(AsyncClosure, memberTask) {
  // First, examples of a "bound" member closure that actually owns the object:
  EXPECT_EQ(
      1337,
      co_await async_closure<ForceOuter>(
          bound_args{as_capture(HasMemberTask{}), 30, as_capture(7)},
          FOLLY_INVOKE_MEMBER(task)));
  EXPECT_EQ(
      1337, // Syntax sugar: implicit `as_capture` for member's object parameter
      co_await async_closure<ForceOuter>(
          bound_args{HasMemberTask{}, 30, as_capture(7)},
          FOLLY_INVOKE_MEMBER(task)));
  EXPECT_EQ(
      1337, // Same, but showing that `make_in_place` still works
      co_await async_closure<ForceOuter>(
          bound_args{make_in_place<HasMemberTask>(), 30, as_capture(7)},
          FOLLY_INVOKE_MEMBER(task)));
  HasMemberTask hmt;
  EXPECT_EQ(
      1337, // Wouldn't compile without either `std::move` or `folly::copy`.
      co_await async_closure<ForceOuter>(
          bound_args{std::move(hmt), 30, as_capture(7)},
          FOLLY_INVOKE_MEMBER(task)));

  // Second, call a member coro on an existing `capture<HasMemberTask>`.
  EXPECT_EQ(
      1337,
      co_await async_closure<ForceOuter>(
          as_capture(HasMemberTask{}), [](auto mt) -> ClosureTask<int> {
            co_return co_await async_closure(
                bound_args{mt, 30, as_capture(7)}, FOLLY_INVOKE_MEMBER(task));
          }));
}

// Check that `async_now_closure` returns `NowTask<int>` & return the task.
NowTask<int> intAsyncNowClosure(auto&& bargs, auto&& fn) {
  return async_now_closure(
      folly::bindings::ext::bound_args_unsafe_move::from(std::move(bargs)),
      std::move(fn));
}

template <typename T>
NowTask<void> check_now_closure_no_outer_coro() {
  int b1 = 300, c = 30, d = 7;
  // The coro take raw references & use lambda captures
  int res = co_await intAsyncNowClosure(
      bound_args{as_capture(1000), b1}, [&c, d](auto a, int& b2) -> T {
        static_assert(
            std::is_same_v< // No ref upgrade
                after_cleanup_capture<int>,
                decltype(a)>);
        co_return *a + b2 + c + d;
      });
  EXPECT_EQ(1337, res);
}

// The plumbing for an outer-coro closure is different, so test it too.
template <typename T>
NowTask<void> check_now_closure_with_outer_coro() {
  int cleanBits = 128;
  int res = co_await intAsyncNowClosure(
      capture_in_place<ErrorObliviousHasCleanup>(&cleanBits),
      [](auto c) -> T { co_return *c->cleanBits_; });
  EXPECT_EQ(128, res);
}

CO_TEST(AsyncClosure, nowClosure) {
  co_await check_now_closure_no_outer_coro<Task<int>>();
  co_await check_now_closure_no_outer_coro<NowTask<int>>();

  co_await check_now_closure_with_outer_coro<Task<int>>();
  co_await check_now_closure_with_outer_coro<NowTask<int>>();

  // Going from `ClosureTask` / `MemberTask` to `NowTask` is rare, but it
  // does work.  Of course, passing raw refs is not possible in this case.

  co_await check_now_closure_with_outer_coro<ClosureTask<int>>();

  int closureRes = co_await intAsyncNowClosure(
      as_capture(7), [](auto n) -> ClosureTask<int> {
        static_assert(
            std::is_same_v< // No ref upgrade
                after_cleanup_capture<int>,
                decltype(n)>);
        co_return *n;
      });
  EXPECT_EQ(7, closureRes);

  HasMemberTask hmt;
  auto memberRes = co_await intAsyncNowClosure(
      bound_args{&hmt, 7, as_capture(30)}, FOLLY_INVOKE_MEMBER(task));
  EXPECT_EQ(1337, memberRes);
}

CO_TEST(AsyncClosure, nowClosureCoCleanup) {
  std::optional<exception_wrapper> optCleanErr;
  int res = co_await async_now_closure(
      bound_args{capture_in_place<HasCleanup>(&optCleanErr), as_capture(1300)},
      [](auto cleanup, auto n) -> Task<int> {
        static_assert(
            std::is_same_v<co_cleanup_capture<HasCleanup&>, decltype(cleanup)>);
        static_assert(
            std::is_same_v< // No ref upgrade
                after_cleanup_capture<int&>,
                decltype(n)>);
        co_return *n + 37;
      });
  EXPECT_EQ(1337, res);
  EXPECT_TRUE(optCleanErr.has_value());
}

// Records construction order, asserts that (1) cleanup & destruction happen in
// the opposite order, and (2) all cleanups complete before any dtors.
struct OrderTracker : NonCopyableNonMovable {
  int myN_;
  int& nRef_;
  int myCleanupN_;
  int& cleanupNRef_;

  explicit OrderTracker(int& n, int& cleanupN)
      : myN_(++n), nRef_(n), myCleanupN_(++cleanupN), cleanupNRef_(cleanupN) {}

  AsNoexcept<Task<void>> co_cleanup(async_closure_private_t) {
    EXPECT_EQ(myCleanupN_, cleanupNRef_--);
    co_return;
  }
  ~OrderTracker() {
    // Our contract is that all cleanups complete before any capture is
    // destroyed.  This is required for `AfterCleanup.h` to be useful.
    EXPECT_EQ(1000, cleanupNRef_);
    EXPECT_EQ(myN_, nRef_--);
  }
};

CO_TEST(AsyncClosure, ctorCleanupDtorOrdering) {
  int n = 0, cleanupN = 1000;
  co_await async_closure(
      bound_args{
          capture_in_place<OrderTracker>(n, cleanupN),
          capture_in_place<OrderTracker>(n, cleanupN),
          capture_in_place<OrderTracker>(n, cleanupN),
          capture_in_place<OrderTracker>(n, cleanupN)},
      [](auto c1, auto c2, auto c3, auto c4) -> ClosureTask<void> {
        EXPECT_EQ(4, c1->nRef_);
        EXPECT_EQ(1, c1->myN_);
        EXPECT_EQ(2, c2->myN_);
        EXPECT_EQ(3, c3->myN_);
        EXPECT_EQ(4, c4->myN_);

        EXPECT_EQ(1004, c1->cleanupNRef_);
        EXPECT_EQ(1001, c1->myCleanupN_);
        EXPECT_EQ(1002, c2->myCleanupN_);
        EXPECT_EQ(1003, c3->myCleanupN_);
        EXPECT_EQ(1004, c4->myCleanupN_);

        co_return;
      });
}

#endif
