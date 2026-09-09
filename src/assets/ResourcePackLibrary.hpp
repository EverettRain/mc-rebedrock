#pragma once

// 资源包的「玩家可见」那一层：谁在磁盘上、叫什么、启用了哪些、什么顺序、写在哪。
//
// PackManager 管的是 provider 栈的机械装配（两条栈、谁叠在谁上面），它不知道
// 「用户选了什么」这回事——启动流程里包一被发现就无条件 enable，id 还是
// pack0/pack1 这种当场编出来的序号，元数据整个是空的。资源包选择界面要的三样
// （名字、描述、可开关的启用状态）因此一样都拿不到。
//
// 本类补上那一层，并且是 PackManager 的**所有者**：渲染器只要拿到一个
// ResourcePackLibrary& 就同时够得着「列出包 / 启停 / 调序 / 提交」四个操作和
// 当前生效的资源栈，不必认识 provider、更不必认识 Vulkan。
//
// ★ 提交只落盘，不热重载（下次启动生效）。理由记在
//   wiki/architecture/known-debt.md「资源包热重载」一节：本作的资源栈同时是
//   集成式运行时的**数据包底座**，而方块图集的层号已经烘进了每一份已上传的
//   区块网格里，换栈要连带重建玩法数据表、重铺图集、重写描述符集并让区块流送
//   线程全部停下重网格化。commit() 因此返回 restartRequired，界面据此提示。

#include "assets/PackManager.hpp"
#include "assets/PackMetadata.hpp"
#include "assets/ResourceProvider.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc::assets {

// 一个被发现的包在选择界面里的样子。字段全部来自磁盘（目录名 + pack.mcmeta），
// 没有一项是这里编出来的——UI 显示什么就是包自己声明的什么。
struct ResourcePackEntry final {
    // 稳定键：resourcepacks/ 下的目录名，或 .zip 的文件名（含扩展名）。
    // 启用列表、调序、落盘全都引用它；它不是路径，因此把整个游戏目录搬个地方
    // 也不会让玩家的选择失效。
    std::string id;
    // 列表主行。就是 id 去掉 .zip 后缀——vanilla 也是拿文件名当标题，
    // pack.mcmeta 里没有「名字」这个字段，只有描述。
    std::string title;
    // pack.mcmeta 的 description，可能为空（包没写，或写成了富文本数组——
    // PackMetadata 只收纯字符串那一种形态）。
    std::string description;
    int minFormat = 0;
    int maxFormat = 0;
    // pack_format 与本 build 的资源半边是否匹配。与 vanilla 一样只是个提示，
    // 不阻止启用——照样加载，界面上标一下。
    bool compatible = true;
    // `--pack` 点名的包。命令行说了用哪个包就必须是那个包，因此它强制启用、
    // 强制置顶，界面不该给关（setEnabled(false) 对它是 no-op）。
    bool pinned = false;
};

struct PackCommitOutcome final {
    // 落盘是否成功。error 非空时为 false。
    bool written = false;
    // 草稿与本次运行生效的集合不同 → 需要重启才看得见效果。
    bool restartRequired = false;
    std::string error;
};

class ResourcePackLibrary final {
  public:
    // `selectionFile` 是启用列表的落盘位置。选这么一个独立文件而不是往
    // options.properties 里塞一行，有两个理由：
    //   1. 这是一份**有序列表**，而 options 是扁平的 key=value；塞进去就得选一个
    //      分隔符，而包 id 是文件名——逗号、等号、冒号、空格在文件名里都合法，
    //      于是立刻需要一套转义规则。一行一个 id 不需要转义。
    //   2. options.properties 每改一个设置就整份重写；启用列表是另一条节奏
    //      （只在玩家按下 Done 时写一次），两者混在一份文件里，任何一次
    //      普通设置写盘都会顺带重写玩家的资源包选择。
    explicit ResourcePackLibrary(std::filesystem::path selectionFile);

    // ---- 发现期：由启动流程按扫描顺序调用 ----

    // 登记一个已发现的包。`provider` 与 `overlays` 都是**非拥有**指针，
    // 由调用方保管其生命周期（ZipResourcePackProvider 依赖 miniz、只存在于
    // 游戏可执行文件那个目标里，所以本类不能自己去建它）。
    // `overlays` 是这个包内部按版本门控展开出来的附加层，自下而上排列——它们
    // 属于这个包，不是独立的包，因此不会出现在 packs() 里。
    void addPack(ResourcePackEntry entry, const ResourceProvider& provider,
                 std::vector<const ResourceProvider*> overlays = {});

    // 读启用列表。**必须在全部 addPack 之后调用**：文件里列到的 id 要和已发现的
    // 包对得上才算数（包被删掉了就从列表里掉出去）。
    //
    // ★ 文件不存在 = 发现到的包全部启用，顺序即扫描顺序——也就是本特性之前的
    //   行为，一字不差。新玩家、旧游戏目录都落在这一支上。
    void loadSelection();

    // 按当前草稿装配资源栈。可以重复调用（未来做热重载时，这就是那条
    // 「重建资源栈」的接口）；provider() 的地址在重建前后保持不变。
    void buildStack(const ResourceProvider& base);

    // 当前生效的资源栈。buildStack 之前调用是编程错误（直接中止，而不是
    // 交出一个空栈让调用方渲染出满屏缺失纹理）。
    [[nodiscard]] const ResourceProvider& provider() const;
    [[nodiscard]] bool hasStack() const { return stack_.has_value(); }

    // ---- UI 四操作 ----

    // 列出：全部已发现的包，按扫描顺序（与启用与否无关，这样界面上一行的位置
    // 不会因为勾选而跳动）。启用状态问 isEnabled，优先级问 draftOrder。
    [[nodiscard]] const std::vector<ResourcePackEntry>& packs() const { return packs_; }
    [[nodiscard]] const ResourcePackEntry* find(const std::string& id) const;

    // 草稿里已启用的 id，自下而上：越靠后优先级越高，覆盖前面的。
    // 界面上「已选」那一列若按 vanilla 的高优先级在上排列，倒着读这个列表即可。
    [[nodiscard]] const std::vector<std::string>& draftOrder() const { return draft_; }
    // 本次运行**实际**生效的那一份。与 draftOrder 的差就是「重启才生效」的部分。
    [[nodiscard]] const std::vector<std::string>& activeOrder() const { return active_; }
    [[nodiscard]] bool isEnabled(const std::string& id) const;

    // 启停。启用时接在栈顶（优先级最高），与 PackManager::enable 一致。
    // 返回草稿是否真的变了（未知 id、重复启停、想关掉 pinned 包都返回 false）。
    bool setEnabled(const std::string& id, bool enabled);

    // 调序：在启用栈里挪一位。「Up」= 提高优先级 = 更能盖住别人。
    // 返回是否真的挪动了（已经在端点、或根本没启用则不动）。
    bool movePriorityUp(const std::string& id);
    bool movePriorityDown(const std::string& id);

    // 提交：把草稿写进 selectionFile。**不重载**任何资源，见文件头。
    PackCommitOutcome commit();
    // 放弃草稿，回到本次运行生效的那一份。
    void discardDraft();
    [[nodiscard]] bool draftDiffersFromActive() const { return draft_ != active_; }

    // ---- 包身份 ----
    // 由磁盘路径导出稳定 id：resourcepacks/ 下的目录名，或 .zip 的文件名（含扩展名）。
    // 已经登记过同名的包（一个 resourcepacks/foo 和一个 --pack 指到别处的 foo）会拿到
    // 加了后缀的 id——id 是要落盘的键，撞了就会在下次启动时张冠李戴。
    //
    // 身份规则住在这里而不是启动流程里：id 一旦退化成 pack0/pack1 这种序号，
    // 玩家增删一个包就会让落盘的名单整体错位、指到别的包上，而且没有任何报错。
    [[nodiscard]] std::string deriveId(const std::filesystem::path& path) const;
    // 列表主行：id 去掉 .zip 后缀。pack.mcmeta 里没有「名字」这个字段。
    [[nodiscard]] static std::string titleFromId(std::string_view id);

    // ---- 纯函数：文件格式 ----
    // 一行一个 id；空行与 `#` 开头的行是注释。行首尾空白被去掉，因此一个
    // 首尾带空格的目录名无法通过本文件表达（写盘时会被 commit() 拒绝）。
    [[nodiscard]] static std::vector<std::string> parseSelection(std::string_view text);
    [[nodiscard]] static std::string serializeSelection(const std::vector<std::string>& order);
    // 这个 id 能否原样写进启用列表再读回来。
    [[nodiscard]] static bool idIsPersistable(std::string_view id);

    [[nodiscard]] const PackManager& packManager() const { return manager_; }
    [[nodiscard]] const std::filesystem::path& selectionFile() const { return selectionFile_; }

  private:
    struct Registered final {
        const ResourceProvider* provider = nullptr;
        std::vector<const ResourceProvider*> overlays;
        PackMetadata metadata;
    };

    [[nodiscard]] std::size_t indexOf(const std::string& id) const;

    std::filesystem::path selectionFile_;
    std::vector<ResourcePackEntry> packs_;
    std::vector<Registered> registered_; // 与 packs_ 同下标
    std::vector<std::string> draft_;
    std::vector<std::string> active_;
    PackManager manager_;
    std::optional<LayeredResourceProvider> stack_;
};

} // namespace mc::assets
