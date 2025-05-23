#include "mod/mod.h"

#include "ll/api/mod/RegisterHelper.h"
#include "nlohmann/json.hpp"

void init();
void deinit();

namespace mod {

Mod& Mod::getInstance() {
    static Mod instance;
    return instance;
}

bool Mod::load() {
    getSelf().getLogger().debug("Loading...");
    // Code for loading the mod goes here.
    getSelf().getLogger().info("Thirst by killcerr loaded");
    getSelf().getLogger().debug("Loaded...");
    return true;
}

bool Mod::enable() {
    getSelf().getLogger().debug("Enabling...");
    // Code for enabling the mod goes here.
    init();
    getSelf().getLogger().debug("Enabled...");
    return true;
}

bool Mod::disable() {
    getSelf().getLogger().debug("Disabling...");
    // Code for disabling the mod goes here.
    deinit();
    getSelf().getLogger().debug("Disabled...");
    return true;
}

} // namespace mod

LL_REGISTER_MOD(mod::Mod, mod::Mod::getInstance());
#include "ll/api/Config.h"
#include "ll/api/chrono/GameChrono.h"
#include "ll/api/coro/CoroTask.h"
#include "ll/api/event/Emitter.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDieEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerInteractBlockEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/player/PlayerUseItemEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/service/GamingStatus.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core/utility/MCRESULT.h"             // IWYU pragma: keep
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h" // IWYU pragma: keep
#include "mc/legacy/ActorUniqueID.h"
#include "mc/nbt/CompoundTag.h" // IWYU pragma: keep
#include "mc/network/packet/SetTitlePacket.h"
#include "mc/platform/UUID.h" // IWYU pragma: keep
#include "mc/server/commands/CommandContext.h"
#include "mc/server/commands/CommandVersion.h"
#include "mc/server/commands/MinecraftCommands.h"
#include "mc/server/commands/PlayerCommandOrigin.h"
#include "mc/util/ExpressionNode.h"
#include "mc/util/MolangScriptArg.h"
#include "mc/util/MolangVariable.h"
#include "mc/util/MolangVariableMap.h"
#include "mc/world/Minecraft.h"
#include "mc/world/actor/RenderParams.h"
#include "mc/world/actor/player/PlayerItemInUse.h"
#include "mc/world/actor/provider/SynchedActorDataAccess.h"
#include "mc/world/events/ServerInstanceEventCoordinator.h"
#include "mc/world/item/BucketItem.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/PotionItem.h"
#include "mc/world/level/ActorEventBroadcaster.h" // IWYU pragma: keep
#include "mc/world/level/BlockSource.h"           // IWYU pragma: keep
#include "mc/world/level/Level.h"
#include "mc/world/level/biome/Biome.h"
#include "mc/world/level/biome/registry/BiomeRegistry.h"
#include "mc/world/level/biome/source/BiomeSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/levelgen/WorldGenerator.h"
#include "mc/world/phys/HitResult.h" // IWYU pragma: keep


#include <charconv>
#include <ranges> // IWYU pragma: keep

struct Modification {
    int                      value;
    std::vector<std::string> commands;
};
std::unordered_map<mce::UUID, int>                                                        current_thirsts;
std::unordered_map<std::string, Modification>                                             modifications;
std::unordered_map<std::string, std::vector<std::string>>                                 commands;
std::unordered_map<int, std::string>                                                      texts;
std::unordered_map<mce::UUID, bool>                                                       show_thirsts;
int                                                                                       thirst_base;
int                                                                                       thirst_max;
int                                                                                       thirst_min;
int                                                                                       thirst_tick;
phmap::flat_hash_map<mce::UUID, phmap::flat_hash_map<std::size_t, std::function<void()>>> on_tick_callbacks;


void init_molang() {
    ExpressionNode::registerQueryFunction(
        "query.is_biome",
        [&](RenderParams& params, const std::vector<ExpressionNode>& args) -> MolangScriptArg const& {
            auto biome =
                params.mActor->getDimension().mWorldGenerator->getBiomeSource().getBiome(params.mActor->getPosition());
            static const MolangScriptArg return_true(true), return_false(false);
            for (const auto& arg : args) {
                const auto& tag = arg.evalGeneric(params);
                if (biome->mHash->getHash() == tag.mPOD.mHashType64) {
                    return return_true;
                }
            }
            return return_false;
        },
        "",
        MolangQueryFunctionReturnType::Number,
        "default",
        0,
        ~0ull,
        {}
    );
}

bool exec_molang(Player* player, const std::string& script) {
    player->mMolangVariables->setMolangVariable(
        "variable.current_thirst",
        static_cast<float>(current_thirsts[player->getUuid()])
    );

    auto res = player->evalMolang(script) != 0.0;

    current_thirsts[player->getUuid()] = static_cast<int>(player->mMolangVariables
                                                              ->_getOrAddMolangVariable(
                                                                  HashedString{"variable.current_thirst"}.getHash(),
                                                                  "variable.current_thirst",
                                                                  true
                                                              )
                                                              ->mValue->mPOD.mFloat);
    return res;
}

void exec_cmds(Player* player, const std::vector<std::string>& cmds) {
    for (const auto& cmd : cmds) {
        CommandContext context =
            CommandContext(cmd, std::make_unique<PlayerCommandOrigin>(*player), CommandVersion::CurrentVersion());
        ll::service::getMinecraft()->mCommands->executeCommand(context, false);
    }
}

nlohmann::json& data() {
    static nlohmann::json data = [] {
        auto dir = mod::Mod::getInstance().getSelf().getDataDir();
        if (!std::filesystem::exists(dir)) std::filesystem::create_directories(dir);
        if (!std::filesystem::exists(dir / "data.json")) std::ofstream(dir / "data.json") << "{}";
        return nlohmann::json::parse(std::ifstream(dir / "data.json"), nullptr, true, false);
    }();
    return data;
}

nlohmann::json& set_data(const std::string& uuid, int thirst) {
    data()[uuid] = thirst;
    return data();
}

void load_config() {
    struct {
        int                                                       version       = 0;
        std::unordered_map<std::string, Modification>             modifications = {};
        std::unordered_map<std::string, std::vector<std::string>> commands      = {};
        std::unordered_map<std::string, std::string>              texts         = {};
        int                                                       thirst_base   = 20;
        int                                                       thirst_max    = 20;
        int                                                       thirst_min    = 0;
        int                                                       thirst_tick   = 200;
    } config;
    auto dir = mod::Mod::getInstance().getSelf().getConfigDir();
    if (!std::filesystem::exists(dir)) std::filesystem::create_directories(dir);
    if (!std::filesystem::exists(dir / "config.json")) ll::config::saveConfig(config, dir / "config.json");
    ll::config::loadConfig(config, dir / "config.json");
    modifications = config.modifications;
    commands      = config.commands;
    std::transform(config.texts.begin(), config.texts.end(), std::inserter(texts, texts.end()), [](auto&& val) {
        int thirst;
        std::from_chars(val.first.data(), val.first.data() + val.first.size(), thirst);
        return std::make_pair(thirst, val.second);
    });
    thirst_base = config.thirst_base;
    thirst_max  = config.thirst_max;
    thirst_min  = config.thirst_min;
    thirst_tick = config.thirst_tick;
}
struct EatEvent final : ll::event::PlayerEvent {
    ItemStack& item;
    constexpr explicit EatEvent(Player& player, ItemStack& item) : ll::event::PlayerEvent(player), item(item) {}
};
class EatEventEmitter : public ll::event::Emitter<[](auto&&...) { return nullptr; }, EatEvent> {};
LL_AUTO_TYPE_INSTANCE_HOOK(PlayerEatHook, HookPriority::Normal, Player, &Player::eat, void, ItemStack const& instance) {
    ll::event::EventBus::getInstance().publish(EatEvent{*this, const_cast<ItemStack&>(instance)});
    origin(instance);
}

// 药水
LL_AUTO_TYPE_INSTANCE_HOOK(
    PotionUseHook,
    HookPriority::Normal,
    PotionItem,
    &PotionItem::$useTimeDepleted,
    ::ItemUseMethod,
    ::ItemStack& inoutInstance,
    Level*       level,
    Player*      player
) {
    if (player) {
        ll::event::EventBus::getInstance().publish(EatEvent{*player, inoutInstance});
    }
    return origin(inoutInstance, level, player);
}

// 牛奶
LL_AUTO_TYPE_INSTANCE_HOOK(
    MilkBucketUseHook,
    HookPriority::Normal,
    Item,
    (uintptr_t)BucketItem::$vftable()[79],
    ::ItemUseMethod,
    ::ItemStack& inoutInstance,
    Level*       level,
    Player*      player
) {
    if (player && inoutInstance.getTypeName() == "minecraft:milk_bucket") {
        ll::event::EventBus::getInstance().publish(EatEvent{*player, inoutInstance});
    }
    return origin(inoutInstance, level, player);
}


void show_text(Player* player) {
    if (!show_thirsts[player->getUuid()]) {
        SetTitlePacket packet{SetTitlePacket::TitleType::Actionbar, "", std::nullopt};
        packet.mFadeInTime  = 0;
        packet.mFadeOutTime = 1;
        packet.mStayTime    = 1;
        player->sendNetworkPacket(packet);
    } else {
        auto           val = current_thirsts[player->getUuid()];
        SetTitlePacket packet{SetTitlePacket::TitleType::Actionbar, texts[val], std::nullopt};
        packet.mFadeInTime  = 0;
        packet.mFadeOutTime = INT_MAX;
        packet.mStayTime    = INT_MAX;
        player->sendNetworkPacket(packet);
    }
}
LL_AUTO_TYPE_INSTANCE_HOOK(
    ServerStartHook,
    HookPriority::Normal,
    ServerInstanceEventCoordinator,
    &ServerInstanceEventCoordinator::sendServerInitializeEnd,
    void,
    ::ServerInstance& ins
) {
    init_molang();
    return origin(ins);
}
bool isSneaking(Actor* actor) {
    return SynchedActorDataAccess::getActorFlag(actor->getEntityContext(), ActorFlags::Sneaking);
}
void init() {
    data();
    load_config();
    ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerJoinEvent>([](auto& event) {
        Player*   player = std::addressof(event.self());
        mce::UUID uuid   = event.self().getUuid();
        if (data().contains(uuid.asString())) current_thirsts[uuid] = data()[uuid.asString()];
        else current_thirsts[uuid] = thirst_base;
        if (!show_thirsts.contains(uuid)) show_thirsts[uuid] = true;
        ll::coro::keepThis([player, uuid]() -> ll::coro::CoroTask<> {
            while (ll::getGamingStatus() == ll::GamingStatus::Running && current_thirsts.contains(uuid)) {
                if (!current_thirsts.contains(uuid)) co_return;
                if (!player->isAlive() || player->getPlayerGameType() == GameType::Creative
                    || player->getPlayerGameType() == GameType::Spectator) {
                    co_await ll::chrono::ticks(1);
                    continue;
                }
                if (current_thirsts[uuid] > thirst_max) current_thirsts[uuid] = thirst_max;
                if (current_thirsts[uuid] < thirst_min) current_thirsts[uuid] = thirst_min;
                show_text(player);
                for (const auto& [molang, cmds] : commands) {
                    if (exec_molang(static_cast<Player*>(player), molang)) exec_cmds(player, cmds);
                }
                if (on_tick_callbacks.contains(uuid)) {
                    for (auto& [_, callback] : on_tick_callbacks[uuid]) {
                        callback();
                    }
                }
                co_await ll::chrono::ticks(thirst_tick);
            }
            co_return;
        }).launch(ll::thread::ServerThreadExecutor::getDefault());
    });
    ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerDisconnectEvent>([](auto& event) {
        auto uuid = event.self().getUuid();
        auto val  = current_thirsts[uuid];
        current_thirsts.erase(uuid);
        auto uuidstr = uuid.asString();
        set_data(uuidstr, val);
        on_tick_callbacks.erase(uuid);
    });
    ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerUseItemEvent>([](auto& event) {
        if (event.item().getItem()->isFood() || event.item().isPotionItem()
            || event.item().getTypeName() == "minecraft:milk_bucket")
            return;
        if (!isSneaking(&event.self())) return;
        mod::Mod::getInstance().getSelf().getLogger().info(event.item().getTypeName());
        if (auto hitResult = event.self().traceRay(5.5f, false, true); hitResult.mIsHitLiquid) {
            auto& block = event.self().getDimensionBlockSource().getBlock(hitResult.mLiquid);
            auto  name  = block.getTypeName();
            if (modifications.contains(name)) {
                current_thirsts[event.self().getUuid()] += modifications[name].value;
                exec_cmds(&event.self(), modifications[name].commands);
            } else if (name = name.substr(name.find(":") + 1); modifications.contains(name)) {
                current_thirsts[event.self().getUuid()] += modifications[name].value;
                exec_cmds(&event.self(), modifications[name].commands);
            } else {
                auto& extra_block = event.self().getDimensionBlockSource().getBlock(hitResult.mLiquid, 1);
                name              = extra_block.getTypeName();
                if (modifications.contains(name)) {
                    current_thirsts[event.self().getUuid()] += modifications[name].value;
                    exec_cmds(&event.self(), modifications[name].commands);
                } else {
                    name = name.substr(name.find(":") + 1);
                    if (modifications.contains(name)) {
                        current_thirsts[event.self().getUuid()] += modifications[name].value;
                        exec_cmds(&event.self(), modifications[name].commands);
                    }
                }
            }
        }
        auto name = event.item().getTypeName();
        if (modifications.contains(name)) {
            current_thirsts[event.self().getUuid()] += modifications[name].value;
            exec_cmds(&event.self(), modifications[name].commands);
            const_cast<ItemStack*>(event.self().getAllHand()[0])->remove(1);
        } else {
            name = name.substr(name.find(":") + 1);
            if (modifications.contains(name)) {
                current_thirsts[event.self().getUuid()] += modifications[name].value;
                exec_cmds(&event.self(), modifications[name].commands);
                const_cast<ItemStack*>(event.self().getAllHand()[0])->remove(1);
            } else {
                return;
            }
        }
        if (current_thirsts[event.self().getUuid()] > thirst_max) current_thirsts[event.self().getUuid()] = thirst_max;
        if (current_thirsts[event.self().getUuid()] < thirst_min) current_thirsts[event.self().getUuid()] = thirst_min;
    });
    ll::event::EventBus::getInstance().emplaceListener<EatEvent>([](auto& event) {
        auto name = event.item.getTypeName();
        if (modifications.contains(name)) {
            current_thirsts[event.self().getUuid()] += modifications[name].value;
            exec_cmds(&event.self(), modifications[name].commands);
        } else {
            name = name.substr(name.find(":") + 1);
            if (modifications.contains(name)) {
                current_thirsts[event.self().getUuid()] += modifications[name].value;
                exec_cmds(&event.self(), modifications[name].commands);
            } else {
                return;
            }
        }
        if (current_thirsts[event.self().getUuid()] > thirst_max) current_thirsts[event.self().getUuid()] = thirst_max;
        if (current_thirsts[event.self().getUuid()] < thirst_min) current_thirsts[event.self().getUuid()] = thirst_min;
    });
    ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerInteractBlockEvent>([](auto& event) {
        if (!isSneaking(&event.self())) return;
        static phmap::flat_hash_map<ActorUniqueID, std::chrono::time_point<std::chrono::steady_clock>>
            last_trigger_times;
        using namespace std::literals;
        const auto& id = event.self().getOrCreateUniqueID();
        if (last_trigger_times.contains(id)) {
            auto& last_trigger_time = last_trigger_times[id];
            if (std::chrono::steady_clock::now() - last_trigger_time < 0.25s) {
                return;
            } else {
                last_trigger_time = std::chrono::steady_clock::now();
            }
        } else {
            last_trigger_times[id] = std::chrono::steady_clock::now();
        }

        if (auto hitResult = event.self().traceRay(5.5f, false, true); hitResult.mIsHitLiquid) {
            auto& block = event.self().getDimensionBlockSource().getBlock(hitResult.mLiquid);
            auto  name  = block.getTypeName();
            if (modifications.contains(name)) {
                current_thirsts[event.self().getUuid()] += modifications[name].value;
                exec_cmds(&event.self(), modifications[name].commands);
            } else if (name = name.substr(name.find(":") + 1); modifications.contains(name)) {
                current_thirsts[event.self().getUuid()] += modifications[name].value;
                exec_cmds(&event.self(), modifications[name].commands);
            } else {
                auto& extra_block = event.self().getDimensionBlockSource().getBlock(hitResult.mLiquid, 1);
                name              = extra_block.getTypeName();
                if (modifications.contains(name)) {
                    current_thirsts[event.self().getUuid()] += modifications[name].value;
                    exec_cmds(&event.self(), modifications[name].commands);
                } else {
                    name = name.substr(name.find(":") + 1);
                    if (modifications.contains(name)) {
                        current_thirsts[event.self().getUuid()] += modifications[name].value;
                        exec_cmds(&event.self(), modifications[name].commands);
                    }
                }
            }
        }
    });
    ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerDieEvent>(
        [](auto& event) { current_thirsts[event.self().getUuid()] = thirst_base + 1; },
        ll::event::EventPriority::Highest
    );
}
void deinit() {
    for (const auto& [uuid, value] : current_thirsts) {
        set_data(uuid.asString(), value);
    }
    std::ofstream(mod::Mod::getInstance().getSelf().getDataDir() / "data.json") << data();
}
__declspec(dllexport) int get_thirst(const std::string& uuid) {
    if (auto key = mce::UUID::fromString(uuid); current_thirsts.contains(key)) return current_thirsts[key];
    if (data().contains(uuid)) return data()[uuid].get<int>();
    return -1;
}
__declspec(dllexport) void set_thirst(const std::string& uuid, int value) {
    if (value < thirst_min) {
        value = thirst_min;
    } else if (value > thirst_max) {
        value = thirst_max;
    }
    if (auto key = mce::UUID::fromString(uuid); current_thirsts.contains(key)) {
        current_thirsts[key] = value;
        show_text(ll::service::getLevel()->getPlayer(key));
    } else data()[uuid] = value;
}
__declspec(dllexport) void set_show_thirst(const std::string& uuid, bool value) {
    show_thirsts[mce::UUID::fromString(uuid)] = value;
}
__declspec(dllexport) std::function<bool(void)>
                      register_on_tick_callback(const std::string& uuid, std::function<void()>&& callback) {
    static std::size_t idx = 0;
    if (auto key = mce::UUID::fromString(uuid); current_thirsts.contains(key)) {
        on_tick_callbacks[key][idx] = callback;
        return std::bind(
            [](mce::UUID uuid, std::size_t idx) -> bool {
                if (on_tick_callbacks.contains(uuid) && on_tick_callbacks[uuid].contains(idx)) {
                    on_tick_callbacks[uuid].erase(idx);
                    return true;
                }
                return false;
            },
            key,
            idx++
        );
    }
    return nullptr;
}