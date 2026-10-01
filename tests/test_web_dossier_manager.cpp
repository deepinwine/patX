#include "database.hpp"
#include "patx/sidecar_process_lifecycle.hpp"
#include "web_dossier.hpp"

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace webdossier;

#define CHECK(condition) do { if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #condition << std::endl; return false; } } while (false)
#define CHECK_EQ(actual, expected) CHECK((actual) == (expected))

namespace {

struct BlockingRpcState {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool saw_cancel = false;
    bool shutdown = false;
    bool destroyed = false;
    ResultCode result = ResultCode::SidecarError;
};

class BlockingTransport : public RpcTransport {
public:
    explicit BlockingTransport(std::shared_ptr<BlockingRpcState> state)
        : state_(std::move(state)) {}

    ~BlockingTransport() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->destroyed = true;
        state_->cv.notify_all();
    }

    bool Start(const std::string&, std::string&) override { return true; }

    bool Call(const std::string&, int, std::string&,
              std::atomic<bool>& cancel) override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->entered = true;
        state_->cv.notify_all();
        state_->cv.wait(lock, [&] { return cancel.load(); });
        state_->saw_cancel = true;
        return false;
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->shutdown = true;
        state_->cv.notify_all();
    }

private:
    std::shared_ptr<BlockingRpcState> state_;
};

class ManagerSyncWorker : public IJoinableDossierWorker {
public:
    ManagerSyncWorker(Manager& manager, Patent patent,
                      std::shared_ptr<BlockingRpcState> state)
        : state_(std::move(state)), thread_([this, &manager, patent = std::move(patent)] {
              const auto report = manager.SyncCase(patent, cancel_);
              std::lock_guard<std::mutex> lock(state_->mutex);
              state_->result = report.code;
          }) {}

    void RequestCancel() override {
        cancel_ = true;
        state_->cv.notify_all();
    }

    void Join() override {
        if (thread_.joinable()) thread_.join();
    }

private:
    std::shared_ptr<BlockingRpcState> state_;
    std::atomic<bool> cancel_{false};
    std::thread thread_;
};

} // namespace

static bool TestCancellationDropsTransport() {
    Database db(":memory:");
    if (!db.IsOpen()) {
        std::cerr << "failed to open test database" << std::endl;
        return false;
    }

    auto state = std::make_shared<BlockingRpcState>();
    Manager manager(db, ".", std::make_unique<BlockingTransport>(state));
    Patent patent;
    patent.id = 1;
    patent.geke_code = "CN-CANCEL";
    patent.application_number = "CN202410000001.1";

    DossierWorkerOwner owner;
    auto worker = std::make_unique<ManagerSyncWorker>(manager, patent, state);
    owner.Adopt(std::move(worker));

    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [&] { return state->entered; });
    }
    owner.CancelAndJoin();

    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        ok = state->result == ResultCode::Cancelled && state->saw_cancel &&
             state->shutdown && state->destroyed;
    }
    if (!ok) {
        std::cerr << "cancel was not propagated through the blocking RPC" << std::endl;
        return false;
    }
    return true;
}

static bool TestSidecarProcessLifecycleState() {
    SidecarProcessLifecycle lifecycle;
    int notifications = 0;

    const auto first = lifecycle.BeginChild();
    CHECK(first != 0);
    CHECK(lifecycle.NotifyChildExit(first, [&] { ++notifications; }));
    CHECK_EQ(notifications, 1);

    const auto stale = lifecycle.BeginChild();
    const auto current = lifecycle.BeginChild();
    CHECK(stale != current);
    CHECK(!lifecycle.NotifyChildExit(stale, [&] { ++notifications; }));
    CHECK_EQ(notifications, 1);
    CHECK(lifecycle.NotifyChildExit(current, [&] { ++notifications; }));
    CHECK_EQ(notifications, 2);

    const auto stoppable = lifecycle.BeginChild();
    CHECK(lifecycle.StopChild(stoppable));
    CHECK(!lifecycle.StopChild(stoppable));
    CHECK(!lifecycle.NotifyChildExit(stoppable, [&] { ++notifications; }));

    const auto detached = lifecycle.BeginChild();
    lifecycle.DetachOwner();
    CHECK(!lifecycle.NotifyChildExit(detached, [&] { ++notifications; }));
    CHECK_EQ(notifications, 2);
    return true;
}

int main() {
    if (!TestCancellationDropsTransport()) return 1;
    if (!TestSidecarProcessLifecycleState()) return 1;
    std::cout << "web dossier manager and sidecar lifecycle tests passed" << std::endl;
    return 0;
}
