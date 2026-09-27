#pragma once

// Ztoryc: the characters' mouth library, enriched while working (2026-09-27).
//
// A character comes into a shot as a COPY: its scene's structure is copied
// into the shot's .tnz, and a mouth set made there stays there — with Import
// and with Load alike. The mouth maps follow «the map lives where the thing
// lives» (ztorymouthmap.h), so a set made in a shot lands:
//   - mouths on a LEVEL:  in the .zmouth beside the level's PSD — the shot's
//     copy with Import, the character's own file with Load (then shared
//     already: nothing to publish);
//   - mouths in a SUB-SCENE: in the .zmouth beside the SHOT's scene.
// Publishing takes such sets back to the character's library: the .zmouth
// beside its PSD, or beside its scene (Franco, 2026-09-27: «come possiamo
// arricchire la libreria… man mano che lavoriamo nelle scene?»).
//
// Explicit, and offered at save time too with a «don't ask again»: a shot
// also holds variants meant for that scene only, which must not reach
// everybody's library by themselves.

#include "ztorymouthmap.h"

#include <functional>

#include <QHash>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVector>

class QWidget;
class ToonzScene;

struct MouthLibraryItem {
  QString   character;   // the asset name of the library
  QString   subScene;    // empty = the mouths are a level
  QString   label;       // where, as the user reads it
  TFilePath shotOwner;   // ZtoryMouthMap owner in the shot (decoded)
  TFilePath libOwner;    // …and in the library
  QVector<MouthSet> added;    // not in the library
  QVector<MouthSet> changed;  // in the library under the same name, different
  //! Sets that point at frames of the mouth sub-scene the library's copy does
  //! not have: drawn in the shot. A set is a MAP, not drawings — published
  //! alone it would point at nothing there.
  QSet<QString> missingFrames;
};

namespace ZtoryMouthLibrary {

// The shot's sets the library does not have, or has differently. Empty for a
// character's own scene: that IS the library.
QVector<MouthLibraryItem> pending(ToonzScene *scene);

// The dialog: what can be published, each set with its box. `onSave` adds
// «don't ask again when saving».
void showPublishDialog(QWidget *parent, bool onSave = false);

// After a save by the user (not autosave, not the export's sub-scene saves):
// offers the dialog if there is something to publish and it is not turned
// off.
void offerOnSave();

// ── The other way: from the library to the shot ─────────────────────────
// The library's map for the sub-scene `sub` of this scene, found by the
// chain «the nearest sub-scene above it named as a character's scene».
// False if the chain finds no library (or this is a character's own scene).
bool libraryMapForSubScene(ToonzScene *scene, const QString &sub,
                           MouthMap *map, QString *character);

// Mouths on IMPORTED levels: the library has sets the shot's copy of the
// .zmouth does not have, or has differently. (Sub-scenes need no update:
// the shot reads the library's sets live, ZtoryMouthApply::findTargets.)
// In these items `added`/`changed` are the LIBRARY's sets.
QVector<MouthLibraryItem> pendingFromLibrary(ToonzScene *scene);

void showUpdateDialog(QWidget *parent, bool onOpen = false);

// After a scene is opened: offers the update if there is one and it is not
// turned off.
void offerOnOpen();

// The characters' scenes by the lower-case NAME of the file — the name their
// sub-scene takes in a shot — with the asset's name. Shared with the rig
// library (ztoriglibrary.h): one definition of «which character is this».
QHash<QString, QPair<QString, TFilePath>> characterScenesByName();

// The key a sub-scene's name is looked up by in characterScenesByName: lower
// case, WITHOUT the «_1» Tahoma appends to keep level names unique — in a
// shot SOFIA.tnz comes in as the sub-scene «SOFIA_1», and matching the bare
// name found no character at all (2026-09-27).
QString characterKey(const QString &subSceneName);

// Runs `fn` once NO modal window is open (checked every quarter of a second,
// given up after a minute). The questions at open and save used to pop right
// after the load — INSIDE the startup window's modal session when a scene is
// opened from there: a modal within a modal, the outer one closing under the
// inner, and macOS reporting «modalSession has been exited prematurely» just
// before the app froze in window activation (2026-09-27).
void runWhenNoModal(std::function<void()> fn);

// The parent for those questions: the main window. A parentless dialog can
// end up BEHIND it, and the app looks frozen.
QWidget *dialogParent();

}  // namespace ZtoryMouthLibrary
