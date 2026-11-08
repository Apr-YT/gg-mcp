// async_runner.h — gg-ui 的单 worker 异步任务队列（纯 STL，不依赖 imgui/Session）
//
// 为什么存在（BACKLOG P1-7）：UI 里搜索是同步 call_tool（全量 0.19s、模糊
// 首帧 0.32s），期间界面卡死。解法是把阻塞调用丢给 worker 线程，UI 线程只做
// 展示。队列机制独立于 UI 单独可测（tests/unit/test_async_runner.cpp）。
//
// 并发语义（这就是 BACKLOG 说的"没验证过的语义"，本类 + 单测把它钉死）：
//   1. 单 worker 线程 → 所有 job 的 run() 严格串行，天然不存在"两个搜索同时
//      打到 Session"。（Session::call_tool 全程持会话锁，wire 层也有自己的锁，
//      即使 UI 线程同时发快操作，wire 上也是串行的。）
//   2. apply() 只在 drain_completed() 的调用线程（UI 线程）执行 → g_app 的
//      一切变更保持单线程语义，worker 永远不许碰 g_app/logf。
//   3. 不可取消：wire 调用不可中断，session 也没有 cancel 语义。shutdown()
//      丢弃未开始的 job（不 run 也不 apply），等正在跑的 run() 结束才返回。
//   4. FIFO 保序：先点先跑。"搜索中再点搜索"由 UI 层禁用按钮挡住，不靠队列。
//
// 用法：
//   gg::AsyncRunner runner;            // main 启动时构造（worker 随即起跑）
//   runner.post("精确搜索",
//               []{ /* run: 只能 call_tool + 写自己的 outcome */ },
//               []{ /* apply: g_app 变更 + logf，UI 线程 */ });
//   每帧: runner.drain_completed();
//   退出前: runner.shutdown();          // 幂等；之后 post 直接拒绝
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace gg {

class AsyncRunner {
public:
    struct Job {
        std::string label;
        std::function<void()> run;      // worker 线程
        std::function<void()> apply;    // drain 线程（UI）
    };

    AsyncRunner() : worker_([this] { loop(); }) {}
    ~AsyncRunner() { shutdown(); }

    AsyncRunner(const AsyncRunner&) = delete;
    AsyncRunner& operator=(const AsyncRunner&) = delete;

    // 投递任务。shutdown 后拒绝（返回 false，job 不执行）。
    bool post(std::string label, std::function<void()> run, std::function<void()> apply) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stopping_) return false;
            pending_.push_back(Job{std::move(label), std::move(run), std::move(apply)});
        }
        cv_.notify_one();
        return true;
    }

    // UI 线程每帧调用：执行所有已完成 job 的 apply。
    // 返回执行的个数（测试用）。
    size_t drain_completed() {
        std::deque<Job> done;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            done.swap(completed_);
        }
        for (auto& j : done)
            if (j.apply) j.apply();
        return done.size();
    }

    // 有待执行或正在执行。UI 据此禁用按钮 + 转圈。
    bool busy() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return running_ || !pending_.empty();
    }

    // 正在执行的 job 的 label（spinner 文案）；空闲返回空串。
    std::string current_label() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return running_ ? cur_label_ : std::string();
    }

    // 排队数（不含正在跑的），测试与状态栏用。
    size_t queued() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return pending_.size();
    }

    // 停止：丢弃未开始的 job，等当前 run() 结束后 join。幂等。
    // 注意：被丢弃 job 的 apply 不会执行 —— 它们的 outcome 随之销毁，
    // 这是退出路径，UI 马上要没了，不需要"应用一半的状态"。
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stopping_) return;
            stopping_ = true;
            pending_.clear();
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

private:
    void loop() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] { return stopping_ || !pending_.empty(); });
                if (stopping_ && pending_.empty()) return;
                j = std::move(pending_.front());
                pending_.pop_front();
                running_ = true;
                cur_label_ = j.label;
            }
            if (j.run) j.run();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                running_ = false;
                cur_label_.clear();
                // stopping 时完成的 job 也推进 completed：apply 都是纯 UI
                // 状态更新，shutdown 后由调用方决定还 drain 不 drain（退出
                // 路径不 drain 也无害 —— 反正窗口马上销毁）。
                completed_.push_back(std::move(j));
            }
        }
    }

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Job> pending_;
    std::deque<Job> completed_;
    bool running_ = false;
    bool stopping_ = false;
    std::string cur_label_;
    std::thread worker_;
};

} // namespace gg
