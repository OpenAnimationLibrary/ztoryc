#pragma once

// Ztoryc: the one place where task statuses change.
//
// Every automatic rule lives here — a gesture in the app moves a task forward,
// a finished task makes the next one ready — and every change, automatic or by
// hand, is announced on ZtoryModel::taskEvents(): that is what a push to
// Kitsu listens to. Statuses mirrored FROM Kitsu (a pull) do not come through
// setStatus: what the server said is not sent back to it.
//
// Before this module the rules were in five places, and two of them
// disagreed about which task opening a shot puts in WIP (2026-09-26).

#include "ztorymodel.h"

#include <QString>
#include <QVector>

namespace ZtoryTaskFlow {

enum class Entity { Shot = 0, Asset = 1 };

enum class Origin {
  User = 0,  // edited by hand in the tracker: any transition
  App  = 1,  // a gesture in Ztoryc: forward only, never out of WFA or Done
};

// One status change made by setStatus — the one asked for, or one it caused.
struct Change {
  Entity     entity;
  QString    uuid, taskType;
  TaskStatus from, to;
};

// Sets `taskType` of the project shot / asset `uuid` to `to`, if `origin`
// allows it. When the new status is Done, the next task of the chain goes
// Todo → Ready. Returns true if the status changed.
// A task already Done still readies the next one (a Storyboard marked Done by
// hand before the shot export must not leave Layout at Todo).
// Unless `batch`, saves the project DB and notifies the tracker; in a batch
// the caller does both once — saveAndNotifyTasks for shots, saveProjectDb +
// assetsChanged for assets.
// If `changes` is given, every change made is appended to it, the caused
// ones included: what an undo needs to take them all back.
bool setStatus(Entity entity, const QString &uuid, const QString &taskType,
               TaskStatus to, Origin origin, bool batch = false,
               QVector<Change> *changes = nullptr);

// Takes back `changes`, last first — each one only if its task still has the
// status it was given (a task moved on since then is left alone). Does not
// save or notify: the undo that calls it does.
void revert(const QVector<Change> &changes);

// How far along a status is (Todo 0 … Done 5; Retake sits between Ready and
// WIP). Also what decides which of two duplicate tasks wins a merge.
int rank(TaskStatus s);

// Task names are not case-sensitive: Kitsu's «rigging» and our «Rigging» are
// one step. Merges the keys of `tasks` that differ only by case onto the
// spelling used in `chain` (if the chain has neither, onto whichever the
// map holds last): the status furthest along wins, assignees are joined.
void foldTaskNames(QMap<QString, TaskState> &tasks, const QStringList &chain);

// `list` without the entries that repeat an earlier one ignoring case.
QStringList uniqueIgnoringCase(const QStringList &list);

// Todo → Ready for the task after `doneTask` in the entity's chain, without
// saving, notifying or announcing a transition — for statuses that came from
// Kitsu, whose own automations do the same on the server.
bool readyNextAfter(Entity entity, const QString &uuid, const QString &doneTask);

// --- Sync with Kitsu (2026-09-27) --------------------------------------
// Each task remembers its «base»: the status Kitsu had at the last sync.
// Comparing Ztoryc and Kitsu with the base says who changed what:
//
//   Ztoryc vs base   Kitsu vs base   →
//   same             same               nothing
//   same             changed            take Kitsu's
//   changed          same               keep Ztoryc's (a push sends it)
//   changed          changed, differ    CONFLICT: Kitsu wins, reported
//
// With no base yet (a task never synced) a Kitsu «Todo» means nobody worked
// on it there, so Ztoryc's is kept; any other Kitsu status wins.
// Before the base existed, a push wrote every local status over Kitsu's and
// undid the Done a supervisor had just set (Franco, 2026-09-27).

enum class Merge { Same, TookServer, KeptLocal, Conflict };

// Merges one task pulled from Kitsu into Ztoryc, per the table above. Raw
// writes — no transition, nothing pushed back — and the base moves to what
// Kitsu said. The caller saves and notifies.
Merge mergeFromServer(Entity entity, const QString &uuid,
                      const QString &taskType, TaskStatus server);

// True if the task changed in Ztoryc since the last sync — what a push may
// send. Without a base: anything but Todo.
bool changedSinceSync(Entity entity, const QString &uuid,
                      const QString &taskType);

// The status Kitsu is expected to have: the base, or Todo without one. A
// push writes only if Kitsu still has it.
TaskStatus expectedOnServer(Entity entity, const QString &uuid,
                            const QString &taskType);

// Records that Kitsu now has `status` for the task (a change that reached
// it). Raw; the caller saves.
void markSynced(Entity entity, const QString &uuid, const QString &taskType,
                TaskStatus status);

// Kitsu's status wins: written as is — no rules, no transition, so nothing
// is pushed back — then saved and notified. Returns true if it changed.
bool applyFromServer(Entity entity, const QString &uuid,
                     const QString &taskType, TaskStatus status);

// A shot scene was opened: the first task of the technique that is not Done
// (Storyboard excluded) goes to WIP if it is Ready or Retake.
void shotOpened(const QString &shotUuid, const QString &technique);

// The task a character scene is for: the asset type's «Rigging», matched
// without case. Empty if the chain has none.
QString characterSceneTask(const QString &assetUuid);

// The scene of a character was created: its Rigging task goes to WIP.
void characterSceneCreated(const QString &assetUuid);

}  // namespace ZtoryTaskFlow
