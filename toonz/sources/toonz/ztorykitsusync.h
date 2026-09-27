#pragma once

// Ztoryc: the Sync with Kitsu, out of the tracker (2026-09-27).
//
// One object for the whole app. It runs the four steps of «⇄ Sync with
// Kitsu» and applies to the model what Kitsu sends back — shot and asset ids,
// statuses (three-way merge on the base, ZtoryTaskFlow::mergeFromServer),
// assets created on Kitsu, the team. The tracker is only the button, the
// confirmation and the label.
//
// It used to live in the tracker's constructor: seven handlers testing a step
// number, and ~400 lines of model logic inside UI lambdas — run once PER OPEN
// TRACKER, so two trackers (room + floating) applied every pull twice.
//
// The steps. The pushes come first: each sends only what changed in Ztoryc
// since the last sync, and only over the base Kitsu still has. The pulls
// then merge on the base.
//   1 shots to Kitsu (entities, then task statuses)
//   2 assets to Kitsu (entities, then task statuses)
//   3 assets from Kitsu (new entities, then task statuses)
//   4 team + shot statuses from Kitsu

#include "kitsuclient.h"

#include <QObject>
#include <QStringList>
#include <QVector>

class QTimer;

class ZtoryKitsuSync : public QObject {
  Q_OBJECT
public:
  static ZtoryKitsuSync *instance();

  bool isRunning() const { return m_step >= 0; }
  // How many statuses a Sync would write on Kitsu now: what the tracker asks
  // confirmation for before a large one (typically the first, with no base).
  static int pendingSends();
  // Starts a Sync. Returns false, saying why, when it cannot: not linked,
  // already running, or Kitsu's statuses not loaded yet (without them every
  // Kitsu status would read as Todo, and the merge would throw away changes
  // not sent yet — they are requested, to try again in a moment).
  bool start(int handles, QString *why);

signals:
  void progress(const QString &text);
  // warn: something needs a look (conflicts, changes that could not go).
  void finished(bool ok, bool warn, const QString &summary);
  void assetTypesChanged();  // a pull adopted task types or asset types
  void teamChanged(int added);  // the team pulled from Kitsu added people

private:
  explicit ZtoryKitsuSync(QObject *parent = nullptr);
  void advance(bool ok, const QString &msg);
  void showWarnings();
  void warn(const QString &text);

  int         m_step    = -1;  // -1 idle; 0..4 while syncing
  int         m_handles = 0;
  int         m_updated = 0, m_conflicts = 0, m_notSent = 0;
  QStringList m_warnings;
  QTimer     *m_watchdog = nullptr;
  QVector<KitsuTaskPush>      m_pendingTasks;       // step 1, after the shots
  QVector<KitsuAssetTaskPush> m_pendingAssetTasks;  // step 2, after the assets
};
