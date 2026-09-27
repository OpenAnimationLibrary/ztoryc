#include "ztorytaskflow.h"

namespace ZtoryTaskFlow {

// Retake sits between Ready and WIP: work was sent back, and picking it up
// again (→ WIP) is a step forward.
int rank(TaskStatus s) {
  switch (s) {
  case TaskStatus::Todo:
    return 0;
  case TaskStatus::Ready:
    return 1;
  case TaskStatus::Retake:
    return 2;
  case TaskStatus::Wip:
    return 3;
  case TaskStatus::Wfa:
    return 4;
  case TaskStatus::Done:
    return 5;
  }
  return 0;
}

Merge mergeFromServer(Entity entity, const QString &uuid,
                      const QString &taskType, TaskStatus server) {
  ZtoryModel *m   = ZtoryModel::instance();
  const int   ent = static_cast<int>(entity);
  const TaskStatus local = m->taskStatusOf(ent, uuid, taskType);
  TaskStatus base        = TaskStatus::Todo;
  const bool hasBase     = m->taskSyncedOf(ent, uuid, taskType, &base);

  Merge result;
  if (local == server)
    result = Merge::Same;
  else if (!hasBase)
    result = server == TaskStatus::Todo ? Merge::KeptLocal : Merge::TookServer;
  else if (server == base)
    result = Merge::KeptLocal;  // only Ztoryc changed
  else if (local == base)
    result = Merge::TookServer;  // only Kitsu changed
  else
    result = Merge::Conflict;  // both changed: Kitsu wins

  if (result == Merge::TookServer || result == Merge::Conflict)
    m->writeTaskStatus(ent, uuid, taskType, server);
  m->setTaskSynced(ent, uuid, taskType, server);
  return result;
}

bool changedSinceSync(Entity entity, const QString &uuid,
                      const QString &taskType) {
  const ZtoryModel *m = ZtoryModel::instance();
  const int ent       = static_cast<int>(entity);
  return m->taskStatusOf(ent, uuid, taskType) !=
         expectedOnServer(entity, uuid, taskType);
}

TaskStatus expectedOnServer(Entity entity, const QString &uuid,
                            const QString &taskType) {
  TaskStatus base = TaskStatus::Todo;
  ZtoryModel::instance()->taskSyncedOf(static_cast<int>(entity), uuid,
                                       taskType, &base);
  return base;
}

void markSynced(Entity entity, const QString &uuid, const QString &taskType,
                TaskStatus status) {
  ZtoryModel::instance()->setTaskSynced(static_cast<int>(entity), uuid,
                                        taskType, status);
}

void foldTaskNames(QMap<QString, TaskState> &tasks, const QStringList &chain) {
  const QStringList keys = tasks.keys();
  for (const QString &k : keys) {
    if (!tasks.contains(k)) continue;  // already merged into another key
    QString canon;
    for (const QString &c : chain)
      if (c.compare(k, Qt::CaseInsensitive) == 0) { canon = c; break; }
    if (canon.isEmpty())
      for (const QString &o : tasks.keys())
        if (o != k && o.compare(k, Qt::CaseInsensitive) == 0) { canon = o; break; }
    if (canon.isEmpty() || canon == k) continue;
    const TaskState src = tasks.take(k);
    if (!tasks.contains(canon)) {
      tasks.insert(canon, src);
      continue;
    }
    TaskState &dst = tasks[canon];
    if (rank(src.status) > rank(dst.status)) dst.status = src.status;
    if (!dst.hasSynced && src.hasSynced) {
      dst.hasSynced = true;
      dst.synced    = src.synced;
    }
    for (const QString &name : src.assignees)
      if (!dst.assignees.contains(name)) dst.assignees.push_back(name);
  }
}

QStringList uniqueIgnoringCase(const QStringList &list) {
  QStringList out;
  for (const QString &s : list) {
    bool seen = false;
    for (const QString &o : out)
      if (o.compare(s, Qt::CaseInsensitive) == 0) { seen = true; break; }
    if (!seen) out.push_back(s);
  }
  return out;
}

}  // namespace ZtoryTaskFlow

namespace {

using ZtoryTaskFlow::rank;

// What the app may do on its own. WFA and Done are judgements made in review
// (on Kitsu, by a person): nothing automatic takes a task out of them.
bool appMayMove(TaskStatus from, TaskStatus to) {
  if (from == TaskStatus::Wfa || from == TaskStatus::Done) return false;
  return rank(to) > rank(from);
}

// The task types of the entity, in pipeline order: the technique's for a shot,
// the asset type's for an asset.
QStringList chainOf(ZtoryTaskFlow::Entity entity, const QString &uuid) {
  const ZtoryModel *m = ZtoryModel::instance();
  if (entity == ZtoryTaskFlow::Entity::Shot) {
    for (const ProjectShot &ps : m->projectShots())
      if (ps.uuid == uuid) return m->taskTypesForProjectShot(ps);
  } else {
    for (const Asset &a : m->assets())
      if (a.uuid == uuid) return m->assetTaskTypesForType(a.type);
  }
  return QStringList();
}

// The task after `task` in `chain`, or empty at the end / not found.
QString nextInChain(const QStringList &chain, const QString &task) {
  const int i = chain.indexOf(task);
  return (i >= 0 && i + 1 < chain.size()) ? chain[i + 1] : QString();
}

void persistAndNotify(ZtoryTaskFlow::Entity entity) {
  ZtoryModel *m = ZtoryModel::instance();
  if (entity == ZtoryTaskFlow::Entity::Shot) {
    m->saveAndNotifyTasks();
  } else {
    m->saveProjectDb();
    emit m->assetsChanged();
  }
}

}  // namespace

namespace ZtoryTaskFlow {

bool setStatus(Entity entity, const QString &uuid, const QString &taskType,
               TaskStatus to, Origin origin, bool batch,
               QVector<Change> *changes) {
  ZtoryModel *m   = ZtoryModel::instance();
  const int   ent = static_cast<int>(entity);
  bool found      = false;
  const TaskStatus from = m->taskStatusOf(ent, uuid, taskType, &found);
  if (!found) return false;
  if (from == to) {
    if (to == TaskStatus::Done) {
      const QString next = nextInChain(chainOf(entity, uuid), taskType);
      if (!next.isEmpty())
        setStatus(entity, uuid, next, TaskStatus::Ready, Origin::App, batch,
                  changes);
    }
    return false;
  }
  if (origin == Origin::App && !appMayMove(from, to)) return false;

  m->writeTaskStatus(ent, uuid, taskType, to);
  if (changes) changes->push_back({entity, uuid, taskType, from, to});
  emit m->taskEvents()->transitioned(ent, uuid, taskType,
                                     static_cast<int>(from),
                                     static_cast<int>(to),
                                     static_cast<int>(origin));

  // A finished task makes the next one ready. It is the app that does it, so
  // it is announced (and pushed) as an App transition of its own.
  if (to == TaskStatus::Done) {
    const QString next = nextInChain(chainOf(entity, uuid), taskType);
    if (!next.isEmpty())
      setStatus(entity, uuid, next, TaskStatus::Ready, Origin::App,
                /*batch=*/true, changes);
  }

  if (!batch) persistAndNotify(entity);
  return true;
}

void revert(const QVector<Change> &changes) {
  ZtoryModel *m = ZtoryModel::instance();
  for (int i = changes.size() - 1; i >= 0; --i) {
    const Change &c = changes[i];
    if (m->taskStatusOf(static_cast<int>(c.entity), c.uuid, c.taskType) != c.to)
      continue;
    setStatus(c.entity, c.uuid, c.taskType, c.from, Origin::User,
              /*batch=*/true);
  }
}

bool readyNextAfter(Entity entity, const QString &uuid,
                    const QString &doneTask) {
  const QString next = nextInChain(chainOf(entity, uuid), doneTask);
  if (next.isEmpty()) return false;
  ZtoryModel *m   = ZtoryModel::instance();
  const int   ent = static_cast<int>(entity);
  if (m->taskStatusOf(ent, uuid, next) != TaskStatus::Todo) return false;
  return m->writeTaskStatus(ent, uuid, next, TaskStatus::Ready);
}

bool applyFromServer(Entity entity, const QString &uuid,
                     const QString &taskType, TaskStatus status) {
  ZtoryModel *m   = ZtoryModel::instance();
  const int   ent = static_cast<int>(entity);
  bool found      = false;
  if (m->taskStatusOf(ent, uuid, taskType, &found) == status || !found)
    return false;
  m->writeTaskStatus(ent, uuid, taskType, status);
  persistAndNotify(entity);
  return true;
}

void shotOpened(const QString &shotUuid, const QString &technique) {
  ZtoryModel *m = ZtoryModel::instance();
  // The technique written in the shot's .ztoryc wins over the project DB's.
  QStringList chain;
  if (!technique.isEmpty())
    if (const Technique *t = m->findTechnique(technique)) chain = t->taskTypes;
  if (chain.isEmpty()) chain = chainOf(Entity::Shot, shotUuid);

  // Storyboard is the board pass, done before the shot scene exists; the
  // first task after it that is not Done is the one being worked on.
  for (const QString &tt : chain) {
    if (ZtoryModel::isStoryboardTask(tt)) continue;
    const TaskStatus s =
        m->taskStatusOf(static_cast<int>(Entity::Shot), shotUuid, tt);
    if (s == TaskStatus::Done) continue;
    if (s == TaskStatus::Ready || s == TaskStatus::Retake)
      setStatus(Entity::Shot, shotUuid, tt, TaskStatus::Wip, Origin::App);
    return;
  }
}

QString characterSceneTask(const QString &assetUuid) {
  // The character scene is where the rig is built. The name is matched
  // without case: projects pulled from Kitsu spell it «rigging».
  for (const QString &tt : chainOf(Entity::Asset, assetUuid))
    if (tt.compare("Rigging", Qt::CaseInsensitive) == 0) return tt;
  return QString();
}

void characterSceneCreated(const QString &assetUuid) {
  const QString task = characterSceneTask(assetUuid);
  if (!task.isEmpty())
    setStatus(Entity::Asset, assetUuid, task, TaskStatus::Wip, Origin::App);
}

}  // namespace ZtoryTaskFlow
