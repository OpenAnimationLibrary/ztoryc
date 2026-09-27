#include "ztorykitsusync.h"

#include "ztorymodel.h"
#include "ztorytaskflow.h"

#include "toonzqt/dvdialog.h"

#include <QTimer>

//=============================================================================
// Applying what Kitsu sends back to the model. Moved as they were from the
// tracker's constructor; only the UI refresh became model signals.
//=============================================================================

namespace {

// What a pull did, for the Sync's summary.
struct PullCounts {
  int updated = 0, conflicts = 0, notSent = 0, adopted = 0;
};

// Merges one pulled task and counts what happened. Returns true when
// Ztoryc's own change was kept — something not sent yet.
bool mergeAndCount(ZtoryTaskFlow::Entity entity, const QString &uuid,
                   const QString &taskType, TaskStatus server,
                   PullCounts &c) {
  switch (ZtoryTaskFlow::mergeFromServer(entity, uuid, taskType, server)) {
  case ZtoryTaskFlow::Merge::TookServer: ++c.updated; return false;
  case ZtoryTaskFlow::Merge::Conflict: ++c.updated; ++c.conflicts; return false;
  case ZtoryTaskFlow::Merge::KeptLocal: return true;
  case ZtoryTaskFlow::Merge::Same: return false;
  }
  return false;
}

// The Kitsu ids of the shots a push created or matched.
void applyShotIds(const QHash<QString, QString> &byKey) {
  ZtoryModel *mm = ZtoryModel::instance();
  bool dirty     = false;
  for (ProjectShot &ps : mm->projectShots_rw()) {
    const QString seq = ps.seq.trimmed().isEmpty() ? "SQ01" : ps.seq.trimmed();
    auto it = byKey.find(seq + "\n" + ps.label.trimmed());
    if (it != byKey.end() && ps.kitsuShotId != it.value()) {
      ps.kitsuShotId = it.value();
      dirty          = true;
    }
  }
  if (dirty) mm->saveProjectDb();
}

// The Kitsu ids of the assets a push created or matched.
void applyAssetIds(const QHash<QString, QString> &byKey) {
  ZtoryModel *mm = ZtoryModel::instance();
  bool dirty     = false;
  for (Asset &a : mm->assets()) {
    auto it = byKey.find(a.type + "\n" + a.name.trimmed());
    if (it != byKey.end() && a.kitsuAssetId != it.value()) {
      a.kitsuAssetId = it.value();
      dirty          = true;
    }
  }
  if (dirty) mm->saveProjectDb();
}

// Shot task statuses from Kitsu, merged on the base.
PullCounts applyShotStatuses(const QVector<KitsuPullEntry> &entries) {
  ZtoryModel *mm = ZtoryModel::instance();
  PullCounts c;
  bool dirty = false;
  // Done tasks seen in this pull: their next task is readied AFTER every
  // entry is applied, or a later «next task: Todo» entry would undo it.
  QVector<QPair<QString, QString>> doneTasks;  // shot uuid, task type
  for (const KitsuPullEntry &e : entries) {
    const QString ekey = KitsuClient::normalizeTaskType(e.taskType);
    for (ProjectShot &ps : mm->projectShots_rw()) {
      bool match;
      if (!ps.kitsuShotId.isEmpty() && !e.kitsuShotId.isEmpty())
        match = (ps.kitsuShotId == e.kitsuShotId);
      else {
        const QString psseq =
            ps.seq.trimmed().isEmpty() ? "SQ01" : ps.seq.trimmed();
        match = (ps.label.trimmed() == e.shot.trimmed() &&
                 psseq == e.seq.trimmed());
      }
      if (!match) continue;
      if (ps.kitsuShotId.isEmpty() && !e.kitsuShotId.isEmpty()) {
        ps.kitsuShotId = e.kitsuShotId;
        dirty          = true;
      }
      for (const QString &tt : mm->taskTypesForProjectShot(ps))
        if (KitsuClient::normalizeTaskType(tt) == ekey) {
          if (mergeAndCount(ZtoryTaskFlow::Entity::Shot, ps.uuid, tt,
                            e.status, c))
            ++c.notSent;
          dirty = true;  // the base moved
          // Add-only assignee merge (mirrors the add-only push).
          for (const QString &nm : e.assignees)
            if (!ps.tasks[tt].assignees.contains(nm)) {
              ps.tasks[tt].assignees.push_back(nm);
              dirty = true;
            }
          if (e.status == TaskStatus::Done) doneTasks.push_back({ps.uuid, tt});
          break;
        }
    }
  }
  // Mirrored from Kitsu: the dependency is applied without announcing a
  // transition, so the automatic push does not send it back.
  for (const auto &d : doneTasks)
    if (ZtoryTaskFlow::readyNextAfter(ZtoryTaskFlow::Entity::Shot, d.first,
                                      d.second))
      dirty = true;
  if (dirty) mm->saveAndNotifyTasks();
  return c;
}

// Asset task statuses from Kitsu, merged on the base.
PullCounts applyAssetStatuses(const QVector<KitsuAssetStatusEntry> &entries) {
  ZtoryModel *mm = ZtoryModel::instance();
  PullCounts c;
  bool dirty = false;
  QVector<QPair<QString, QString>> doneTasks;  // asset uuid, task type
  for (const KitsuAssetStatusEntry &e : entries) {
    const QString ekey = KitsuClient::normalizeTaskType(e.taskType);
    for (Asset &a : mm->assets()) {
      bool match;
      if (!a.kitsuAssetId.isEmpty() && !e.kitsuAssetId.isEmpty())
        match = (a.kitsuAssetId == e.kitsuAssetId);
      else
        match = (a.type == e.assetType &&
                 a.name.trimmed().compare(e.assetName.trimmed(),
                                          Qt::CaseInsensitive) == 0);
      if (!match) continue;
      if (a.kitsuAssetId.isEmpty() && !e.kitsuAssetId.isEmpty()) {
        a.kitsuAssetId = e.kitsuAssetId;
        dirty          = true;
      }
      QString target;
      for (const QString &tt : mm->assetTaskTypesForType(a.type))
        if (KitsuClient::normalizeTaskType(tt) == ekey) {
          target = tt;
          break;
        }
      // No counterpart in this asset type's pipeline: ADOPT the Kitsu task
      // type instead of dropping it. Silently discarding it is what made a
      // pull look like it had worked while leaving the tasks empty — on
      // «CARTOON SCHOOL 2026», Modeling and Rigging vanished this way because
      // Ztoryc's canonical pipeline is Concept/Rough/Clean/Color.
      // Kitsu is the source of truth for the pipeline, so it gets appended.
      if (target.isEmpty()) {
        mm->addAssetTaskType(a.type, e.taskType);
        target = e.taskType;
        ++c.adopted;
        dirty = true;
      }
      if (mergeAndCount(ZtoryTaskFlow::Entity::Asset, a.uuid, target,
                        e.status, c))
        ++c.notSent;
      dirty = true;  // the base moved
      for (const QString &nm : e.assignees)
        if (!a.tasks[target].assignees.contains(nm)) {
          a.tasks[target].assignees.push_back(nm);
          dirty = true;
        }
      // As for shots: a Done from Kitsu readies the next task — after the loop.
      if (e.status == TaskStatus::Done) doneTasks.push_back({a.uuid, target});
    }
  }
  for (const auto &d : doneTasks)
    if (ZtoryTaskFlow::readyNextAfter(ZtoryTaskFlow::Entity::Asset, d.first,
                                      d.second))
      dirty = true;
  if (dirty) {
    mm->saveProjectDb();
    emit mm->assetsChanged();
  }
  return c;
}

// Assets authored on Kitsu: matched by id, then by type + name; the new ones
// are added. Returns how many asset TYPES had to be created.
int importAssets(const QVector<KitsuAsset> &assets) {
  ZtoryModel *mm = ZtoryModel::instance();
  int newTypes   = 0;
  bool dirty     = false;
  for (const KitsuAsset &ka : assets) {
    if (ka.name.trimmed().isEmpty()) continue;
    Asset *found = nullptr;
    for (Asset &a : mm->assets()) {
      if (!ka.kitsuAssetId.isEmpty() && a.kitsuAssetId == ka.kitsuAssetId) {
        found = &a;
        break;
      }
      if (a.kitsuAssetId.isEmpty() && a.type == ka.type &&
          a.name.trimmed().compare(ka.name.trimmed(), Qt::CaseInsensitive) == 0)
        found = &a;  // keep looking for a stronger id match
    }
    if (found) {
      if (found->kitsuAssetId != ka.kitsuAssetId) {
        found->kitsuAssetId = ka.kitsuAssetId;
        dirty               = true;
      }
      continue;
    }
    // A Kitsu asset type this project has no pipeline for (the instance also
    // defines Scene and analisi_target beyond our canonical four): create it,
    // or the asset lands with a type that shows nowhere in the Asset Types
    // tab and silently borrows the canonical task order.
    if (!ka.type.trimmed().isEmpty() && !mm->findAssetType(ka.type)) {
      mm->assetTypes().push_back(
          AssetType{ka.type, ZtoryModel::canonicalAssetTaskOrder()});
      ++newTypes;
    }
    mm->addAsset(ka.type, ka.name.trimmed());
    mm->assets().back().kitsuAssetId = ka.kitsuAssetId;
    dirty                            = true;
  }
  if (dirty) {
    mm->saveProjectDb();
    emit mm->assetsChanged();
  }
  return newTypes;
}

// The project's people on Kitsu, added to the team (never removed).
int applyTeam(const QVector<KitsuPerson> &persons) {
  ZtoryModel *mm     = ZtoryModel::instance();
  QStringList roster = mm->team();
  int added          = 0;
  for (const KitsuPerson &p : persons)
    if (!p.name.trimmed().isEmpty() &&
        !roster.contains(p.name, Qt::CaseInsensitive)) {
      roster.push_back(p.name);
      ++added;
    }
  if (added > 0) {
    mm->setTeam(roster);
    mm->saveProjectDb();
  }
  return added;
}

}  // namespace

//=============================================================================
// The Sync
//=============================================================================

ZtoryKitsuSync *ZtoryKitsuSync::instance() {
  static ZtoryKitsuSync *s = new ZtoryKitsuSync();  // app lifetime
  return s;
}

int ZtoryKitsuSync::pendingSends() {
  int toSend = 0, skippedShots = 0;
  QVector<KitsuTaskPush> shotTasks;
  KitsuClient::buildShotPushFromProject(0, shotTasks, skippedShots);
  for (const KitsuTaskPush &t : shotTasks)
    if (!t.createOnly) ++toSend;
  for (const KitsuAssetTaskPush &t : KitsuClient::buildAssetTasksFromModel())
    if (!t.createOnly) ++toSend;
  return toSend;
}

ZtoryKitsuSync::ZtoryKitsuSync(QObject *parent) : QObject(parent) {
  m_watchdog = new QTimer(this);
  m_watchdog->setSingleShot(true);
  connect(m_watchdog, &QTimer::timeout, this, [this]() {
    advance(false, tr("no answer from Kitsu for two minutes"));
  });

  KitsuClient *kc = KitsuClient::instance();
  // Ids: applied whenever they come.
  connect(kc, &KitsuClient::shotIdsResolved, this, &applyShotIds);
  connect(kc, &KitsuClient::assetIdsResolved, this, &applyAssetIds);
  connect(kc, &KitsuClient::taskTypesMissing, this,
          [this](const QStringList &names) {
            warn(tr("These workflow tasks were NOT created in Kitsu: the server "
                    "has no Shot task type with that name, and it refused to "
                    "create one (only a Kitsu admin can):\n\n%1\n\nCreate them "
                    "in Kitsu (Settings > Task Types) and sync again.")
                     .arg(names.join("\n")));
          });
  connect(kc, &KitsuClient::assetsSkipped, this,
          [this](const QStringList &lines) {
            warn(tr("Some assets were NOT sent to Kitsu, because the Kitsu "
                    "server has no asset type with that name:\n\n%1\n\nCreate "
                    "the type in Kitsu (or change the asset's type here) and "
                    "sync again.")
                     .arg(lines.join("\n")));
          });

  connect(kc, &KitsuClient::assetTasksUnlinked, this, [this](int count) {
    warn(tr("%1 asset task status(es) changed in Ztoryc were NOT sent: on Kitsu "
            "their task type is not linked to the asset's type (Kitsu hides "
            "those tasks). Remove the task type from that asset type's pipeline "
            "in Ztoryc, or link it to the asset type on Kitsu.")
             .arg(count));
  });

  // Step 1: shots, then their task statuses.
  connect(kc, &KitsuClient::shotsPushed, this,
          [this](bool ok, int, int, const QString &msg) {
            if (m_step != 1) return;
            if (ok && !m_pendingTasks.isEmpty()) {
              // Rebuilt now: shotIdsResolved (emitted just before) has stored
              // the Kitsu id of the shots this push CREATED, and the task push
              // finds shots by id — by name it can't tell apart the "SQ01" of
              // one episode from another's.
              int unusedSkipped = 0;
              KitsuClient::buildShotPushFromProject(0, m_pendingTasks,
                                                    unusedSkipped);
              m_watchdog->start(120000);  // the second half of the step
              KitsuClient::instance()->pushTasks(
                  ZtoryModel::instance()->kitsuProjectId(), m_pendingTasks);
              m_pendingTasks.clear();
              return;
            }
            advance(ok, msg);
          });
  connect(kc, &KitsuClient::tasksPushed, this,
          [this](bool ok, int, const QString &msg) {
            if (m_step == 1) advance(ok, msg);
          });

  // Step 2: assets, then their task statuses.
  connect(kc, &KitsuClient::assetsPushed, this,
          [this](bool ok, int, int, const QString &msg) {
            if (m_step != 2) return;
            if (ok && !m_pendingAssetTasks.isEmpty()) {
              m_watchdog->start(120000);  // the second half of the step
              KitsuClient::instance()->pushAssetTasks(
                  ZtoryModel::instance()->kitsuProjectId(), m_pendingAssetTasks);
              m_pendingAssetTasks.clear();
              return;
            }
            advance(ok, msg);
          });
  connect(kc, &KitsuClient::assetTasksPushed, this,
          [this](bool ok, int, const QString &msg) {
            if (m_step == 2) advance(ok, msg);
          });

  // Step 3: assets from Kitsu, then their task statuses.
  connect(kc, &KitsuClient::assetsPulled, this,
          [this](bool ok, const QVector<KitsuAsset> &assets, const QString &msg) {
            if (ok && importAssets(assets) > 0) emit assetTypesChanged();
            if (m_step != 3) return;
            if (!ok) {
              advance(false, msg);
              return;
            }
            ZtoryModel *m = ZtoryModel::instance();
            KitsuClient::instance()->pullAssetStatuses(m->kitsuProjectId(),
                                                       m->kitsuEpisodeId());
          });
  connect(kc, &KitsuClient::assetStatusesPulled, this,
          [this](bool ok, const QVector<KitsuAssetStatusEntry> &entries,
                 const QString &msg) {
            if (ok) {
              const PullCounts c = applyAssetStatuses(entries);
              m_updated += c.updated;
              m_conflicts += c.conflicts;
              m_notSent += c.notSent;
              // A new column in the asset table must not look like it came
              // from nowhere: the Asset Types tab shows the pipelines.
              if (c.adopted > 0) emit assetTypesChanged();
            }
            if (m_step == 3) advance(ok, msg);
          });

  // Step 4: team and shot statuses from Kitsu.
  connect(kc, &KitsuClient::teamPulled, this,
          [this](bool ok, const QVector<KitsuPerson> &persons, const QString &) {
            const int added = ok ? applyTeam(persons) : 0;
            if (added > 0) emit teamChanged(added);
          });
  connect(kc, &KitsuClient::statusesPulled, this,
          [this](bool ok, const QVector<KitsuPullEntry> &entries,
                 const QString &msg) {
            if (ok) {
              const PullCounts c = applyShotStatuses(entries);
              m_updated += c.updated;
              m_conflicts += c.conflicts;
              m_notSent += c.notSent;
            }
            if (m_step == 4) advance(ok, msg);
          });
}

bool ZtoryKitsuSync::start(int handles, QString *why) {
  ZtoryModel *m   = ZtoryModel::instance();
  KitsuClient *kc = KitsuClient::instance();
  if (!m->isKitsuLinked() || !kc->isLoggedIn()) {
    if (why) *why = tr("Not connected to a Kitsu production.");
    return false;
  }
  if (isRunning()) {
    if (why) *why = tr("A Sync is already running.");
    return false;
  }
  if (!kc->hasTaskStatuses()) {
    kc->fetchTaskStatuses();
    if (why)
      *why = tr("Kitsu's statuses are still loading — press Sync again in a "
                "moment.");
    return false;
  }
  m_handles = handles;
  m_updated = m_conflicts = m_notSent = 0;
  m_warnings.clear();
  m_step = 0;
  advance(true, QString());
  return true;
}

void ZtoryKitsuSync::warn(const QString &text) {
  if (isRunning())
    m_warnings << text;
  else
    DVGui::MsgBoxInPopup(DVGui::WARNING, text);
}

// Shown once at the end, not as popups that stop the Sync halfway.
void ZtoryKitsuSync::showWarnings() {
  if (m_warnings.isEmpty()) return;
  DVGui::MsgBoxInPopup(DVGui::WARNING, m_warnings.join("\n\n"));
  m_warnings.clear();
}

// One step, called when the previous one has answered.
void ZtoryKitsuSync::advance(bool ok, const QString &msg) {
  if (m_step < 0) return;
  // Re-armed at every step: a reply that never comes must not leave the Sync
  // — and the tracker's disabled button — stuck until the app restarts.
  m_watchdog->start(120000);
  if (!ok) {
    m_step = -1;
    m_watchdog->stop();
    emit finished(false, true, tr("Sync stopped: %1").arg(msg));
    showWarnings();
    return;
  }
  ZtoryModel *m   = ZtoryModel::instance();
  KitsuClient *kc = KitsuClient::instance();
  switch (++m_step) {
  case 1: {
    emit progress(tr("Sync 1/4 — shots to Kitsu…"));
    int skipped = 0;
    const QVector<KitsuShotPush> shots =
        KitsuClient::buildShotPushFromProject(m_handles, m_pendingTasks,
                                              skipped);
    if (shots.isEmpty()) break;
    kc->pushShots(m->kitsuProjectId(), m->episode(),
                  m->productionType() == "tvshow", shots);
    return;
  }
  case 2: {
    emit progress(tr("Sync 2/4 — assets to Kitsu…"));
    const QVector<KitsuAsset> assets = KitsuClient::buildAssetsFromModel();
    if (assets.isEmpty()) break;
    m_pendingAssetTasks = KitsuClient::buildAssetTasksFromModel();
    kc->pushAssets(m->kitsuProjectId(), assets);
    return;
  }
  case 3:
    emit progress(tr("Sync 3/4 — assets from Kitsu…"));
    kc->pullAssets(m->kitsuProjectId(), m->kitsuEpisodeId());
    return;
  case 4:
    emit progress(tr("Sync 4/4 — shot statuses from Kitsu…"));
    kc->pullTeam(m->kitsuProjectId());
    kc->pullStatuses(m->kitsuProjectId(), m->kitsuEpisodeId());
    return;
  default: {
    m_step = -1;
    m_watchdog->stop();
    QString text =
        tr("Synced with Kitsu: %1 status(es) taken from Kitsu").arg(m_updated);
    if (m_conflicts)
      text += tr("; %1 conflict(s) — changed on both sides, Kitsu's kept")
                  .arg(m_conflicts);
    if (m_notSent)
      text += tr("; %1 changed in Ztoryc could not be sent (not on Kitsu yet?)")
                  .arg(m_notSent);
    emit finished(true, m_conflicts || m_notSent, text + ".");
    showWarnings();
    return;
  }
  }
  advance(true, QString());  // a step with nothing to do: the next one
}
