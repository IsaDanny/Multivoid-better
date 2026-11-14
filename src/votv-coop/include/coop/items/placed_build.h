// coop/items/placed_build.h -- the walls a player builds, shared: the wall builder's frame, the wall
// it becomes once its bricks are in, and the wet-concrete and radioactive walls. docs/deployables.md.
//
// The host owns every one of them. Its own births (its player's build, a frame filling up, concrete
// drying) and deaths go to every client as the actor's getData record and its save key. A client's
// copies run none of the verbs that change a wall -- the frame's brick overlap, buildWall, the
// concrete's drying tick, Damage -- so a copy only changes when the host says so; a brick a client
// throws into a frame is a synced prop, and the host's own frame eats it. A client's build is the
// one thing a client authors: the frame its addWall placed is sent to the host as a commit, and the
// host's PlacedBuildSpawn naming that commit retires the client's own frame.
//
// Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct BlobChunkPayload;
struct PlacedBuildDestroyPayload;
}  // namespace coop::net

namespace coop::placed_build {

void Install(coop::net::Session* session);
void Tick();

// HOST: a client's commit chunk. EVERYONE: the host's spawn chunk. EVERYONE: the host's destroy.
void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void OnSpawnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void OnDestroy(const coop::net::PlacedBuildDestroyPayload& p);

// HOST: every standing wall to a joiner at its world-ready edge; the joiner skips the keys its save
// already loaded.
void QueueConnectBroadcastForSlot(int peerSlot);

void OnPeerLeftSlot(int slot);
void OnDisconnect();

}  // namespace coop::placed_build
