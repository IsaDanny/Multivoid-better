// ue_wrap/actors/placed_build.cpp -- see the header.

#include "ue_wrap/actors/placed_build.h"

#include "ue_wrap/core/class_resolve.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor

#include <cmath>
#include <cstddef>

namespace ue_wrap::placed_build {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace SR = ue_wrap::save_record;
namespace CR = ue_wrap::class_resolve;

constexpr size_t kCount = static_cast<size_t>(Kind::Count);

constexpr const wchar_t* kClassNames[kCount] = {
    L"customWall_unfinished_C",
    L"customWall_C",
    L"customWall_wetConcrete_C",
    L"customWall_rad_C",
};

R::FName g_names[kCount] = {};
bool     g_namesResolved = false;

// FTransform's rotation is a quaternion; a spawn takes a rotator. The engine's own FQuat::Rotator,
// including its gimbal-lock branch.
FRotator QuatToRotator(float x, float y, float z, float w) {
    constexpr float kRadToDeg = 57.2957795f;
    const float singularity = z * x - w * y;
    const float yawY = 2.f * (w * z + x * y);
    const float yawX = 1.f - 2.f * (y * y + z * z);
    FRotator r{};
    r.Yaw = std::atan2(yawY, yawX) * kRadToDeg;
    if (singularity < -0.4999995f) {
        r.Pitch = -90.f;
        r.Roll  = -r.Yaw - 2.f * std::atan2(x, w) * kRadToDeg;
    } else if (singularity > 0.4999995f) {
        r.Pitch = 90.f;
        r.Roll  = r.Yaw - 2.f * std::atan2(x, w) * kRadToDeg;
    } else {
        r.Pitch = std::asin(2.f * singularity) * kRadToDeg;
        r.Roll  = std::atan2(-2.f * (w * x + y * z), 1.f - 2.f * (x * x + y * y)) * kRadToDeg;
    }
    return r;
}

}  // namespace

bool ResolveNames() {
    if (g_namesResolved) return true;
    for (size_t i = 0; i < kCount; ++i) {
        g_names[i] = ue_wrap::fname_utils::StringToFName(kClassNames[i]);
        if (g_names[i].ComparisonIndex == 0) return false;  // the string library is not up yet
    }
    g_namesResolved = true;
    return true;
}

Kind KindOf(void* actor) {
    if (!actor || !g_namesResolved) return Kind::Count;
    void* cls = R::ClassOf(actor);
    if (!cls) return Kind::Count;
    const R::FName& n = R::NameOf(cls);
    for (size_t i = 0; i < kCount; ++i)
        if (n.ComparisonIndex == g_names[i].ComparisonIndex && n.Number == g_names[i].Number)
            return static_cast<Kind>(i);
    return Kind::Count;
}

const wchar_t* ClassNameOf(Kind k) {
    const size_t i = static_cast<size_t>(k);
    return i < kCount ? kClassNames[i] : L"";
}

bool RecordLocation(const SR::SaveRecord& rec, float& x, float& y, float& z) {
    x = rec.xform[4];
    y = rec.xform[5];
    z = rec.xform[6];
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

void* SpawnFromRecord(Kind k, const SR::SaveRecord& rec, SR::ClassLoad load) {
    const size_t i = static_cast<size_t>(k);
    if (i >= kCount) return nullptr;
    const CR::Resolved rc = CR::ResolveClass(kClassNames[i], CR::AnyFamily(), load);
    if (rc.state != CR::ClassState::Found || !rc.cls) {
        UE_LOGW("placed_build: class %ls is not available here (state %d) -- nothing spawned",
                kClassNames[i], static_cast<int>(rc.state));
        return nullptr;
    }
    FVector loc{};
    if (!RecordLocation(rec, loc.X, loc.Y, loc.Z)) return nullptr;
    const FRotator rot = QuatToRotator(rec.xform[0], rec.xform[1], rec.xform[2], rec.xform[3]);
    const FVector scale{rec.xform[7], rec.xform[8], rec.xform[9]};
    const bool scaleSane = std::isfinite(scale.X) && std::isfinite(scale.Y) && std::isfinite(scale.Z) &&
                           scale.X > 0.f && scale.Y > 0.f && scale.Z > 0.f;
    void* actor = E::BeginDeferredSpawn(rc.cls, loc, rot, 1, scaleSane ? &scale : nullptr);
    if (!actor) return nullptr;
    if (!E::FinishDeferredSpawn(actor, loc, rot, scaleSane ? &scale : nullptr)) return nullptr;
    SR::SaveRecord applied = rec;
    applied.className = kClassNames[i];
    if (!SR::ApplyRecord(actor, applied, load)) {
        UE_LOGW("placed_build: loadData refused on a fresh %ls -- destroyed", kClassNames[i]);
        E::DestroyActor(actor);
        return nullptr;
    }
    return actor;
}

bool Capture(void* actor, SR::SaveRecord& out) { return actor && SR::CaptureRecord(actor, out); }

bool Apply(void* actor, const SR::SaveRecord& rec) {
    return actor && SR::ApplyRecord(actor, rec, SR::ClassLoad::LookupOnly);
}

}  // namespace ue_wrap::placed_build
