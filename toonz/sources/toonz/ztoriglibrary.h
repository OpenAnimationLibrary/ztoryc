#pragma once

// ZtoRig: the character's library of poses and correctives, enriched while
// animating (Franco, 2026-08-16 «il ritorno in libreria», 2026-09-27).
//
// A character comes into a shot as a COPY, so a pose recorded there stays
// there. The library is a file beside the character's scene — SOFIA.zrig —
// readable without opening the scene:
//   - a POSE is the same PoseAction on every column of the character (an
//     exploded rig spreads it over its pieces, ZtoRigPanel::onRecord), so it
//     is stored per PART, the part known by its column's name, which the copy
//     keeps. Its deltas are by vertex NAME: portable by construction
//     (plasticskeletondeformation.h, PoseAction);
//   - a CORRECTIVE is sculpted for one mesh, by vertex INDEX, and does not
//     transfer: it goes with a fingerprint of the mesh files, and lands only
//     where the mesh is the same.
// Publish (shot -> library) and update (library -> scene), explicit, and
// offered at save / open with a «don't ask again» — the same model as the
// mouth sets (ztorymouthlibrary.h).
//
// Compatibility (Franco, 2026-08-16): the library's character must have every
// column and vertex the pose names. If not, the publish is REFUSED, and the
// user chooses: create a v2 of the character from this copy, or discard.
//
// Kept apart from the storyboard code on purpose: character animation is the
// part that would go to Otter (ANIMATIC_TASKS, SOSPESI).

#include "tfilepath.h"

#include <QString>

class QWidget;
class TXsheet;

namespace ZtoRigLibrary {

// The character being worked in: its asset name, its scene (the library's
// home) and its xsheet — the character's own scene, or the sub-scene of it
// one is inside in a shot. False when one is inside no character. Shared
// with the animation clips (ztorigclips.h).
bool currentCharacter(QString *name, TFilePath *scene, TXsheet **xsh);

// The two dialogs. `onSave` / `onOpen` add «don't ask again».
void showPublishDialog(QWidget *parent, bool onSave = false);
void showUpdateDialog(QWidget *parent, bool onOpen = false);

// After a save by the user / after a scene is opened.
void offerOnSave();
void offerOnOpen();

}  // namespace ZtoRigLibrary
