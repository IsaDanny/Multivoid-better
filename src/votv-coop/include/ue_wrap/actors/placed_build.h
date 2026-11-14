// ue_wrap/actors/placed_build.h -- the walls a player builds: the wall builder's unfinished frame,
// the finished wall it turns into once its bricks are in, and the two finished variants (wet
// concrete, which dries into a wall, and the radioactive wall). Engine-wrapper layer: class
// identity, the actor's own save record, a spawn from a record, and a key write. No network.
//
// Every class here descends from Aactor_save_C and carries its whole state in getData (shape,
// material, health, the unfinished frame's brick count), so the record is the only payload a lane
// needs, the same handover the anchored hook uses (ue_wrap/actors/hook.h).
//
// Game thread only.

#pragma once

#include <cstdint>
#include <string>

#include "ue_wrap/actors/save_record.h"

namespace ue_wrap::placed_build {

// Wire order: the index IS the wire's classId, so a row may be appended but never reordered.
enum class Kind : uint8_t {
    WallUnfinished  = 0,  // customWall_unfinished_C, what prop_wallbuilder_C::addWall places
    Wall            = 1,  // customWall_C, what buildWall turns a full frame into
    WallWetConcrete = 2,  // customWall_wetConcrete_C, dries into a wall on its own tick
    WallRad         = 3,  // customWall_rad_C
    Count           = 4,
};

// Mint the class names once per process. KindOf answers Count until this has succeeded.
bool ResolveNames();

// `actor`'s Kind by an exact compare of its class name, or Count. No allocation and no engine call,
// so an object-index listener may ask it.
Kind KindOf(void* actor);

// The Blueprint class name for a Kind, empty past the table.
const wchar_t* ClassNameOf(Kind k);

// Spawn `k` at the record's transform and hand the record to the actor's own loadData. The record's
// class name is ignored: the Kind names the class. Null on any failure, with nothing left standing.
void* SpawnFromRecord(Kind k, const save_record::SaveRecord& rec, save_record::ClassLoad load);

// The actor's own getData, and its loadData. Thin names over save_record so a lane reads as what
// it does.
bool Capture(void* actor, save_record::SaveRecord& out);
bool Apply(void* actor, const save_record::SaveRecord& rec);

// The world location the record's transform holds.
bool RecordLocation(const save_record::SaveRecord& rec, float& x, float& y, float& z);

}  // namespace ue_wrap::placed_build
