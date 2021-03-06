/*
 * Copyright 2014-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <thread>

#include <folly/Optional.h>
#include <folly/executors/InlineExecutor.h>
#include <folly/executors/QueuedImmediateExecutor.h>
#include <folly/futures/detail/Core.h>
#include <folly/synchronization/Baton.h>

#ifndef FOLLY_FUTURE_USING_FIBER
#if FOLLY_MOBILE || defined(__APPLE__)
#define FOLLY_FUTURE_USING_FIBER 0
#else
#define FOLLY_FUTURE_USING_FIBER 1
#include <folly/fibers/Baton.h>
#endif
#endif

namespace folly {

class Timekeeper;

namespace futures {
namespace detail {
#if FOLLY_FUTURE_USING_FIBER
typedef folly::fibers::Baton FutureBatonType;
#else
typedef folly::Baton<> FutureBatonType;
#endif
} // namespace detail
} // namespace futures

namespace detail {
std::shared_ptr<Timekeeper> getTimekeeperSingleton();
} // namespace detail

namespace futures {
namespace detail {
//  Guarantees that the stored functor is destructed before the stored promise
//  may be fulfilled. Assumes the stored functor to be noexcept-destructible.
template <typename T, typename F>
class CoreCallbackState {
  using DF = _t<std::decay<F>>;

 public:
  CoreCallbackState(Promise<T>&& promise, F&& func) noexcept(
      noexcept(DF(std::declval<F&&>())))
      : func_(std::forward<F>(func)), promise_(std::move(promise)) {
    assert(before_barrier());
  }

  CoreCallbackState(CoreCallbackState&& that) noexcept(
      noexcept(DF(std::declval<F&&>()))) {
    if (that.before_barrier()) {
      new (&func_) DF(std::forward<F>(that.func_));
      promise_ = that.stealPromise();
    }
  }

  CoreCallbackState& operator=(CoreCallbackState&&) = delete;

  ~CoreCallbackState() {
    if (before_barrier()) {
      stealPromise();
    }
  }

  template <typename... Args>
  auto invoke(Args&&... args) noexcept(
      noexcept(std::declval<F&&>()(std::declval<Args&&>()...))) {
    assert(before_barrier());
    return std::forward<F>(func_)(std::forward<Args>(args)...);
  }

  template <typename... Args>
  auto tryInvoke(Args&&... args) noexcept {
    return makeTryWith([&] { return invoke(std::forward<Args>(args)...); });
  }

  void setTry(Try<T>&& t) {
    stealPromise().setTry(std::move(t));
  }

  void setException(exception_wrapper&& ew) {
    stealPromise().setException(std::move(ew));
  }

  Promise<T> stealPromise() noexcept {
    assert(before_barrier());
    func_.~DF();
    return std::move(promise_);
  }

 private:
  bool before_barrier() const noexcept {
    return !promise_.isFulfilled();
  }

  union {
    DF func_;
  };
  Promise<T> promise_{Promise<T>::makeEmpty()};
};

template <typename T, typename F>
auto makeCoreCallbackState(Promise<T>&& p, F&& f) noexcept(
    noexcept(CoreCallbackState<T, F>(
        std::declval<Promise<T>&&>(),
        std::declval<F&&>()))) {
  return CoreCallbackState<T, F>(std::move(p), std::forward<F>(f));
}

template <typename T, typename R, typename... Args>
auto makeCoreCallbackState(Promise<T>&& p, R (&f)(Args...)) noexcept {
  return CoreCallbackState<T, R (*)(Args...)>(std::move(p), &f);
}

template <class T>
FutureBase<T>::FutureBase(SemiFuture<T>&& other) noexcept : core_(other.core_) {
  other.core_ = nullptr;
}

template <class T>
FutureBase<T>::FutureBase(Future<T>&& other) noexcept : core_(other.core_) {
  other.core_ = nullptr;
}

template <class T>
template <class T2, typename>
FutureBase<T>::FutureBase(T2&& val)
    : core_(CoreType::make(Try<T>(std::forward<T2>(val)))) {}

template <class T>
template <typename T2>
FutureBase<T>::FutureBase(
    typename std::enable_if<std::is_same<Unit, T2>::value>::type*)
    : core_(CoreType::make(Try<T>(T()))) {}

template <class T>
template <
    class... Args,
    typename std::enable_if<std::is_constructible<T, Args&&...>::value, int>::
        type>
FutureBase<T>::FutureBase(in_place_t, Args&&... args)
    : core_(CoreType::make(in_place, std::forward<Args>(args)...)) {}

template <class T>
void FutureBase<T>::assign(FutureBase<T>&& other) noexcept {
  detach();
  core_ = exchange(other.core_, nullptr);
}

template <class T>
FutureBase<T>::~FutureBase() {
  detach();
}

template <class T>
T& FutureBase<T>::value() & {
  return result().value();
}

template <class T>
T const& FutureBase<T>::value() const& {
  return result().value();
}

template <class T>
T&& FutureBase<T>::value() && {
  return std::move(result().value());
}

template <class T>
T const&& FutureBase<T>::value() const&& {
  return std::move(result().value());
}

template <class T>
Try<T>& FutureBase<T>::result() & {
  return getCoreTryChecked();
}

template <class T>
Try<T> const& FutureBase<T>::result() const& {
  return getCoreTryChecked();
}

template <class T>
Try<T>&& FutureBase<T>::result() && {
  return std::move(getCoreTryChecked());
}

template <class T>
Try<T> const&& FutureBase<T>::result() const&& {
  return std::move(getCoreTryChecked());
}

template <class T>
bool FutureBase<T>::isReady() const {
  return getCore().hasResult();
}

template <class T>
bool FutureBase<T>::hasValue() const {
  return result().hasValue();
}

template <class T>
bool FutureBase<T>::hasException() const {
  return result().hasException();
}

template <class T>
void FutureBase<T>::detach() {
  if (core_) {
    core_->detachFuture();
    core_ = nullptr;
  }
}

template <class T>
void FutureBase<T>::throwIfInvalid() const {
  if (!core_) {
    throw_exception<FutureInvalid>();
  }
}

template <class T>
Optional<Try<T>> FutureBase<T>::poll() {
  auto& core = getCore();
  return core.hasResult() ? Optional<Try<T>>(std::move(core.getTry()))
                          : Optional<Try<T>>();
}

template <class T>
void FutureBase<T>::raise(exception_wrapper exception) {
  getCore().raise(std::move(exception));
}

template <class T>
template <class F>
void FutureBase<T>::setCallback_(F&& func) {
  getCore().setCallback(std::forward<F>(func));
}

template <class T>
FutureBase<T>::FutureBase(futures::detail::EmptyConstruct) noexcept
    : core_(nullptr) {}

// MSVC 2017 Update 7 released with a bug that causes issues expanding to an
// empty parameter pack when invoking a templated member function. It should
// be fixed for MSVC 2017 Update 8.
// TODO: Remove.
namespace detail_msvc_15_7_workaround {
template <bool isTry, typename State, typename T>
decltype(auto) invoke(State& state, Try<T>& /* t */) {
  return state.invoke();
}
template <bool isTry, typename State, typename T, typename Arg>
decltype(auto) invoke(State& state, Try<T>& t) {
  return state.invoke(t.template get<isTry, Arg>());
}
template <bool isTry, typename State, typename T>
decltype(auto) tryInvoke(State& state, Try<T>& /* t */) {
  return state.tryInvoke();
}
template <bool isTry, typename State, typename T, typename Arg>
decltype(auto) tryInvoke(State& state, Try<T>& t) {
  return state.tryInvoke(t.template get<isTry, Arg>());
}
} // namespace detail_msvc_15_7_workaround

// then

// Variant: returns a value
// e.g. f.then([](Try<T>&& t){ return t.value(); });
template <class T>
template <typename F, typename R, bool isTry, typename... Args>
typename std::enable_if<!R::ReturnsFuture::value, typename R::Return>::type
FutureBase<T>::thenImplementation(
    F&& func,
    futures::detail::argResult<isTry, F, Args...>) {
  static_assert(sizeof...(Args) <= 1, "Then must take zero/one argument");
  typedef typename R::ReturnsFuture::Inner B;

  Promise<B> p;
  p.core_->setInterruptHandlerNoLock(this->getCore().getInterruptHandler());

  // grab the Future now before we lose our handle on the Promise
  auto sf = p.getSemiFuture();
  sf.setExecutor(this->getExecutor());
  auto f = Future<B>(sf.core_);
  sf.core_ = nullptr;

  /* This is a bit tricky.

     We can't just close over *this in case this Future gets moved. So we
     make a new dummy Future. We could figure out something more
     sophisticated that avoids making a new Future object when it can, as an
     optimization. But this is correct.

     core_ can't be moved, it is explicitly disallowed (as is copying). But
     if there's ever a reason to allow it, this is one place that makes that
     assumption and would need to be fixed. We use a standard shared pointer
     for core_ (by copying it in), which means in essence obj holds a shared
     pointer to itself.  But this shouldn't leak because Promise will not
     outlive the continuation, because Promise will setException() with a
     broken Promise if it is destructed before completed. We could use a
     weak pointer but it would have to be converted to a shared pointer when
     func is executed (because the Future returned by func may possibly
     persist beyond the callback, if it gets moved), and so it is an
     optimization to just make it shared from the get-go.

     Two subtle but important points about this design. futures::detail::Core
     has no back pointers to Future or Promise, so if Future or Promise get
     moved (and they will be moved in performant code) we don't have to do
     anything fancy. And because we store the continuation in the
     futures::detail::Core, not in the Future, we can execute the continuation
     even after the Future has gone out of scope. This is an intentional design
     decision. It is likely we will want to be able to cancel a continuation
     in some circumstances, but I think it should be explicit not implicit
     in the destruction of the Future used to create it.
     */
  this->setCallback_(
      [state = futures::detail::makeCoreCallbackState(
           std::move(p), std::forward<F>(func))](Try<T>&& t) mutable {
        if (!isTry && t.hasException()) {
          state.setException(std::move(t.exception()));
        } else {
          state.setTry(makeTryWith([&] {
            return detail_msvc_15_7_workaround::
                invoke<isTry, decltype(state), T, Args...>(state, t);
          }));
        }
      });
  return f;
}

// Pass through a simple future as it needs no deferral adaptation
template <class T>
Future<T> chainExecutor(Executor*, Future<T>&& f) {
  return std::move(f);
}

// Correctly chain a SemiFuture for deferral
template <class T>
Future<T> chainExecutor(Executor* e, SemiFuture<T>&& f) {
  if (!e) {
    e = &InlineExecutor::instance();
  }
  return std::move(f).via(e);
}

// Variant: returns a Future
// e.g. f.then([](T&& t){ return makeFuture<T>(t); });
template <class T>
template <typename F, typename R, bool isTry, typename... Args>
typename std::enable_if<R::ReturnsFuture::value, typename R::Return>::type
FutureBase<T>::thenImplementation(
    F&& func,
    futures::detail::argResult<isTry, F, Args...>) {
  static_assert(sizeof...(Args) <= 1, "Then must take zero/one argument");
  typedef typename R::ReturnsFuture::Inner B;

  Promise<B> p;
  p.core_->setInterruptHandlerNoLock(this->getCore().getInterruptHandler());

  // grab the Future now before we lose our handle on the Promise
  auto sf = p.getSemiFuture();
  auto* e = this->getExecutor();
  sf.setExecutor(e);
  auto f = Future<B>(sf.core_);
  sf.core_ = nullptr;

  this->setCallback_([state = futures::detail::makeCoreCallbackState(
                          std::move(p), std::forward<F>(func))](
                         Try<T>&& t) mutable {
    if (!isTry && t.hasException()) {
      state.setException(std::move(t.exception()));
    } else {
      // Ensure that if function returned a SemiFuture we correctly chain
      // potential deferral.
      auto tf2 = detail_msvc_15_7_workaround::
          tryInvoke<isTry, decltype(state), T, Args...>(state, t);
      if (tf2.hasException()) {
        state.setException(std::move(tf2.exception()));
      } else {
        auto statePromise = state.stealPromise();
        auto tf3 =
            chainExecutor(statePromise.core_->getExecutor(), *std::move(tf2));
        tf3.setCallback_([p2 = std::move(statePromise)](Try<B>&& b) mutable {
          p2.setTry(std::move(b));
        });
      }
    }
  });

  return f;
}

template <class T>
template <typename E>
SemiFuture<T>
FutureBase<T>::withinImplementation(Duration dur, E e, Timekeeper* tk) {
  struct Context {
    explicit Context(E ex) : exception(std::move(ex)) {}
    E exception;
    Future<Unit> thisFuture;
    Promise<T> promise;
    std::atomic<bool> token{false};
  };

  std::shared_ptr<Timekeeper> tks;
  if (LIKELY(!tk)) {
    tks = folly::detail::getTimekeeperSingleton();
    tk = tks.get();
  }

  if (UNLIKELY(!tk)) {
    return makeSemiFuture<T>(FutureNoTimekeeper());
  }

  auto ctx = std::make_shared<Context>(std::move(e));

  auto f = [ctx](Try<T>&& t) {
    if (!ctx->token.exchange(true)) {
      ctx->promise.setTry(std::move(t));
    }
  };
  using R = futures::detail::callableResult<T, decltype(f)>;
  ctx->thisFuture = this->template thenImplementation<decltype(f), R>(
      std::move(f), typename R::Arg());

  // Properly propagate interrupt values through futures chained after within()
  ctx->promise.setInterruptHandler(
      [weakCtx = to_weak_ptr(ctx)](const exception_wrapper& ex) {
        if (auto lockedCtx = weakCtx.lock()) {
          lockedCtx->thisFuture.raise(ex);
        }
      });

  // Have time keeper use a weak ptr to hold ctx,
  // so that ctx can be deallocated as soon as the future job finished.
  tk->after(dur).then([weakCtx = to_weak_ptr(ctx)](Try<Unit>&& t) mutable {
    auto lockedCtx = weakCtx.lock();
    if (!lockedCtx) {
      // ctx already released. "this" completed first, cancel "after"
      return;
    }
    // "after" completed first, cancel "this"
    lockedCtx->thisFuture.raise(FutureTimeout());
    if (!lockedCtx->token.exchange(true)) {
      if (t.hasException()) {
        lockedCtx->promise.setException(std::move(t.exception()));
      } else {
        lockedCtx->promise.setException(std::move(lockedCtx->exception));
      }
    }
  });

  return ctx->promise.getSemiFuture();
}

/**
 * Defer work until executor is actively boosted.
 *
 * NOTE: that this executor is a private implementation detail belonging to the
 * Folly Futures library and not intended to be used elsewhere. It is designed
 * specifically for the use case of deferring work on a SemiFuture. It is NOT
 * thread safe. Please do not use for any other purpose without great care.
 */
class DeferredExecutor final : public Executor {
 public:
  void add(Func func) override {
    auto state = state_.load(std::memory_order_acquire);
    if (state == State::HAS_FUNCTION) {
      // This means we are inside runAndDestroy, just run the function inline
      func();
      return;
    }

    func_ = std::move(func);
    std::shared_ptr<FutureBatonType> baton;
    do {
      if (state == State::HAS_EXECUTOR) {
        state_.store(State::HAS_FUNCTION, std::memory_order_release);
        executor_->add([this] { this->runAndDestroy(); });
        return;
      }
      if (state == State::DETACHED) {
        // Function destructor may trigger more functions to be added to the
        // Executor. They should be run inline.
        state = State::HAS_FUNCTION;
        func_ = nullptr;
        delete this;
        return;
      }
      if (state == State::HAS_BATON) {
        baton = baton_.copy();
      }
      assert(state == State::EMPTY || state == State::HAS_BATON);
    } while (!state_.compare_exchange_weak(
        state,
        State::HAS_FUNCTION,
        std::memory_order_release,
        std::memory_order_acquire));

    // After compare_exchange_weak is complete, we can no longer use this
    // object since it may be destroyed from another thread.
    if (baton) {
      baton->post();
    }
  }

  void setExecutor(folly::Executor* executor) {
    executor_ = executor;
    auto state = state_.load(std::memory_order_acquire);
    do {
      if (state == State::HAS_FUNCTION) {
        executor_->add([this] { this->runAndDestroy(); });
        return;
      }
      assert(state == State::EMPTY);
    } while (!state_.compare_exchange_weak(
        state,
        State::HAS_EXECUTOR,
        std::memory_order_release,
        std::memory_order_acquire));
  }

  void runAndDestroy() {
    assert(state_.load(std::memory_order_relaxed) == State::HAS_FUNCTION);
    func_();
    delete this;
  }

  void detach() {
    auto state = state_.load(std::memory_order_acquire);
    do {
      if (state == State::HAS_FUNCTION) {
        // Function destructor may trigger more functions to be added to the
        // Executor. They should be run inline.
        func_ = nullptr;
        delete this;
        return;
      }

      assert(state == State::EMPTY);
    } while (!state_.compare_exchange_weak(
        state,
        State::DETACHED,
        std::memory_order_release,
        std::memory_order_acquire));
  }

  void wait() {
    auto state = state_.load(std::memory_order_acquire);
    auto baton = std::make_shared<FutureBatonType>();
    baton_ = baton;
    do {
      if (state == State::HAS_FUNCTION) {
        return;
      }
      assert(state == State::EMPTY);
    } while (!state_.compare_exchange_weak(
        state,
        State::HAS_BATON,
        std::memory_order_release,
        std::memory_order_acquire));

    baton->wait();

    assert(state_.load(std::memory_order_relaxed) == State::HAS_FUNCTION);
  }

  bool wait(Duration duration) {
    auto state = state_.load(std::memory_order_acquire);
    auto baton = std::make_shared<FutureBatonType>();
    baton_ = baton;
    do {
      if (state == State::HAS_FUNCTION) {
        return true;
      }
      assert(state == State::EMPTY);
    } while (!state_.compare_exchange_weak(
        state,
        State::HAS_BATON,
        std::memory_order_release,
        std::memory_order_acquire));

    if (baton->try_wait_for(duration)) {
      assert(state_.load(std::memory_order_relaxed) == State::HAS_FUNCTION);
      return true;
    }

    state = state_.load(std::memory_order_acquire);
    do {
      if (state == State::HAS_FUNCTION) {
        return true;
      }
      assert(state == State::HAS_BATON);
    } while (!state_.compare_exchange_weak(
        state,
        State::EMPTY,
        std::memory_order_release,
        std::memory_order_acquire));
    return false;
  }

 private:
  enum class State {
    EMPTY,
    HAS_FUNCTION,
    HAS_EXECUTOR,
    HAS_BATON,
    DETACHED,
  };
  std::atomic<State> state_{State::EMPTY};
  Func func_;
  Executor* executor_;
  folly::Synchronized<std::shared_ptr<FutureBatonType>> baton_;
};

// Vector-like structure to play with window,
// which otherwise expects a vector of size `times`,
// which would be expensive with large `times` sizes.
struct WindowFakeVector {
  using iterator = std::vector<size_t>::iterator;

  WindowFakeVector(size_t size) : size_(size) {}

  size_t operator[](const size_t index) const {
    return index;
  }
  size_t size() const {
    return size_;
  }

 private:
  size_t size_;
};
} // namespace detail
} // namespace futures

template <class T>
SemiFuture<typename std::decay<T>::type> makeSemiFuture(T&& t) {
  return makeSemiFuture(Try<typename std::decay<T>::type>(std::forward<T>(t)));
}

// makeSemiFutureWith(SemiFuture<T>()) -> SemiFuture<T>
template <class F>
typename std::
    enable_if<isSemiFuture<invoke_result_t<F>>::value, invoke_result_t<F>>::type
    makeSemiFutureWith(F&& func) {
  using InnerType = typename isSemiFuture<invoke_result_t<F>>::Inner;
  try {
    return std::forward<F>(func)();
  } catch (std::exception& e) {
    return makeSemiFuture<InnerType>(
        exception_wrapper(std::current_exception(), e));
  } catch (...) {
    return makeSemiFuture<InnerType>(
        exception_wrapper(std::current_exception()));
  }
}

// makeSemiFutureWith(T()) -> SemiFuture<T>
// makeSemiFutureWith(void()) -> SemiFuture<Unit>
template <class F>
typename std::enable_if<
    !(isSemiFuture<invoke_result_t<F>>::value),
    SemiFuture<lift_unit_t<invoke_result_t<F>>>>::type
makeSemiFutureWith(F&& func) {
  using LiftedResult = lift_unit_t<invoke_result_t<F>>;
  return makeSemiFuture<LiftedResult>(
      makeTryWith([&func]() mutable { return std::forward<F>(func)(); }));
}

template <class T>
SemiFuture<T> makeSemiFuture(std::exception_ptr const& e) {
  return makeSemiFuture(Try<T>(e));
}

template <class T>
SemiFuture<T> makeSemiFuture(exception_wrapper ew) {
  return makeSemiFuture(Try<T>(std::move(ew)));
}

template <class T, class E>
typename std::
    enable_if<std::is_base_of<std::exception, E>::value, SemiFuture<T>>::type
    makeSemiFuture(E const& e) {
  return makeSemiFuture(Try<T>(make_exception_wrapper<E>(e)));
}

template <class T>
SemiFuture<T> makeSemiFuture(Try<T>&& t) {
  return SemiFuture<T>(SemiFuture<T>::CoreType::make(std::move(t)));
}

// This must be defined after the constructors to avoid a bug in MSVC
// https://connect.microsoft.com/VisualStudio/feedback/details/3142777/out-of-line-constructor-definition-after-implicit-reference-causes-incorrect-c2244
inline SemiFuture<Unit> makeSemiFuture() {
  return makeSemiFuture(Unit{});
}

template <class T>
SemiFuture<T> SemiFuture<T>::makeEmpty() {
  return SemiFuture<T>(futures::detail::EmptyConstruct{});
}

template <class T>
typename SemiFuture<T>::DeferredExecutor* SemiFuture<T>::getDeferredExecutor()
    const {
  if (auto executor = this->getExecutor()) {
    assert(dynamic_cast<DeferredExecutor*>(executor) != nullptr);
    return static_cast<DeferredExecutor*>(executor);
  }
  return nullptr;
}

template <class T>
void SemiFuture<T>::releaseDeferredExecutor(corePtr core) {
  if (!core) {
    return;
  }
  if (auto executor = core->getExecutor()) {
    assert(dynamic_cast<DeferredExecutor*>(executor) != nullptr);
    static_cast<DeferredExecutor*>(executor)->detach();
    core->setExecutor(nullptr);
  }
}

template <class T>
SemiFuture<T>::~SemiFuture() {
  releaseDeferredExecutor(this->core_);
}

template <class T>
SemiFuture<T>::SemiFuture(SemiFuture<T>&& other) noexcept
    : futures::detail::FutureBase<T>(std::move(other)) {}

template <class T>
SemiFuture<T>::SemiFuture(Future<T>&& other) noexcept
    : futures::detail::FutureBase<T>(std::move(other)) {
  // SemiFuture should not have an executor on construction
  if (this->core_) {
    this->setExecutor(nullptr);
  }
}

template <class T>
SemiFuture<T>& SemiFuture<T>::operator=(SemiFuture<T>&& other) noexcept {
  releaseDeferredExecutor(this->core_);
  this->assign(std::move(other));
  return *this;
}

template <class T>
SemiFuture<T>& SemiFuture<T>::operator=(Future<T>&& other) noexcept {
  releaseDeferredExecutor(this->core_);
  this->assign(std::move(other));
  // SemiFuture should not have an executor on construction
  if (this->core_) {
    this->setExecutor(nullptr);
  }
  return *this;
}

template <class T>
Future<T> SemiFuture<T>::via(
    Executor::KeepAlive<> executor,
    int8_t priority) && {
  if (!executor) {
    throw_exception<FutureNoExecutor>();
  }

  if (auto deferredExecutor = getDeferredExecutor()) {
    deferredExecutor->setExecutor(executor.get());
  }

  auto newFuture = Future<T>(this->core_);
  this->core_ = nullptr;
  newFuture.setExecutor(std::move(executor), priority);

  return newFuture;
}

template <class T>
Future<T> SemiFuture<T>::via(Executor* executor, int8_t priority) && {
  return std::move(*this).via(getKeepAliveToken(executor), priority);
}

template <class T>
Future<T> SemiFuture<T>::toUnsafeFuture() && {
  return std::move(*this).via(&InlineExecutor::instance());
}

template <class T>
template <typename F>
SemiFuture<typename futures::detail::tryCallableResult<T, F>::value_type>
SemiFuture<T>::defer(F&& func) && {
  DeferredExecutor* deferredExecutor = getDeferredExecutor();
  if (!deferredExecutor) {
    deferredExecutor = new DeferredExecutor();
    this->setExecutor(deferredExecutor);
  }

  auto sf = Future<T>(this->core_).then(std::forward<F>(func)).semi();
  this->core_ = nullptr;
  // Carry deferred executor through chain as constructor from Future will
  // nullify it
  sf.setExecutor(deferredExecutor);
  return sf;
}

template <class T>
template <typename F>
SemiFuture<typename futures::detail::valueCallableResult<T, F>::value_type>
SemiFuture<T>::deferValue(F&& func) && {
  return std::move(*this).defer([f = std::forward<F>(func)](
                                    folly::Try<T>&& t) mutable {
    return std::forward<F>(f)(
        t.template get<
            false,
            typename futures::detail::valueCallableResult<T, F>::FirstArg>());
  });
}

template <class T>
template <class ExceptionType, class F>
SemiFuture<T> SemiFuture<T>::deferError(F&& func) && {
  return std::move(*this).defer(
      [func = std::forward<F>(func)](Try<T>&& t) mutable {
        if (auto e = t.template tryGetExceptionObject<ExceptionType>()) {
          return makeSemiFutureWith(
              [&]() mutable { return std::forward<F>(func)(*e); });
        } else {
          return makeSemiFuture<T>(std::move(t));
        }
      });
}

template <class T>
template <class F>
SemiFuture<T> SemiFuture<T>::deferError(F&& func) && {
  return std::move(*this).defer(
      [func = std::forward<F>(func)](Try<T> t) mutable {
        if (t.hasException()) {
          return makeSemiFutureWith([&]() mutable {
            return std::forward<F>(func)(std::move(t.exception()));
          });
        } else {
          return makeSemiFuture<T>(std::move(t));
        }
      });
}

template <typename T>
SemiFuture<T> SemiFuture<T>::delayed(Duration dur, Timekeeper* tk) && {
  return collectAllSemiFuture(*this, futures::sleep(dur, tk))
      .toUnsafeFuture()
      .then([](std::tuple<Try<T>, Try<Unit>> tup) {
        Try<T>& t = std::get<0>(tup);
        return makeFuture<T>(std::move(t));
      });
}

template <class T>
Future<T> Future<T>::makeEmpty() {
  return Future<T>(futures::detail::EmptyConstruct{});
}

template <class T>
Future<T>::Future(Future<T>&& other) noexcept
    : futures::detail::FutureBase<T>(std::move(other)) {}

template <class T>
Future<T>& Future<T>::operator=(Future<T>&& other) noexcept {
  this->assign(std::move(other));
  return *this;
}

template <class T>
template <
    class T2,
    typename std::enable_if<
        !std::is_same<T, typename std::decay<T2>::type>::value &&
            std::is_constructible<T, T2&&>::value &&
            std::is_convertible<T2&&, T>::value,
        int>::type>
Future<T>::Future(Future<T2>&& other)
    : Future(std::move(other).then([](T2&& v) { return T(std::move(v)); })) {}

template <class T>
template <
    class T2,
    typename std::enable_if<
        !std::is_same<T, typename std::decay<T2>::type>::value &&
            std::is_constructible<T, T2&&>::value &&
            !std::is_convertible<T2&&, T>::value,
        int>::type>
Future<T>::Future(Future<T2>&& other)
    : Future(std::move(other).then([](T2&& v) { return T(std::move(v)); })) {}

template <class T>
template <
    class T2,
    typename std::enable_if<
        !std::is_same<T, typename std::decay<T2>::type>::value &&
            std::is_constructible<T, T2&&>::value,
        int>::type>
Future<T>& Future<T>::operator=(Future<T2>&& other) {
  return operator=(
      std::move(other).then([](T2&& v) { return T(std::move(v)); }));
}

// unwrap

template <class T>
template <class F>
typename std::
    enable_if<isFuture<F>::value, Future<typename isFuture<T>::Inner>>::type
    Future<T>::unwrap() {
  return then([](Future<typename isFuture<T>::Inner> internal_future) {
    return internal_future;
  });
}

template <class T>
Future<T> Future<T>::via(Executor::KeepAlive<> executor, int8_t priority) && {
  this->setExecutor(std::move(executor), priority);

  auto newFuture = Future<T>(this->core_);
  this->core_ = nullptr;
  return newFuture;
}

template <class T>
Future<T> Future<T>::via(Executor* executor, int8_t priority) && {
  return std::move(*this).via(getKeepAliveToken(executor), priority);
}

template <class T>
Future<T> Future<T>::via(Executor::KeepAlive<> executor, int8_t priority) & {
  this->throwIfInvalid();
  Promise<T> p;
  auto sf = p.getSemiFuture();
  auto func = [p = std::move(p)](Try<T>&& t) mutable {
    p.setTry(std::move(t));
  };
  using R = futures::detail::callableResult<T, decltype(func)>;
  this->template thenImplementation<decltype(func), R>(
      std::move(func), typename R::Arg());
  // Construct future from semifuture manually because this may not have
  // an executor set due to legacy code. This means we can bypass the executor
  // check in SemiFuture::via
  auto f = Future<T>(sf.core_);
  sf.core_ = nullptr;
  return std::move(f).via(std::move(executor), priority);
}

template <class T>
Future<T> Future<T>::via(Executor* executor, int8_t priority) & {
  return via(getKeepAliveToken(executor), priority);
}

template <typename T>
template <typename R, typename Caller, typename... Args>
  Future<typename isFuture<R>::Inner>
Future<T>::then(R(Caller::*func)(Args...), Caller *instance) {
  typedef typename std::remove_cv<typename std::remove_reference<
      typename futures::detail::ArgType<Args...>::FirstArg>::type>::type
      FirstArg;

  return then([instance, func](Try<T>&& t){
    return (instance->*func)(t.template get<isTry<FirstArg>::value, Args>()...);
  });
}

template <class T>
template <typename F>
Future<typename futures::detail::tryCallableResult<T, F>::value_type>
Future<T>::thenTry(F&& func) && {
  return std::move(*this).then(std::forward<F>(func));
}

template <class T>
template <typename F>
Future<typename futures::detail::valueCallableResult<T, F>::value_type>
Future<T>::thenValue(F&& func) && {
  return std::move(*this).then([f = std::forward<F>(func)](
                                   folly::Try<T>&& t) mutable {
    return std::forward<F>(f)(
        t.template get<
            false,
            typename futures::detail::valueCallableResult<T, F>::FirstArg>());
  });
}

template <class T>
template <class ExceptionType, class F>
Future<T> Future<T>::thenError(F&& func) && {
  // Forward to onError but ensure that returned future carries the executor
  // Allow for applying to future with null executor while this is still
  // possible.
  auto* e = this->getExecutor();
  return onError([func = std::forward<F>(func)](ExceptionType& ex) mutable {
           return std::forward<F>(func)(ex);
         })
      .via(e ? e : &InlineExecutor::instance());
}

template <class T>
template <class F>
Future<T> Future<T>::thenError(F&& func) && {
  // Forward to onError but ensure that returned future carries the executor
  // Allow for applying to future with null executor while this is still
  // possible.
  auto* e = this->getExecutor();
  return onError([func = std::forward<F>(func)](
                     folly::exception_wrapper&& ex) mutable {
           return std::forward<F>(func)(std::move(ex));
         })
      .via(e ? e : &InlineExecutor::instance());
}

template <class T>
Future<Unit> Future<T>::then() {
  return then([]() {});
}

// onError where the callback returns T
template <class T>
template <class F>
typename std::enable_if<
    !is_invocable<F, exception_wrapper>::value &&
        !futures::detail::Extract<F>::ReturnsFuture::value,
    Future<T>>::type
Future<T>::onError(F&& func) {
  typedef std::remove_reference_t<
      typename futures::detail::Extract<F>::FirstArg>
      Exn;
  static_assert(
      std::is_same<typename futures::detail::Extract<F>::RawReturn, T>::value,
      "Return type of onError callback must be T or Future<T>");

  Promise<T> p;
  p.core_->setInterruptHandlerNoLock(this->getCore().getInterruptHandler());
  auto sf = p.getSemiFuture();

  this->setCallback_(
      [state = futures::detail::makeCoreCallbackState(
           std::move(p), std::forward<F>(func))](Try<T>&& t) mutable {
        if (auto e = t.template tryGetExceptionObject<Exn>()) {
          state.setTry(makeTryWith([&] { return state.invoke(*e); }));
        } else {
          state.setTry(std::move(t));
        }
      });

  // Allow for applying to future with null executor while this is still
  // possible.
  // TODO(T26801487): Should have an executor
  return std::move(sf).via(&InlineExecutor::instance());
}

// onError where the callback returns Future<T>
template <class T>
template <class F>
typename std::enable_if<
    !is_invocable<F, exception_wrapper>::value &&
        futures::detail::Extract<F>::ReturnsFuture::value,
    Future<T>>::type
Future<T>::onError(F&& func) {
  static_assert(
      std::is_same<typename futures::detail::Extract<F>::Return, Future<T>>::
          value,
      "Return type of onError callback must be T or Future<T>");
  typedef std::remove_reference_t<
      typename futures::detail::Extract<F>::FirstArg>
      Exn;

  Promise<T> p;
  auto sf = p.getSemiFuture();

  this->setCallback_(
      [state = futures::detail::makeCoreCallbackState(
           std::move(p), std::forward<F>(func))](Try<T>&& t) mutable {
        if (auto e = t.template tryGetExceptionObject<Exn>()) {
          auto tf2 = state.tryInvoke(*e);
          if (tf2.hasException()) {
            state.setException(std::move(tf2.exception()));
          } else {
            tf2->setCallback_([p = state.stealPromise()](Try<T> && t3) mutable {
              p.setTry(std::move(t3));
            });
          }
        } else {
          state.setTry(std::move(t));
        }
      });

  // Allow for applying to future with null executor while this is still
  // possible.
  // TODO(T26801487): Should have an executor
  return std::move(sf).via(&InlineExecutor::instance());
}

template <class T>
template <class F>
Future<T> Future<T>::ensure(F&& func) {
  return this->then([funcw = std::forward<F>(func)](Try<T>&& t) mutable {
    std::forward<F>(funcw)();
    return makeFuture(std::move(t));
  });
}

template <class T>
template <class F>
Future<T> Future<T>::onTimeout(Duration dur, F&& func, Timekeeper* tk) {
  return within(dur, tk).template thenError<FutureTimeout>(
      [funcw = std::forward<F>(func)](auto const&) mutable {
        return std::forward<F>(funcw)();
      });
}

template <class T>
template <class F>
typename std::enable_if<
    is_invocable<F, exception_wrapper>::value &&
        futures::detail::Extract<F>::ReturnsFuture::value,
    Future<T>>::type
Future<T>::onError(F&& func) {
  static_assert(
      std::is_same<typename futures::detail::Extract<F>::Return, Future<T>>::
          value,
      "Return type of onError callback must be T or Future<T>");

  Promise<T> p;
  auto sf = p.getSemiFuture();
  this->setCallback_(
      [state = futures::detail::makeCoreCallbackState(
           std::move(p), std::forward<F>(func))](Try<T> t) mutable {
        if (t.hasException()) {
          auto tf2 = state.tryInvoke(std::move(t.exception()));
          if (tf2.hasException()) {
            state.setException(std::move(tf2.exception()));
          } else {
            tf2->setCallback_([p = state.stealPromise()](Try<T> && t3) mutable {
              p.setTry(std::move(t3));
            });
          }
        } else {
          state.setTry(std::move(t));
        }
      });

  // Allow for applying to future with null executor while this is still
  // possible.
  // TODO(T26801487): Should have an executor
  return std::move(sf).via(&InlineExecutor::instance());
}

// onError(exception_wrapper) that returns T
template <class T>
template <class F>
typename std::enable_if<
    is_invocable<F, exception_wrapper>::value &&
        !futures::detail::Extract<F>::ReturnsFuture::value,
    Future<T>>::type
Future<T>::onError(F&& func) {
  static_assert(
      std::is_same<typename futures::detail::Extract<F>::Return, Future<T>>::
          value,
      "Return type of onError callback must be T or Future<T>");

  Promise<T> p;
  auto sf = p.getSemiFuture();
  this->setCallback_(
      [state = futures::detail::makeCoreCallbackState(
           std::move(p), std::forward<F>(func))](Try<T>&& t) mutable {
        if (t.hasException()) {
          state.setTry(makeTryWith(
              [&] { return state.invoke(std::move(t.exception())); }));
        } else {
          state.setTry(std::move(t));
        }
      });

  // Allow for applying to future with null executor while this is still
  // possible.
  // TODO(T26801487): Should have an executor
  return std::move(sf).via(&InlineExecutor::instance());
}

template <class Func>
auto via(Executor* x, Func&& func)
    -> Future<typename isFuture<decltype(std::declval<Func>()())>::Inner> {
  // TODO make this actually more performant. :-P #7260175
  return via(x).then(std::forward<Func>(func));
}

// makeFuture

template <class T>
Future<typename std::decay<T>::type> makeFuture(T&& t) {
  return makeFuture(Try<typename std::decay<T>::type>(std::forward<T>(t)));
}

inline Future<Unit> makeFuture() {
  return makeFuture(Unit{});
}

// makeFutureWith(Future<T>()) -> Future<T>
template <class F>
typename std::
    enable_if<isFuture<invoke_result_t<F>>::value, invoke_result_t<F>>::type
    makeFutureWith(F&& func) {
  using InnerType = typename isFuture<invoke_result_t<F>>::Inner;
  try {
    return std::forward<F>(func)();
  } catch (std::exception& e) {
    return makeFuture<InnerType>(
        exception_wrapper(std::current_exception(), e));
  } catch (...) {
    return makeFuture<InnerType>(exception_wrapper(std::current_exception()));
  }
}

// makeFutureWith(T()) -> Future<T>
// makeFutureWith(void()) -> Future<Unit>
template <class F>
typename std::enable_if<
    !(isFuture<invoke_result_t<F>>::value),
    Future<lift_unit_t<invoke_result_t<F>>>>::type
makeFutureWith(F&& func) {
  using LiftedResult = lift_unit_t<invoke_result_t<F>>;
  return makeFuture<LiftedResult>(
      makeTryWith([&func]() mutable { return std::forward<F>(func)(); }));
}

template <class T>
Future<T> makeFuture(std::exception_ptr const& e) {
  return makeFuture(Try<T>(e));
}

template <class T>
Future<T> makeFuture(exception_wrapper ew) {
  return makeFuture(Try<T>(std::move(ew)));
}

template <class T, class E>
typename std::enable_if<std::is_base_of<std::exception, E>::value,
                        Future<T>>::type
makeFuture(E const& e) {
  return makeFuture(Try<T>(make_exception_wrapper<E>(e)));
}

template <class T>
Future<T> makeFuture(Try<T>&& t) {
  return Future<T>(Future<T>::CoreType::make(std::move(t)));
}

// via
Future<Unit> via(Executor* executor, int8_t priority) {
  return makeFuture().via(executor, priority);
}

// mapSetCallback calls func(i, Try<T>) when every future completes

template <class T, class InputIterator, class F>
void mapSetCallback(InputIterator first, InputIterator last, F func) {
  for (size_t i = 0; first != last; ++first, ++i) {
    first->setCallback_([func, i](Try<T>&& t) {
      func(i, std::move(t));
    });
  }
}

// collectAll (variadic)

template <typename... Fs>
typename futures::detail::CollectAllVariadicContext<
    typename std::decay<Fs>::type::value_type...>::type
collectAllSemiFuture(Fs&&... fs) {
  auto ctx = std::make_shared<futures::detail::CollectAllVariadicContext<
      typename std::decay<Fs>::type::value_type...>>();
  futures::detail::collectVariadicHelper<
      futures::detail::CollectAllVariadicContext>(ctx, std::forward<Fs>(fs)...);
  return ctx->p.getSemiFuture();
}

template <typename... Fs>
Future<typename futures::detail::CollectAllVariadicContext<
    typename std::decay<Fs>::type::value_type...>::type::value_type>
collectAll(Fs&&... fs) {
  return collectAllSemiFuture(std::forward<Fs>(fs)...).toUnsafeFuture();
}

// collectAll (iterator)

template <class InputIterator>
SemiFuture<std::vector<
    Try<typename std::iterator_traits<InputIterator>::value_type::value_type>>>
collectAllSemiFuture(InputIterator first, InputIterator last) {
  typedef
    typename std::iterator_traits<InputIterator>::value_type::value_type T;

  struct CollectAllContext {
    CollectAllContext(size_t n) : results(n) {}
    ~CollectAllContext() {
      p.setValue(std::move(results));
    }
    Promise<std::vector<Try<T>>> p;
    std::vector<Try<T>> results;
  };

  auto ctx =
      std::make_shared<CollectAllContext>(size_t(std::distance(first, last)));
  mapSetCallback<T>(first, last, [ctx](size_t i, Try<T>&& t) {
    ctx->results[i] = std::move(t);
  });
  return ctx->p.getSemiFuture();
}

template <class InputIterator>
Future<std::vector<
    Try<typename std::iterator_traits<InputIterator>::value_type::value_type>>>
collectAll(InputIterator first, InputIterator last) {
  return collectAllSemiFuture(first, last).toUnsafeFuture();
}

// collect (iterator)

namespace futures {
namespace detail {

template <typename T>
struct CollectContext {
  struct Nothing {
    explicit Nothing(int /* n */) {}
  };

  using Result = typename std::conditional<
    std::is_void<T>::value,
    void,
    std::vector<T>>::type;

  using InternalResult = typename std::conditional<
    std::is_void<T>::value,
    Nothing,
    std::vector<Optional<T>>>::type;

  explicit CollectContext(size_t n) : result(n) {
    finalResult.reserve(n);
  }
  ~CollectContext() {
    if (!threw.exchange(true)) {
      // map Optional<T> -> T
      std::transform(result.begin(), result.end(),
                     std::back_inserter(finalResult),
                     [](Optional<T>& o) { return std::move(o.value()); });
      p.setValue(std::move(finalResult));
    }
  }
  void setPartialResult(size_t i, Try<T>& t) {
    result[i] = std::move(t.value());
  }
  Promise<Result> p;
  InternalResult result;
  Result finalResult;
  std::atomic<bool> threw {false};
};

} // namespace detail
} // namespace futures

// TODO(T26439406): Make return SemiFuture
template <class InputIterator>
Future<typename futures::detail::CollectContext<typename std::iterator_traits<
    InputIterator>::value_type::value_type>::Result>
collect(InputIterator first, InputIterator last) {
  typedef
    typename std::iterator_traits<InputIterator>::value_type::value_type T;

  auto ctx = std::make_shared<futures::detail::CollectContext<T>>(
      std::distance(first, last));
  mapSetCallback<T>(first, last, [ctx](size_t i, Try<T>&& t) {
    if (t.hasException()) {
       if (!ctx->threw.exchange(true)) {
         ctx->p.setException(std::move(t.exception()));
       }
     } else if (!ctx->threw) {
       ctx->setPartialResult(i, t);
     }
  });
  return ctx->p.getSemiFuture().via(&InlineExecutor::instance());
}

// collect (variadic)

// TODO(T26439406): Make return SemiFuture
template <typename... Fs>
typename futures::detail::CollectVariadicContext<
    typename std::decay<Fs>::type::value_type...>::type
collect(Fs&&... fs) {
  auto ctx = std::make_shared<futures::detail::CollectVariadicContext<
      typename std::decay<Fs>::type::value_type...>>();
  futures::detail::collectVariadicHelper<
      futures::detail::CollectVariadicContext>(ctx, std::forward<Fs>(fs)...);
  return ctx->p.getSemiFuture().via(&InlineExecutor::instance());
}

// collectAny (iterator)

// TODO(T26439406): Make return SemiFuture
template <class InputIterator>
Future<
  std::pair<size_t,
            Try<
              typename
              std::iterator_traits<InputIterator>::value_type::value_type>>>
collectAny(InputIterator first, InputIterator last) {
  typedef
    typename std::iterator_traits<InputIterator>::value_type::value_type T;

  struct CollectAnyContext {
    CollectAnyContext() {}
    Promise<std::pair<size_t, Try<T>>> p;
    std::atomic<bool> done {false};
  };

  auto ctx = std::make_shared<CollectAnyContext>();
  mapSetCallback<T>(first, last, [ctx](size_t i, Try<T>&& t) {
    if (!ctx->done.exchange(true)) {
      ctx->p.setValue(std::make_pair(i, std::move(t)));
    }
  });
  return ctx->p.getSemiFuture().via(&InlineExecutor::instance());
}

// collectAnyWithoutException (iterator)

// TODO(T26439406): Make return SemiFuture
template <class InputIterator>
Future<std::pair<
    size_t,
    typename std::iterator_traits<InputIterator>::value_type::value_type>>
collectAnyWithoutException(InputIterator first, InputIterator last) {
  typedef
      typename std::iterator_traits<InputIterator>::value_type::value_type T;

  struct CollectAnyWithoutExceptionContext {
    CollectAnyWithoutExceptionContext(){}
    Promise<std::pair<size_t, T>> p;
    std::atomic<bool> done{false};
    std::atomic<size_t> nFulfilled{0};
    size_t nTotal;
  };

  auto ctx = std::make_shared<CollectAnyWithoutExceptionContext>();
  ctx->nTotal = size_t(std::distance(first, last));

  mapSetCallback<T>(first, last, [ctx](size_t i, Try<T>&& t) {
    if (!t.hasException() && !ctx->done.exchange(true)) {
      ctx->p.setValue(std::make_pair(i, std::move(t.value())));
    } else if (++ctx->nFulfilled == ctx->nTotal) {
      ctx->p.setException(t.exception());
    }
  });
  return ctx->p.getSemiFuture().via(&InlineExecutor::instance());
}

// collectN (iterator)

template <class InputIterator>
SemiFuture<std::vector<std::pair<
    size_t,
    Try<typename std::iterator_traits<InputIterator>::value_type::value_type>>>>
collectN(InputIterator first, InputIterator last, size_t n) {
  using T =
      typename std::iterator_traits<InputIterator>::value_type::value_type;
  using V = std::vector<Optional<Try<T>>>;
  using Result = std::vector<std::pair<size_t, Try<T>>>;

  assert(n > 0);
  assert(std::distance(first, last) >= 0);

  struct CollectNContext {
    explicit CollectNContext(size_t numFutures) : v(numFutures) {}

    V v;
    std::atomic<size_t> completed = {0}; // # input futures completed
    std::atomic<size_t> stored = {0}; // # output values stored
    Promise<Result> p;

    void setPartialResult(size_t index, Try<T>&& t) {
      v[index] = std::move(t);
    }

    void complete() {
      Result result;
      result.reserve(completed.load());
      for (size_t i = 0; i < v.size(); ++i) {
        auto& entry = v[i];
        if (entry.hasValue()) {
          result.emplace_back(i, std::move(entry).value());
        }
      }
      p.setTry(Try<Result>(std::move(result)));
    }
  };

  auto numFutures = static_cast<size_t>(std::distance(first, last));
  auto ctx = std::make_shared<CollectNContext>(numFutures);

  if (numFutures < n) {
    ctx->p.setException(std::runtime_error("Not enough futures"));
  } else {
    // for each completed Future, increase count and add to vector, until we
    // have n completed futures at which point we fulfil our Promise with the
    // vector
    mapSetCallback<T>(first, last, [ctx, n](size_t i, Try<T>&& t) {
      // relaxed because this guards control but does not guard data
      auto const c = 1 + ctx->completed.fetch_add(1, std::memory_order_relaxed);
      if (c > n) {
        return;
      }
      ctx->setPartialResult(i, std::move(t));
      // release because the stored values in all threads must be visible below
      // acquire because no stored value is permitted to be fetched early
      auto const s = 1 + ctx->stored.fetch_add(1, std::memory_order_acq_rel);
      if (s < n) {
        return;
      }
      ctx->complete();
    });
  }

  return ctx->p.getSemiFuture();
}

// reduce (iterator)

template <class It, class T, class F>
Future<T> reduce(It first, It last, T&& initial, F&& func) {
  if (first == last) {
    return makeFuture(std::forward<T>(initial));
  }

  typedef typename std::iterator_traits<It>::value_type::value_type ItT;
  typedef typename std::
      conditional<is_invocable<F, T&&, Try<ItT>&&>::value, Try<ItT>, ItT>::type
          Arg;
  typedef isTry<Arg> IsTry;

  auto sfunc = std::make_shared<F>(std::move(func));

  auto f = first->then([minitial = std::forward<T>(initial),
                        sfunc](Try<ItT>&& head) mutable {
    return (*sfunc)(
        std::forward<T>(minitial), head.template get<IsTry::value, Arg&&>());
  });

  for (++first; first != last; ++first) {
    f = collectAllSemiFuture(f, *first).toUnsafeFuture().then(
        [sfunc](std::tuple<Try<T>, Try<ItT>>&& t) {
          return (*sfunc)(
              std::move(std::get<0>(t).value()),
              // Either return a ItT&& or a Try<ItT>&& depending
              // on the type of the argument of func.
              std::get<1>(t).template get<IsTry::value, Arg&&>());
        });
  }

  return f;
}

// window (collection)

template <class Collection, class F, class ItT, class Result>
std::vector<Future<Result>>
window(Collection input, F func, size_t n) {
  // Use global QueuedImmediateExecutor singleton to avoid stack overflow.
  auto executor = &QueuedImmediateExecutor::instance();
  return window(executor, std::move(input), std::move(func), n);
}

template <class F>
auto window(size_t times, F func, size_t n)
    -> std::vector<invoke_result_t<F, size_t>> {
  return window(futures::detail::WindowFakeVector(times), std::move(func), n);
}

template <class Collection, class F, class ItT, class Result>
std::vector<Future<Result>>
window(Executor* executor, Collection input, F func, size_t n) {
  struct WindowContext {
    WindowContext(Executor* executor_, Collection&& input_, F&& func_)
        : executor(executor_),
          input(std::move(input_)),
          promises(input.size()),
          func(std::move(func_)) {}
    std::atomic<size_t> i{0};
    Executor* executor;
    Collection input;
    std::vector<Promise<Result>> promises;
    F func;

    static void spawn(std::shared_ptr<WindowContext> ctx) {
      size_t i = ctx->i++;
      if (i < ctx->input.size()) {
        auto fut =
            makeFutureWith([&] { return ctx->func(std::move(ctx->input[i])); });
        fut.setCallback_([ctx = std::move(ctx), i](Try<Result>&& t) mutable {
          const auto executor_ = ctx->executor;
          executor_->add([ctx = std::move(ctx), i, t = std::move(t)]() mutable {
            ctx->promises[i].setTry(std::move(t));
            // Chain another future onto this one
            spawn(std::move(ctx));
          });
        });
      }
    }
  };

  auto max = std::min(n, input.size());

  auto ctx = std::make_shared<WindowContext>(
      executor, std::move(input), std::move(func));

  // Start the first n Futures
  for (size_t i = 0; i < max; ++i) {
    executor->add([ctx]() mutable { WindowContext::spawn(std::move(ctx)); });
  }

  std::vector<Future<Result>> futures;
  futures.reserve(ctx->promises.size());
  for (auto& promise : ctx->promises) {
    futures.emplace_back(promise.getSemiFuture().via(executor));
  }

  return futures;
}

// reduce

template <class T>
template <class I, class F>
Future<I> Future<T>::reduce(I&& initial, F&& func) {
  return then([minitial = std::forward<I>(initial),
               mfunc = std::forward<F>(func)](T&& vals) mutable {
    auto ret = std::move(minitial);
    for (auto& val : vals) {
      ret = mfunc(std::move(ret), std::move(val));
    }
    return ret;
  });
}

// unorderedReduce (iterator)

// TODO(T26439406): Make return SemiFuture
template <class It, class T, class F, class ItT, class Arg>
Future<T> unorderedReduce(It first, It last, T initial, F func) {
  if (first == last) {
    return makeFuture(std::move(initial));
  }

  typedef isTry<Arg> IsTry;

  struct UnorderedReduceContext {
    UnorderedReduceContext(T&& memo, F&& fn, size_t n)
        : lock_(), memo_(makeFuture<T>(std::move(memo))),
          func_(std::move(fn)), numThens_(0), numFutures_(n), promise_()
      {}

    static void fulfillWithValueOrFuture(Promise<T>&& p, T&& v) {
      p.setValue(std::move(v));
    }

    static void fulfillWithValueOrFuture(Promise<T>&& p, Future<T>&& f) {
      f.setCallback_(
          [p = std::move(p)](Try<T>&& t) mutable { p.setTry(std::move(t)); });
    }

    folly::MicroSpinLock lock_; // protects memo_ and numThens_
    Future<T> memo_;
    F func_;
    size_t numThens_; // how many Futures completed and called .then()
    size_t numFutures_; // how many Futures in total
    Promise<T> promise_;
  };

  auto ctx = std::make_shared<UnorderedReduceContext>(
    std::move(initial), std::move(func), std::distance(first, last));

  mapSetCallback<ItT>(
      first,
      last,
      [ctx](size_t /* i */, Try<ItT>&& t) {
        // Futures can be completed in any order, simultaneously.
        // To make this non-blocking, we create a new Future chain in
        // the order of completion to reduce the values.
        // The spinlock just protects chaining a new Future, not actually
        // executing the reduce, which should be really fast.
        Promise<T> p;
        auto f = p.getFuture();
        {
          folly::MSLGuard lock(ctx->lock_);
          f = exchange(ctx->memo_, std::move(f));
          if (++ctx->numThens_ == ctx->numFutures_) {
            // After reducing the value of the last Future, fulfill the Promise
            ctx->memo_.setCallback_(
                [ctx](Try<T>&& t2) { ctx->promise_.setValue(std::move(t2)); });
          }
        }
        f.setCallback_([ctx, mp = std::move(p), mt = std::move(t)](
                           Try<T>&& v) mutable {
          if (v.hasValue()) {
            try {
              ctx->fulfillWithValueOrFuture(
                  std::move(mp),
                  ctx->func_(
                      std::move(v.value()),
                      mt.template get<IsTry::value, Arg&&>()));
            } catch (std::exception& e) {
              mp.setException(exception_wrapper(std::current_exception(), e));
            } catch (...) {
              mp.setException(exception_wrapper(std::current_exception()));
            }
          } else {
            mp.setTry(std::move(v));
          }
        });
      });

  return ctx->promise_.getSemiFuture().via(&InlineExecutor::instance());
}

// within

template <class T>
Future<T> Future<T>::within(Duration dur, Timekeeper* tk) {
  return within(dur, FutureTimeout(), tk);
}

template <class T>
template <class E>
Future<T> Future<T>::within(Duration dur, E e, Timekeeper* tk) {
  if (this->isReady()) {
    return std::move(*this);
  }

  auto* exe = this->getExecutor();
  return this->withinImplementation(dur, e, tk)
      .via(exe ? exe : &InlineExecutor::instance());
}

// delayed

template <class T>
Future<T> Future<T>::delayed(Duration dur, Timekeeper* tk) && {
  auto e = this->getExecutor();
  return collectAllSemiFuture(*this, futures::sleep(dur, tk))
      .via(e ? e : &InlineExecutor::instance())
      .then([](std::tuple<Try<T>, Try<Unit>>&& tup) {
        return makeFuture<T>(std::get<0>(std::move(tup)));
      });
}

template <class T>
Future<T> Future<T>::delayedUnsafe(Duration dur, Timekeeper* tk) {
  return std::move(*this).semi().delayed(dur, tk).toUnsafeFuture();
}

namespace futures {
namespace detail {

template <class FutureType, typename T = typename FutureType::value_type>
void waitImpl(FutureType& f) {
  if (std::is_base_of<Future<T>, FutureType>::value) {
    f = std::move(f).via(&InlineExecutor::instance());
  }
  // short-circuit if there's nothing to do
  if (f.isReady()) {
    return;
  }

  FutureBatonType baton;
  f.setCallback_([&](const Try<T>& /* t */) { baton.post(); });
  baton.wait();
  assert(f.isReady());
}

template <class T>
void convertFuture(SemiFuture<T>&& sf, Future<T>& f) {
  // Carry executor from f, inserting an inline executor if it did not have one
  auto* exe = f.getExecutor();
  f = std::move(sf).via(exe ? exe : &InlineExecutor::instance());
}

template <class T>
void convertFuture(SemiFuture<T>&& sf, SemiFuture<T>& f) {
  f = std::move(sf);
}

template <class FutureType, typename T = typename FutureType::value_type>
void waitImpl(FutureType& f, Duration dur) {
  if (std::is_base_of<Future<T>, FutureType>::value) {
    f = std::move(f).via(&InlineExecutor::instance());
  }
  // short-circuit if there's nothing to do
  if (f.isReady()) {
    return;
  }

  Promise<T> promise;
  auto ret = promise.getSemiFuture();
  auto baton = std::make_shared<FutureBatonType>();
  f.setCallback_([baton, promise = std::move(promise)](Try<T>&& t) mutable {
    promise.setTry(std::move(t));
    baton->post();
  });
  convertFuture(std::move(ret), f);
  if (baton->try_wait_for(dur)) {
    assert(f.isReady());
  }
}

template <class T>
void waitViaImpl(Future<T>& f, DrivableExecutor* e) {
  // Set callback so to ensure that the via executor has something on it
  // so that once the preceding future triggers this callback, drive will
  // always have a callback to satisfy it
  if (f.isReady()) {
    return;
  }
  f = std::move(f).via(e).then([](T&& t) { return std::move(t); });
  while (!f.isReady()) {
    e->drive();
  }
  assert(f.isReady());
  f = std::move(f).via(&InlineExecutor::instance());
}

template <class T, typename Rep, typename Period>
void waitViaImpl(
    Future<T>& f,
    TimedDrivableExecutor* e,
    const std::chrono::duration<Rep, Period>& timeout) {
  // Set callback so to ensure that the via executor has something on it
  // so that once the preceding future triggers this callback, drive will
  // always have a callback to satisfy it
  if (f.isReady()) {
    return;
  }
  // Chain operations, ensuring that the executor is kept alive for the duration
  f = std::move(f).via(e).then(
      [keepAlive = getKeepAliveToken(e)](T&& t) { return std::move(t); });
  auto now = std::chrono::steady_clock::now();
  auto deadline = now + timeout;
  while (!f.isReady() && (now < deadline)) {
    e->try_drive_until(deadline);
    now = std::chrono::steady_clock::now();
  }
  assert(f.isReady() || (now >= deadline));
  if (f.isReady()) {
    f = std::move(f).via(&InlineExecutor::instance());
  }
}

} // namespace detail
} // namespace futures

template <class T>
SemiFuture<T>& SemiFuture<T>::wait() & {
  if (auto deferredExecutor = getDeferredExecutor()) {
    deferredExecutor->wait();
    deferredExecutor->runAndDestroy();
    this->core_->setExecutor(nullptr);
  } else {
    futures::detail::waitImpl(*this);
  }
  return *this;
}

template <class T>
SemiFuture<T>&& SemiFuture<T>::wait() && {
  return std::move(wait());
}

template <class T>
SemiFuture<T>& SemiFuture<T>::wait(Duration dur) & {
  if (auto deferredExecutor = getDeferredExecutor()) {
    if (deferredExecutor->wait(dur)) {
      deferredExecutor->runAndDestroy();
      this->core_->setExecutor(nullptr);
    }
  } else {
    futures::detail::waitImpl(*this, dur);
  }
  return *this;
}

template <class T>
SemiFuture<T>&& SemiFuture<T>::wait(Duration dur) && {
  return std::move(wait(dur));
}

template <class T>
T SemiFuture<T>::get() && {
  return std::move(*this).getTry().value();
}

template <class T>
T SemiFuture<T>::get(Duration dur) && {
  return std::move(*this).getTry(dur).value();
}

template <class T>
Try<T> SemiFuture<T>::getTry() && {
  wait();
  auto future = folly::Future<T>(this->core_);
  this->core_ = nullptr;
  return std::move(std::move(future).getTry());
}

template <class T>
Try<T> SemiFuture<T>::getTry(Duration dur) && {
  wait(dur);
  if (auto deferredExecutor = getDeferredExecutor()) {
    deferredExecutor->detach();
  }
  this->core_->setExecutor(nullptr);
  auto future = folly::Future<T>(this->core_);
  this->core_ = nullptr;

  if (!future.isReady()) {
    throw_exception<FutureTimeout>();
  }
  return std::move(std::move(future).getTry());
}

template <class T>
Future<T>& Future<T>::wait() & {
  futures::detail::waitImpl(*this);
  return *this;
}

template <class T>
Future<T>&& Future<T>::wait() && {
  futures::detail::waitImpl(*this);
  return std::move(*this);
}

template <class T>
Future<T>& Future<T>::wait(Duration dur) & {
  futures::detail::waitImpl(*this, dur);
  return *this;
}

template <class T>
Future<T>&& Future<T>::wait(Duration dur) && {
  futures::detail::waitImpl(*this, dur);
  return std::move(*this);
}

template <class T>
Future<T>& Future<T>::waitVia(DrivableExecutor* e) & {
  futures::detail::waitViaImpl(*this, e);
  return *this;
}

template <class T>
Future<T>&& Future<T>::waitVia(DrivableExecutor* e) && {
  futures::detail::waitViaImpl(*this, e);
  return std::move(*this);
}

template <class T>
Future<T>& Future<T>::waitVia(TimedDrivableExecutor* e, Duration dur) & {
  futures::detail::waitViaImpl(*this, e, dur);
  return *this;
}

template <class T>
Future<T>&& Future<T>::waitVia(TimedDrivableExecutor* e, Duration dur) && {
  futures::detail::waitViaImpl(*this, e, dur);
  return std::move(*this);
}

template <class T>
T Future<T>::get() {
  return std::move(wait().value());
}

template <class T>
T Future<T>::get(Duration dur) {
  wait(dur);
  if (!this->isReady()) {
    throw_exception<FutureTimeout>();
  }
  return std::move(this->value());
}

template <class T>
Try<T>& Future<T>::getTry() {
  return result();
}

template <class T>
T Future<T>::getVia(DrivableExecutor* e) {
  return std::move(waitVia(e).value());
}

template <class T>
T Future<T>::getVia(TimedDrivableExecutor* e, Duration dur) {
  waitVia(e, dur);
  if (!this->isReady()) {
    throw_exception<FutureTimeout>();
  }
  return std::move(value());
}

template <class T>
Try<T>& Future<T>::getTryVia(DrivableExecutor* e) {
  return waitVia(e).getTry();
}

template <class T>
Try<T>& Future<T>::getTryVia(TimedDrivableExecutor* e, Duration dur) {
  waitVia(e, dur);
  if (!this->isReady()) {
    throw_exception<FutureTimeout>();
  }
  return result();
}

namespace futures {
namespace detail {
template <class T>
struct TryEquals {
  static bool equals(const Try<T>& t1, const Try<T>& t2) {
    return t1.value() == t2.value();
  }
};
} // namespace detail
} // namespace futures

template <class T>
Future<bool> Future<T>::willEqual(Future<T>& f) {
  return collectAllSemiFuture(*this, f).toUnsafeFuture().then(
      [](const std::tuple<Try<T>, Try<T>>& t) {
        if (std::get<0>(t).hasValue() && std::get<1>(t).hasValue()) {
          return futures::detail::TryEquals<T>::equals(
              std::get<0>(t), std::get<1>(t));
        } else {
          return false;
        }
      });
}

template <class T>
template <class F>
Future<T> Future<T>::filter(F&& predicate) {
  return this->then([p = std::forward<F>(predicate)](T val) {
    T const& valConstRef = val;
    if (!p(valConstRef)) {
      throw_exception<FuturePredicateDoesNotObtain>();
    }
    return val;
  });
}

template <class F>
Future<Unit> when(bool p, F&& thunk) {
  return p ? std::forward<F>(thunk)().unit() : makeFuture();
}

template <class P, class F>
Future<Unit> whileDo(P&& predicate, F&& thunk) {
  if (predicate()) {
    auto future = thunk();
    return future.then([
      predicate = std::forward<P>(predicate),
      thunk = std::forward<F>(thunk)
    ]() mutable {
      return whileDo(std::forward<P>(predicate), std::forward<F>(thunk));
    });
  }
  return makeFuture();
}

template <class F>
Future<Unit> times(const int n, F&& thunk) {
  return folly::whileDo(
      [ n, count = std::make_unique<std::atomic<int>>(0) ]() mutable {
        return count->fetch_add(1) < n;
      },
      std::forward<F>(thunk));
}

namespace futures {
template <class It, class F, class ItT, class Result>
std::vector<Future<Result>> map(It first, It last, F func) {
  std::vector<Future<Result>> results;
  for (auto it = first; it != last; it++) {
    results.push_back(it->then(func));
  }
  return results;
}
} // namespace futures

template <class Clock>
Future<Unit> Timekeeper::at(std::chrono::time_point<Clock> when) {
  auto now = Clock::now();

  if (when <= now) {
    return makeFuture();
  }

  return after(std::chrono::duration_cast<Duration>(when - now));
}

// Instantiate the most common Future types to save compile time
extern template class Future<Unit>;
extern template class Future<bool>;
extern template class Future<int>;
extern template class Future<int64_t>;
extern template class Future<std::string>;
extern template class Future<double>;
} // namespace folly
