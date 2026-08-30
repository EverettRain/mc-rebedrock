#pragma once

// 严格按环号排序的 section 投递
//
// 真机上约 5 秒的流送长尾并不是漏了工作，也不是某一帧的 CPU 开销，而是严重的乱序
// 待处理 section 的积压原本是一条按到达顺序排的普通 FIFO
// 于是同一批次里先完成网格化的远环 section 会排在近中心 section 前面
// 而一个 24 区块的生成批次本就跨好几个切比雪夫环
// 积压上限的淘汰又取最旧的那一项，完全不看环号
// 这会把近中心的工作淘汰或饿死在一条已经很长的远环尾巴后面
// 实测见到过紧邻中心的八个区块被饿到 4.7 秒，而毫无优先权的远处区块反倒先落地
//
// 这个容器用一小组按环分桶的 FIFO 取代那条普通 deque：
//   - 环内保持到达顺序，一个批次自身的完成顺序不被重排
//   - 跨环时永远先排空环号最小的非空环，即严格的由近及远投递
//     这一点在消费侧成立，与生产侧的批次划得多粗无关
//   - 积压上限的淘汰改为从最远的非空环取，满积压因此甩掉远处的工作而绝不甩中心的工作
//     这正是对那次 4.7 秒饿死的直接修复
//   - 另有一条优先通道，行为与从前一致：玩法编辑仍然直接插到整个队列最前，排在所有环桶之前
//     编辑本来就不属于环形填充这幅图景
//
// 数据布局上，环桶的数量小且有界，等于 loadRadius 加上卸载迟滞，量级在十上下
// evictFarthest 反向扫描第一个非空桶因此只是几次空检查，不构成热路径开销
// push、pop、erase 都只碰自己那个桶，均摊 O(1)，deque 节点的开销与从前那条单 deque 积压完全相同
// 除 std::deque 自身之外，没有逐次调用的堆分配
//
// 依赖上与 ChunkStreamingTrace.hpp 一样轻
// 它按调用方的键与哈希类型做模板，而不是直接依赖 world::SectionPosition
// 这个头文件及其测试因此不会把 ChunkStreamer.cpp 的生成、网格化与持久化依赖图拉进来

#include <cstddef>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace mc::render {

template <typename Key, typename KeyHash>
class SectionDeliveryQueue final {
  public:
    // 把 key 插入到 ring 号所在的桶，负数环号夹到 0
    // highPriority 的 section 也就是玩法编辑走优先通道，它永远先于任何环桶被排空
    // 这与编辑一向插到最前的行为一致
    // 调用方需要先查 contains(key)，压入一个已经在队列里的键会让 location_ 与它真正所在的桶失去同步
    void push(const Key& key, int ring, bool highPriority) {
        if (highPriority) {
            priority_.push_back(key);
            location_.insert_or_assign(key, -1);
            return;
        }
        const std::size_t bucket = bucketFor(ring);
        ensureBucket(bucket);
        rings_[bucket].push_back(key);
        location_.insert_or_assign(key, static_cast<int>(bucket));
    }

    [[nodiscard]] bool contains(const Key& key) const { return location_.contains(key); }

    [[nodiscard]] std::size_t size() const { return location_.size(); }

    [[nodiscard]] bool empty() const { return location_.empty(); }

    // 下一个该投递的键：先取优先通道，再取环号最小的非空桶，两者内部都是 FIFO
    // 调用方必须先查 empty()
    [[nodiscard]] const Key& front() const {
        if (!priority_.empty()) {
            return priority_.front();
        }
        return rings_[lowestNonEmptyRing()].front();
    }

    void popFront() {
        if (!priority_.empty()) {
            location_.erase(priority_.front());
            priority_.pop_front();
            return;
        }
        auto& bucket = rings_[lowestNonEmptyRing()];
        location_.erase(bucket.front());
        bucket.pop_front();
    }

    // 从最远的非空环桶里淘汰一项并返回，没有可淘汰的则返回 nullopt
    // 优先通道永远不淘汰，优先项不受积压上限约束
    // 这是对饿死问题的修复：旧策略淘汰的是 pendingSectionOrder.back()
    // 由于那条队列纯按到达顺序排，back() 常常是一个近中心 section，它只是在某个批次里到得晚
    // 而那个批次在 deque 里还排着大量远环工作
    // 改从最远环淘汰之后，被甩掉的工作永远至少和队列里剩下的任何一项一样远
    [[nodiscard]] std::optional<Key> evictFarthest() {
        for (std::size_t bucket = rings_.size(); bucket-- > 0U;) {
            if (rings_[bucket].empty()) {
                continue;
            }
            const Key victim = rings_[bucket].back();
            location_.erase(victim);
            rings_[bucket].pop_back();
            return victim;
        }
        return std::nullopt;
    }

    // 把指定的键从它所在的位置移除而不投递，无论它在优先通道还是某个环桶里
    // 调用方在带外作废一个待处理项时用它，比如那一项即将带着更新的数据、可能换一个环号重新插入
    void erase(const Key& key) {
        const auto found = location_.find(key);
        if (found == location_.end()) {
            return;
        }
        if (found->second < 0) {
            eraseFrom(priority_, key);
        } else {
            eraseFrom(rings_[static_cast<std::size_t>(found->second)], key);
        }
        location_.erase(found);
    }

    void clear() {
        priority_.clear();
        rings_.clear();
        location_.clear();
    }

  private:
    [[nodiscard]] static std::size_t bucketFor(int ring) {
        return static_cast<std::size_t>(ring < 0 ? 0 : ring);
    }

    void ensureBucket(std::size_t bucket) {
        if (bucket >= rings_.size()) {
            rings_.resize(bucket + 1U);
        }
    }

    // 从 0 号环往上扫，找第一个非空桶
    // 上界是环桶的数量，那个数很小，见类上方关于数据布局的说明
    [[nodiscard]] std::size_t lowestNonEmptyRing() const {
        for (std::size_t bucket = 0U; bucket < rings_.size(); ++bucket) {
            if (!rings_[bucket].empty()) {
                return bucket;
            }
        }
        return 0U; // Unreachable when front()/popFront() are only called on
                   // 队列非空，这是它们各自写明的前置条件
    }

    static void eraseFrom(std::deque<Key>& bucket, const Key& key) {
        for (auto it = bucket.begin(); it != bucket.end(); ++it) {
            if (*it == key) {
                bucket.erase(it);
                return;
            }
        }
    }

    std::deque<Key> priority_;
    std::vector<std::deque<Key>> rings_;
    // 每个在队的键当前落在哪个桶，值是环号，-1 表示优先通道
    // 有了它，erase() 与 popFront() 不必线性扫描每个桶去找
    std::unordered_map<Key, int, KeyHash> location_;
};

} // namespace mc::render
