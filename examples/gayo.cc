#include <iostream>
#include <unistd.h>
#include <queue>

#include <coroutine.h>
#include <fmt/format.h>
#include <future.h>

std::queue<gayo::promise<>> prs;

gayo::future<> mysleep() {
  gayo::promise<> pr;
  auto fu = pr.get_future();
  prs.emplace(std::move(pr));
  return fu;
}

gayo::future<> cycle() {
  int count = 10;
  while (--count) {
    fmt::print("count:{}\n", count);
    co_await mysleep();
  }
}

int main() {
  gayo::sched.hook = [](std::unique_ptr<gayo::task> task) {
    fmt::print("custom schedule\n");
    auto t = task.release();
    t->run_and_dispose();
  };

  cycle();

  // schedule
  while (!prs.empty()) {
    auto& pr = prs.front();
    pr.set_value();
    prs.pop();
    sleep(1);
  }

  fmt::print("finished\n");
  return 0;
}