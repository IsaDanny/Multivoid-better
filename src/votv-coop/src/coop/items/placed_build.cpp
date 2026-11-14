// coop/items/placed_build.cpp -- see the header for the authority and the one client-authored act.

#include "coop/items/placed_build.h"

#include "coop/element/intent_authority.h"
#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/session/host_world_settle.h"

#include "ue_wrap/actors/placed_build.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/class_resolve.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/keyed_objects.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::placed_build {
namespace {

namespace PB = ue_wrap::placed_build;
namespace W  = coop::save_record_wire;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;

using SaveRecord = ue_wrap::save_record::SaveRecord;
using Ref        = ue_wrap::CachedObjRef;

// The one client-authored act: the wall builder placing a frame.
UE_FACT_FN(kRowAddWall, "placed_build", L"prop_wallbuilder_C", L"addWall", Declared);
constexpr int kTagAddWall = 0x50424157;  // 'PBAW'
// The verbs that change a wall. A client's copy runs none of them; the host's result arrives instead.
UE_FACT_FN(kRowFrameOverlap, "placed_build", L"customWall_unfinished_C",
           L"BndEvt__customWall_unfinished_overlap_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
constexpr int kTagFrameOverlap = 0x50424f56;  // 'PBOV'
UE_FACT_FN(kRowBuildWall, "placed_build", L"customWall_unfinished_C", L"buildWall", Declared);
constexpr int kTagBuildWall = 0x50424257;  // 'PBBW'
UE_FACT_FN(kRowConcreteTick, "placed_build", L"customWall_wetConcrete_C", L"ReceiveTick", Declared);
constexpr int kTagConcreteTick = 0x50425754;  // 'PBWT'
UE_FACT_FN(kRowWallDamage, "placed_build", L"customWall_C", L"Damage", Declared);
constexpr int kTagWallDamage = 0x50424447;  // 'PBDG'

// The wall builder places a frame where its two pivots stand, a few metres from the player; twice
// the longest wall it draws is the reach a commit may claim.
constexpr float kReachUU = 3000.f;
constexpr coop::net::IntentBudget kBudget{6.0f, 1.0f};
// Every standing wall is an actor that persists in the host's save, so the table is a bound on
// the world a client can grow.
constexpr size_t kMaxPerSlot = 256;
// A commit the host never answers (the session dropped it) leaves the client's frame standing
// alone. After this long the frame goes, since the host does not have it.
constexpr uint64_t kCommitTimeoutMs = 15000;
constexpr uint64_t kSweepEveryMs = 1000;
constexpr auto     kAssemblyTtl  = std::chrono::seconds(20);

std::atomic<coop::net::Session*> g_session{nullptr};
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_listening = false;

coop::blob_chunks::Assembler g_asmCommit;
coop::blob_chunks::Assembler g_asmSpawn;
uint32_t g_blobSeq   = 0;
uint32_t g_keyNonce  = 0;
uint32_t g_keyNext   = 0;
uint16_t g_commitSeq = 0;
uint64_t g_lastSweep = 0;

// Births the listener saw, judged on the next tick: a listener may not create or destroy objects.
std::vector<Ref> g_births;
// Actors this lane spawned itself (a host canonical, a client copy): their births are not news.
std::unordered_set<void*> g_ownSpawns;
// While a client's addWall body runs, the frames it places are this player's build.
int g_addWallDepth = 0;
std::vector<Ref> g_ownBuilds;

// HOST: every wall the host holds, by the key every copy shares.
struct Canon {
    Ref          ref;
    void*        raw = nullptr;  // the death compare: a dead object is compared, never read
    std::wstring key;
    uint8_t      ownerSlot = 0;
};
std::vector<Canon> g_canon;
std::vector<std::wstring> g_deadKeys;
bool g_seeded = false;
std::array<coop::net::IntentBucket, coop::players::kMaxPeers> g_buckets{};

// CLIENT: frames committed and not yet answered, and the copies the host's word built.
struct Awaiting {
    Ref      ref;
    uint16_t seq    = 0;
    uint64_t sentMs = 0;
};
std::vector<Awaiting> g_awaiting;
struct Copy {
    Ref          ref;
    std::wstring key;
};
std::vector<Copy> g_copies;

coop::net::Session* Session() { return g_session.load(std::memory_order_acquire); }
bool Running() { auto* s = Session(); return s && s->running(); }
bool IsHost() { auto* s = Session(); return s && s->running() && s->role() == coop::net::Role::Host; }
bool IsClient() { auto* s = Session(); return s && s->running() && s->role() == coop::net::Role::Client; }

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool InWorld(float v) { return std::isfinite(v) && std::fabs(v) <= coop::net::kMaxCoord; }

// Every number a record carries is finite and its location is in the world: refused whole otherwise.
bool RecordSane(const SaveRecord& r) {
    for (size_t i = 0; i < r.xform.size(); ++i) {
        if (!std::isfinite(r.xform[i])) return false;
        if (i >= 4 && i <= 6 && !InWorld(r.xform[i])) return false;
    }
    for (const auto& g : r.floats)
        for (float f : g)
            if (!std::isfinite(f)) return false;
    for (const auto& g : r.vectors)
        for (const auto& v : g)
            if (!InWorld(v[0]) || !InWorld(v[1]) || !InWorld(v[2])) return false;
    return true;
}

// u16 seq | u8 classId | u8 originSlot | SaveRecord
void Serialize(std::vector<uint8_t>& b, uint16_t seq, PB::Kind kind, uint8_t origin, const SaveRecord& r) {
    W::AppU16(b, seq);
    W::AppU8(b, static_cast<uint8_t>(kind));
    W::AppU8(b, origin);
    W::SerSave(b, r);
}

bool Parse(const std::vector<uint8_t>& b, uint16_t& seq, PB::Kind& kind, uint8_t& origin, SaveRecord& r) {
    size_t o = 0;
    uint8_t cls = 0;
    if (!W::RdU16(b, o, seq) || !W::RdU8(b, o, cls) || !W::RdU8(b, o, origin)) return false;
    if (cls >= static_cast<uint8_t>(PB::Kind::Count)) return false;
    kind = static_cast<PB::Kind>(cls);
    return W::DeSave(b, o, r);
}

bool KeyFits(const std::wstring& k) {
    return !k.empty() && k != L"None" && k.size() <= sizeof(coop::net::WireKey{}.data);
}

// Unique against the save, not just this session: the counter restarts each launch while the key
// persists, so the per-session nonce keeps a new wall from taking an old one's name.
std::wstring MintKey() {
    wchar_t buf[32];
    swprintf(buf, 32, L"mvwall_%08x_%u", g_keyNonce, ++g_keyNext);
    return std::wstring(buf);
}

// ---- host ----------------------------------------------------------------------------------------

Canon* FindCanonByRaw(void* raw) {
    for (auto& c : g_canon)
        if (c.raw == raw) return &c;
    return nullptr;
}

size_t CanonForSlot(uint8_t slot) {
    size_t n = 0;
    for (const auto& c : g_canon)
        if (c.ownerSlot == slot) ++n;
    return n;
}

void BroadcastSpawn(uint16_t seq, PB::Kind kind, uint8_t origin, const SaveRecord& r) {
    std::vector<uint8_t> blob;
    Serialize(blob, seq, kind, origin, r);
    if (!coop::blob_chunks::SendBlob(Session(), coop::net::ReliableKind::PlacedBuildSpawn, ++g_blobSeq, blob))
        UE_LOGW("placed_build: the spawn of '%ls' did not send -- the clients do not have that wall", r.key.c_str());
}

// Take a wall the host holds into the table, naming it first if the game left it without a key
// the wire can carry. Returns the record as it stands after the naming.
bool Adopt(void* actor, uint8_t ownerSlot, SaveRecord& rec) {
    if (!PB::Capture(actor, rec)) return false;
    if (!KeyFits(rec.key)) {
        rec.key = MintKey();
        if (!PB::Apply(actor, rec)) return false;
    }
    Canon c;
    c.ref.Set(actor);
    c.raw       = actor;
    c.key       = rec.key;
    c.ownerSlot = ownerSlot;
    g_canon.push_back(std::move(c));
    return true;
}

void SeedFromWorld() {
    for (size_t i = 0; i < static_cast<size_t>(PB::Kind::Count); ++i) {
        const auto rc = ue_wrap::class_resolve::ResolveClass(PB::ClassNameOf(static_cast<PB::Kind>(i)),
                                                             ue_wrap::class_resolve::AnyFamily(),
                                                             ue_wrap::class_resolve::Load::LookupOnly);
        if (rc.state != ue_wrap::class_resolve::ClassState::Found) continue;
        OI::ForEachInstance(rc.cls, [](void*, void* obj, int32_t) {
            if (PB::KindOf(obj) != PB::Kind::Count) g_births.emplace_back().Set(obj);
        }, nullptr);
    }
}

void TickHost() {
    if (!g_seeded) {
        if (!coop::host_world_settle::Settled()) return;
        SeedFromWorld();
        g_seeded = true;
    }
    for (auto& b : g_births) {
        void* actor = b.Get();
        if (!actor || FindCanonByRaw(actor)) continue;
        const PB::Kind kind = PB::KindOf(actor);
        SaveRecord rec;
        if (!Adopt(actor, 0, rec)) {
            UE_LOGW("placed_build: a %ls of the host's did not give its record -- the clients do not see it",
                    PB::ClassNameOf(kind));
            continue;
        }
        BroadcastSpawn(0, kind, 0, rec);
    }
    g_births.clear();
    for (const auto& k : g_deadKeys) {
        coop::net::PlacedBuildDestroyPayload p{};
        coop::net::WireKeyFromString(k, p.key);
        Session()->SendReliable(coop::net::ReliableKind::PlacedBuildDestroy, &p, sizeof(p));
    }
    g_deadKeys.clear();
}

// ---- client --------------------------------------------------------------------------------------

void DropCopy(const std::wstring& key) {
    for (size_t i = 0; i < g_copies.size(); ++i) {
        if (g_copies[i].key != key) continue;
        if (void* a = g_copies[i].ref.Get()) E::DestroyActor(a);
        g_copies.erase(g_copies.begin() + static_cast<ptrdiff_t>(i));
        return;
    }
    // A copy the joiner's save loaded is the game's own keyed actor.
    if (void* a = ue_wrap::keyed_objects::Resolve(key.c_str())) E::DestroyActor(a);
}

void TickClient() {
    auto* s = Session();
    const uint64_t now = NowMs();
    for (auto& b : g_ownBuilds) {
        void* frame = b.Get();
        if (!frame) continue;
        const PB::Kind kind = PB::KindOf(frame);
        SaveRecord rec;
        if (!PB::Capture(frame, rec) || !RecordSane(rec)) {
            UE_LOGW("placed_build: the frame this player placed did not give its record -- it stays here alone");
            continue;
        }
        if (++g_commitSeq == 0) g_commitSeq = 1;
        std::vector<uint8_t> blob;
        Serialize(blob, g_commitSeq, kind, 0, rec);
        if (!coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::PlacedBuildCommit, ++g_blobSeq, blob)) {
            UE_LOGW("placed_build: the commit of a placed frame did not send -- the frame goes");
            E::DestroyActor(frame);
            continue;
        }
        g_awaiting.push_back(Awaiting{b, g_commitSeq, now});
    }
    g_ownBuilds.clear();
    for (size_t i = 0; i < g_awaiting.size();) {
        if (now - g_awaiting[i].sentMs < kCommitTimeoutMs) { ++i; continue; }
        UE_LOGW("placed_build: the host never answered commit %u -- the frame goes, the host does not have it",
                static_cast<unsigned>(g_awaiting[i].seq));
        if (void* a = g_awaiting[i].ref.Get()) E::DestroyActor(a);
        g_awaiting.erase(g_awaiting.begin() + static_cast<ptrdiff_t>(i));
    }
}

// ---- the object index and the script gate --------------------------------------------------------

void OnCreated(void*, void* obj, void*, int32_t) {
    if (!Running() || PB::KindOf(obj) == PB::Kind::Count) return;
    if (g_ownSpawns.erase(obj)) return;
    if (IsHost()) {
        g_births.emplace_back().Set(obj);
    } else if (g_addWallDepth > 0) {
        g_ownBuilds.emplace_back().Set(obj);
    }
}

void OnDeleted(void*, void* obj, void*, int32_t) {
    g_ownSpawns.erase(obj);
    if (!IsHost()) return;
    for (size_t i = 0; i < g_canon.size(); ++i) {
        if (g_canon[i].raw != obj) continue;
        g_deadKeys.push_back(g_canon[i].key);
        g_canon.erase(g_canon.begin() + static_cast<ptrdiff_t>(i));
        return;
    }
}

sg::Verdict OnAddWallPre(const sg::Call&) {
    if (IsClient()) ++g_addWallDepth;
    return sg::Verdict::Run;
}

// The frames addWall placed are drained here, inside the bracket, so the listener attributes them.
void OnAddWallPost(const sg::Call&) {
    if (g_addWallDepth <= 0) return;
    OI::DrainAll();
    --g_addWallDepth;
}

sg::Verdict RefuseOnClient(const sg::Call&) { return IsClient() ? sg::Verdict::Cancel : sg::Verdict::Run; }
void NoPost(const sg::Call&) {}

void RegisterWatches() {
    if (g_reg != Reg::Pending) return;
    if (sg::WatchName(kRowAddWall, kTagAddWall, &OnAddWallPre, &OnAddWallPost) &&
        sg::WatchName(kRowFrameOverlap, kTagFrameOverlap, &RefuseOnClient, &NoPost) &&
        sg::WatchName(kRowBuildWall, kTagBuildWall, &RefuseOnClient, &NoPost) &&
        sg::WatchName(kRowConcreteTick, kTagConcreteTick, &RefuseOnClient, &NoPost) &&
        sg::WatchName(kRowWallDamage, kTagWallDamage, &RefuseOnClient, &NoPost)) {
        g_reg = Reg::Registered;
        return;
    }
    g_reg = Reg::Refused;
    UE_LOGE("placed_build: the script-body gate refused a watch -- built walls stay on the machine that built them");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_keyNonce == 0) {
        std::random_device rd;
        g_keyNonce = static_cast<uint32_t>(rd()) | 1u;
    }
    if (!g_listening) {
        g_listening = OI::AddListener(OI::Listener{nullptr, &OnCreated, &OnDeleted});
        if (!g_listening) UE_LOGE("placed_build: the object index has no listener slot left -- walls are not shared");
    }
}

void Tick() {
    if (!Running() || !PB::ResolveNames()) return;
    RegisterWatches();
    const uint64_t now = NowMs();
    if (now - g_lastSweep >= kSweepEveryMs) {
        g_lastSweep = now;
        const auto t = std::chrono::steady_clock::now();
        g_asmCommit.Sweep(t, kAssemblyTtl);
        g_asmSpawn.Sweep(t, kAssemblyTtl);
    }
    if (IsHost()) TickHost();
    else          TickClient();
}

void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!GT::IsGameThread() || !IsHost()) return;
    if (senderSlot == 0 || senderSlot >= coop::players::kMaxPeers) return;
    std::vector<uint8_t> blob;
    if (!g_asmCommit.OnChunk(p, senderSlot, blob)) return;

    uint16_t seq = 0; PB::Kind kind = PB::Kind::Count; uint8_t unused = 0;
    SaveRecord rec;
    if (!Parse(blob, seq, kind, unused, rec) || seq == 0 || kind != PB::Kind::WallUnfinished || !RecordSane(rec)) {
        UE_LOGW("placed_build: a malformed commit from slot %u -- refused", static_cast<unsigned>(senderSlot));
        return;
    }
    auto* s = Session();
    const ue_wrap::FVector at{rec.xform[4], rec.xform[5], rec.xform[6]};
    const auto tok = coop::element::IntentTarget::ForClientIntent(*s, senderSlot, kReachUU);
    const coop::element::IntentSubject sub = tok.AuthorizeSegment(at, at);
    if (!sub) {
        UE_LOGW("placed_build: slot %u placed a frame it could not reach (%s, %.0f of %.0f uu) -- refused",
                static_cast<unsigned>(senderSlot), coop::element::OutcomeName(sub.outcome), sub.distUU, sub.reachUU);
        return;
    }
    if (!g_buckets[senderSlot].Take(kBudget, NowMs()) || CanonForSlot(senderSlot) >= kMaxPerSlot) {
        UE_LOGW("placed_build: slot %u is past its build budget -- refused", static_cast<unsigned>(senderSlot));
        return;
    }
    // Identity is the host's: the class comes from the Kind, the key is minted here.
    rec.className = PB::ClassNameOf(kind);
    rec.key       = MintKey();
    void* frame = PB::SpawnFromRecord(kind, rec, ue_wrap::save_record::ClassLoad::LookupOnly);
    if (!frame) {
        UE_LOGW("placed_build: the frame slot %u placed could not be built here -- refused",
                static_cast<unsigned>(senderSlot));
        return;
    }
    g_ownSpawns.insert(frame);
    Canon c;
    c.ref.Set(frame);
    c.raw       = frame;
    c.key       = rec.key;
    c.ownerSlot = senderSlot;
    g_canon.push_back(std::move(c));
    UE_LOGI("placed_build: slot %u's frame is the host's as '%ls'", static_cast<unsigned>(senderSlot), rec.key.c_str());
    BroadcastSpawn(seq, kind, senderSlot, rec);
}

void OnSpawnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!GT::IsGameThread() || IsHost() || senderSlot != 0) return;
    std::vector<uint8_t> blob;
    if (!g_asmSpawn.OnChunk(p, senderSlot, blob)) return;

    uint16_t seq = 0; PB::Kind kind = PB::Kind::Count; uint8_t origin = 0;
    SaveRecord rec;
    if (!Parse(blob, seq, kind, origin, rec) || !RecordSane(rec) || !KeyFits(rec.key)) return;
    if (origin >= coop::players::kMaxPeers) return;

    // The host's answer to this player's own commit retires the frame the player placed.
    if (seq != 0 && origin == coop::players::Registry::Get().LocalPeerId()) {
        for (size_t i = 0; i < g_awaiting.size(); ++i) {
            if (g_awaiting[i].seq != seq) continue;
            if (void* a = g_awaiting[i].ref.Get()) E::DestroyActor(a);
            g_awaiting.erase(g_awaiting.begin() + static_cast<ptrdiff_t>(i));
            break;
        }
    }
    for (const auto& c : g_copies)
        if (c.key == rec.key && c.ref.Alive()) return;
    if (ue_wrap::keyed_objects::Resolve(rec.key.c_str())) return;  // the save gave this peer the wall already

    rec.className = PB::ClassNameOf(kind);
    void* copy = PB::SpawnFromRecord(kind, rec, ue_wrap::save_record::ClassLoad::Immediate);
    if (!copy) return;
    g_ownSpawns.insert(copy);
    Copy c;
    c.ref.Set(copy);
    c.key = rec.key;
    g_copies.push_back(std::move(c));
}

void OnDestroy(const coop::net::PlacedBuildDestroyPayload& p) {
    if (!GT::IsGameThread() || IsHost()) return;
    DropCopy(coop::net::StringFromWireKey(p.key));
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    if (!GT::IsGameThread() || !IsHost() || peerSlot <= 0) return;
    size_t sent = 0;
    for (const auto& c : g_canon) {
        void* actor = c.ref.Get();
        if (!actor) continue;
        SaveRecord rec;
        if (!PB::Capture(actor, rec)) continue;
        std::vector<uint8_t> blob;
        Serialize(blob, 0, PB::KindOf(actor), 0, rec);
        if (coop::blob_chunks::SendBlobToSlot(Session(), peerSlot, coop::net::ReliableKind::PlacedBuildSpawn,
                                              ++g_blobSeq, blob))
            ++sent;
    }
    if (sent) UE_LOGI("placed_build: replayed %zu walls to slot %d", sent, peerSlot);
}

void OnPeerLeftSlot(int slot) {
    if (slot < 0 || slot >= coop::players::kMaxPeers) return;
    g_asmCommit.ClearSlot(static_cast<uint8_t>(slot));
    g_asmSpawn.ClearSlot(static_cast<uint8_t>(slot));
    g_buckets[static_cast<size_t>(slot)].Reset();
}

void OnDisconnect() {
    g_asmCommit.Clear();
    g_asmSpawn.Clear();
    g_births.clear();
    g_ownBuilds.clear();
    g_ownSpawns.clear();
    g_canon.clear();
    g_deadKeys.clear();
    g_awaiting.clear();
    g_copies.clear();
    g_seeded = false;
    g_addWallDepth = 0;
    for (auto& b : g_buckets) b.Reset();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::placed_build
