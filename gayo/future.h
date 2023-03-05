// import from seastar

#pragma once

#include <exception>
#include <tuple>

#include <boost/smart_ptr/local_shared_ptr.hpp>
#include <boost/smart_ptr/make_local_shared.hpp>

namespace gayo {

struct task {
  virtual ~task(){};
  virtual void run_and_dispose() = 0;
};

template <typename Func> struct lambda_task : public task {
  Func func;
  lambda_task(const Func &func) : func(func) {}
  lambda_task(Func &&func) : func(std::move(func)) {}

  void run_and_dispose() override {
    func();
    delete this;
  }
};

struct scheduler {
  void operator()(std::unique_ptr<task> task) {
    assert(hook);
    hook(std::move(task));
  }

  std::function<void(std::unique_ptr<task>)> hook;
};

thread_local scheduler sched;

void make_ready(std::unique_ptr<task> task) {
  if (!task)
    return;

  sched(std::move(task));
}

template <typename Func> std::unique_ptr<task> make_task(const Func &func) {
  return std::make_unique<lambda_task<Func>>(func);
}

template <typename Func> std::unique_ptr<task> make_task(Func &&func) {
  return std::make_unique<lambda_task<Func>>(std::move(func));
}

template <typename... T> class promise;

template <typename... T> class future;

template <typename... T, typename... A>
future<T...> make_ready_future(A &&... value);

template <typename... T>
future<T...> make_exception_future(std::exception_ptr value) noexcept;

template <typename... T> struct is_future : std::false_type {};
template <typename... T> struct is_future<future<T...>> : std::true_type {};

template <typename... T> class future_state {
public:
  enum class state { future, result, exception };

  ~future_state() {
    switch (_state) {
    case state::future:
      break;
    case state::result:
      _u.value.~tuple();
      break;
    case state::exception:
      _u.ex.~exception_ptr();
      break;
    default:
      abort();
    }
  }

  bool available() const {
    return _state == state::result || _state == state::exception;
  }

  template <typename... U> void set(U &&... value) {
    assert(_state == state::future);
    _state = state::result;
    new (&_u.value) std::tuple<T...>(std::forward<U>(value)...);
    make_ready(std::move(_task));
  }

  void set_exception(std::exception_ptr ex) noexcept {
    assert(_state == state::future);
    _state = state::exception;
    new (&_u.ex) std::exception_ptr(ex);
  }

  std::tuple<T...> get() {
    assert(_state != state::future);
    if (_state == state::exception) {
      std::rethrow_exception(_u.ex);
    }
    return std::move(_u.value);
  }

  void forward_to(promise<T...> &pr) noexcept {
    assert(_state != state::future);
    if (_state == state::exception) {
      pr.set_exception(_u.ex);
    } else {
      pr.set(std::move(get()));
    }
  }

  template <typename Func> void schedule(Func &&func) {
    assert(_state == state::future);
    assert(!_task);
    _task = make_task(std::forward<Func>(func));
  }

private:
  union any {
    any() {}
    ~any() {}
    std::tuple<T...> value;
    std::exception_ptr ex;
  } _u;

  state _state = state::future;
  std::unique_ptr<task> _task;
};

template <typename... T> class promise {
public:
  promise() : _future_state(boost::make_local_shared<future_state<T...>>()) {}

  future<T...> get_future() { return future<T...>(_future_state); }

  template <typename... U> void set_value(U &&... value) {
    _future_state->set(std::forward<U>(value)...);
  }

  void set_exception(std::exception_ptr ex) noexcept {
    _future_state->set_exception(std::move(ex));
  }

  template <typename Exception> void set_exception(Exception &&e) noexcept {
    set_exception(make_exception_ptr(std::forward<Exception>(e)));
  }

private:
  boost::local_shared_ptr<future_state<T...>> _future_state;
};

template <typename... T> class future {
public:
  future(future &&) = default;
  future &operator=(future &&) = default;
  future(const future &) = delete;
  future &operator=(const future &) = delete;

  std::tuple<T...> get() { return _future_state->get(); }

  bool available() const { return _future_state->available(); }

  void set_coroutine(task& task) {
    schedule([&task] {
      task.run_and_dispose();
    });
  }

  template <typename Func> void schedule(Func &&func) {
    _future_state->schedule(std::forward<Func>(func));
  }

  template <typename Func, typename Enable> void then(Func, Enable) noexcept;

  template <typename Func>
  future<>
  then(Func &&func,
       std::enable_if_t<
           std::is_same<std::result_of_t<Func(T &&...)>, void>::value, void *> =
           nullptr) noexcept {
    assert(_future_state);

    if (available()) {
      try {
        std::apply(std::move(func), std::move(state()->get()));
        return make_ready_future<>();
      } catch (...) {
        return make_exception_future(std::current_exception());
      }
    }
    promise<> pr;
    auto fut = pr.get_future();

    state()->schedule([fu = std::move(*this), pr = std::move(pr),
                       func = std::forward<Func>(func)]() mutable {
      try {
        std::apply(std::move(func), fu.get());
        pr.set_value();
      } catch (...) {
        pr.set_exception(std::current_exception());
      }
    });
    return fut;
  }

  void forward_to(promise<T...> &pr) noexcept { _future_state->forward_to(pr); }

  future_state<T...> *state() { return _future_state.get(); }

  template <typename Func>
  std::result_of_t<Func(T &&...)>
  then(Func &&func,
       std::enable_if_t<is_future<std::result_of_t<Func(T &&...)>>::value,
                        void *> = nullptr) noexcept {
    using P = typename std::result_of_t<Func(T && ...)>::promise_type;
    if (available()) {
      try {
        return std::apply(std::move(func), std::move(state()->get()));
      } catch (...) {
        P pr;
        pr.set_exception(std::current_exception());
        return pr.get_future();
      }
    }
    P pr;
    auto next_fut = pr.get_future();
    state()->schedule([fu = std::move(*this), func = std::forward<Func>(func),
                       pr = std::move(pr)]() mutable {
      try {
        auto result = fu.get();
        auto next_fut = std::apply(std::move(func), std::move(result));
        next_fut.forward_to(std::move(pr));
      } catch (...) {
        pr.set_exception(std::current_exception());
      }
    });
    return next_fut;
  }

  template <typename Func>
  std::result_of_t<Func(future<T...>)> then_wrapped(Func &&func) &&noexcept {
    using P = typename std::result_of_t<Func(future<T...>)>::promise_type;
    if (available()) {
      try {
        return func(std::move(*this));
      } catch (...) {
        P pr;
        pr.set_exception(std::current_exception());
        return pr.get_future();
      }
    }
    P pr;
    auto next_fut = pr.get_future();
    state()->schedule([fu = std::move(*this), func = std::forward<Func>(func),
                       pr = std::move(pr)]() mutable {
      try {
        auto next_fut = func(std::move(fu));
        next_fut.forward_to(std::move(pr));
      } catch (...) {
        pr.set_exception(std::current_exception());
      }
    });

    return next_fut;
  }

  template <typename Func> future<> rescue(Func &&func) &&noexcept {
    if (available()) {
      try {
        func([&state = *state()] { return state.get(); });
        return make_ready_future();
      } catch (...) {
        return make_exception_future(std::current_exception());
      }
    }
    promise<> pr;
    auto f = pr.get_future();
    state()->schedule([fu = std::move(*this), pr = std::move(pr),
                       func = std::forward<Func>(func)]() mutable {
      try {
        func([&fu]() mutable { return fu.get(); });
        pr.set_value();
      } catch (...) {
        pr.set_exception(std::current_exception());
      }
    });
    return f;
  }

  template <typename Func> future<T...> finally(Func &&func) noexcept {
    promise<T...> pr;
    auto f = pr.get_future();
    if (available()) {
      try {
        func();
      } catch (...) {
        pr.set_exception(std::current_exception());
        return f;
      }
      forward_to(pr);
      return f;
    }
    state()->schedule([fu = std::move(*this), pr = std::move(pr),
                       func = std::forward<Func>(func)]() mutable {
      try {
        func();
      } catch (...) {
        pr.set_exception(std::current_exception());
        return;
      }
      fu.forward_to(pr);
    });
    return f;
  }

  future<> or_terminate() &&noexcept {
    return std::move(*this).rescue([](auto get) {
      try {
        get();
      } catch (...) {
        std::terminate();
      }
    });
  }

  future<> discard_result() &&noexcept {
    return then([](T &&...) {});
  }

private:
  future(boost::local_shared_ptr<future_state<T...>> future_state)
      : _future_state(future_state) {
    assert(_future_state);
  }

  boost::local_shared_ptr<future_state<T...>> _future_state;

  template <typename... U> friend class promise;
  template <typename... U, typename... A>
  friend future<U...> make_ready_future(A &&... value);
  template <typename... U>
  friend future<U...> make_exception_future(std::exception_ptr ex) noexcept;
  template <typename... U, typename Exception>
  friend future<U...> make_exception_future(Exception &&ex) noexcept;
};

template <typename... T, typename... A>
inline future<T...> make_ready_future(A &&... value) {
  auto state = boost::make_local_shared<future_state<T...>>();
  state->set(std::forward<A>(value)...);
  return future<T...>(state);
}

template <typename... T>
inline future<T...> make_exception_future(std::exception_ptr ex) noexcept {
  auto state = boost::make_local_shared<future_state<T...>>();
  state->set_exception(std::move(ex));
  return future<T...>(state);
}

template <typename... T, typename Exception>
inline future<T...> make_exception_future(Exception &&ex) noexcept {
  return make_exception_future<T...>(
      make_exception_ptr(std::forward<Exception>(ex)));
}

} // namespace gayo
