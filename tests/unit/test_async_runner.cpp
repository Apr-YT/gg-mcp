// test_async_runner.cpp — gg-ui 异步任务队列的并发语义验证（BACKLOG P1-7 的
// "并发调 call_tool 的语义没验证过"落地成测试；队列本身不依赖 Session，
// 语义与 UI 使用方式一一对应）。
//
// 断言的语义（违反任何一条，UI 异步化就是错的）：
//   a) 单 worker 串行：任意时刻最多一个 run() 在执行（重叠计数恒为 0）。
//   b) FIFO：完成顺序 == 投递顺序。
//   c) 不丢不重：N 个 job 的 run 与 apply 各恰好执行 N 次。
//   d) busy()/current_label() 与执行状态一致。
//   e) shutdown 丢弃未开始的 job：不 run 也不 apply；正在跑的 run 会跑完。
//   f) shutdown 后 post 被拒绝。
//   g) 多线程并发 post 不丢任务（队列自身的线程安全）。
#include "../../tools/gg-ui/async_runner.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <thread>
#include <vector>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("ok  : %s\n", msg); \
    else { printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); fails++; } \
} while (0)

// 等条件成立，超时按失败返回 false（避免断言失败时测试卡死）
static bool wait_for(const std::function<bool()>& cond, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (!cond()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

int main() {
    // ---- a/b/c/d) 串行 + FIFO + 不丢不重 + busy/label ----
    {
        gg::AsyncRunner r;
        std::atomic<int> in_run{0};
        std::atomic<int> max_overlap{0};
        std::atomic<int> ran{0};
        std::mutex order_mtx;
        std::vector<int> run_order;
        std::vector<int> apply_order;

        const int N = 50;
        for (int i = 0; i < N; i++) {
            r.post("job-" + std::to_string(i),
                   [&, i] {
                       int cur = ++in_run;
                       if (cur > max_overlap) max_overlap = cur;
                       {
                           std::lock_guard<std::mutex> lk(order_mtx);
                           run_order.push_back(i);
                       }
                       std::this_thread::sleep_for(std::chrono::milliseconds(1));
                       --in_run;
                       ++ran;
                   },
                   [&, i] {
                       std::lock_guard<std::mutex> lk(order_mtx);
                       apply_order.push_back(i);
                   });
        }
        CHECK(r.busy(), "a) busy() true while jobs pending");
        CHECK(wait_for([&] { return ran.load() == N; }, 5000),
              "c) all 50 run() executed");
        CHECK(max_overlap.load() == 1, "a) runs strictly serialized (no overlap)");
        size_t drained = r.drain_completed();
        CHECK(drained == (size_t)N, "c) drain returns all 50 completed");
        CHECK(apply_order.size() == (size_t)N, "c) all 50 apply() executed");
        CHECK(run_order == apply_order, "b) apply order == run order");
        std::vector<int> expect(N);
        std::iota(expect.begin(), expect.end(), 0);
        CHECK(run_order == expect, "b) FIFO: run order == post order");
        CHECK(!r.busy(), "d) busy() false after all done");
        CHECK(r.current_label().empty(), "d) current_label empty when idle");
    }

    // ---- d) current_label 在执行中可见 ----
    {
        gg::AsyncRunner r;
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
        r.post("slow-search",
               [&] {
                   entered = true;
                   while (!release.load())
                       std::this_thread::sleep_for(std::chrono::milliseconds(1));
               },
               [] {});
        CHECK(wait_for([&] { return entered.load(); }, 2000), "d) run started");
        CHECK(r.busy(), "d) busy while running");
        CHECK(r.current_label() == "slow-search", "d) label visible while running");
        release = true;
        CHECK(wait_for([&] { return !r.busy(); }, 2000), "d) finishes");
        r.drain_completed();
    }

    // ---- e/f) shutdown 正确顺序：丢弃未开始、跑完当前、post 拒绝 ----
    // 注意 shutdown 会等正在跑的 run() 结束（不可取消语义），所以测试必须先放行。
    {
        std::atomic<int> ran{0};
        std::atomic<int> applied{0};
        std::atomic<bool> release{false};
        {
            gg::AsyncRunner r;
            r.post("running", [&] {
                while (!release.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                ++ran;
            }, [&] { ++applied; });
            for (int i = 0; i < 5; i++)
                r.post("queued", [&] { ++ran; }, [&] { ++applied; });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            release = true;                 // 放掉正在跑的
            r.shutdown();                   // 等它跑完，丢弃 5 个排队的
            CHECK(ran.load() == 1, "e) only the running job ran; queued dropped");
            CHECK(r.drain_completed() <= 1, "e) at most the finished one completes");
            CHECK(!r.post("late", [] {}, [] {}), "f) post after shutdown rejected");
        }
    }

    // ---- g) 多线程并发 post 不丢 ----
    {
        gg::AsyncRunner r;
        std::atomic<int> ran{0};
        const int T = 8, PER = 100;
        std::vector<std::thread> ts;
        for (int t = 0; t < T; t++)
            ts.emplace_back([&, t] {
                for (int i = 0; i < PER; i++)
                    r.post("t" + std::to_string(t), [&] { ++ran; }, [] {});
            });
        for (auto& th : ts) th.join();
        CHECK(wait_for([&] { return ran.load() == T * PER; }, 10000),
              "g) 8 threads x 100 posts: none lost");
        CHECK(r.drain_completed() == (size_t)(T * PER),
              "g) all completed exactly once");
    }

    printf(fails ? "\n== FAILED: %d ==\n" : "\n== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
