#include "ztorymouthlibrary.h"

#include "tapp.h"
#include "ztoryassetpreview.h"  // ztoryPsdsUsedByScene
#include "ztorycharacter.h"
#include "ztorymodel.h"

#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshsimplelevel.h"

#include <QCheckBox>
#include <memory>
#include <QMainWindow>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

namespace {

const char *kAskOnSaveKey = "Ztoryc/MouthLibrary/askOnSave";
const char *kAskOnOpenKey = "Ztoryc/MouthLibrary/askOnOpen";

// Everything that makes two sets the same set.
QString signatureOf(const MouthSet &s) {
  QStringList parts{s.name, s.view, s.expression, s.variant};
  for (int i = 0; i < 10; i++) {
    QStringList slot;
    for (const MouthTarget &t : s.mouths[i])
      slot << t.levelName + ":" +
                  QString::fromStdString(t.frameId.expand()) + ":" +
                  t.poseName;
    parts << slot.join(",");
  }
  return parts.join("|");
}

// The highest frame of the anchor the set points at (0 = none).
int maxAnchorFrame(const MouthSet &s) {
  int top = 0;
  for (int i = 0; i < 10; i++)
    for (const MouthTarget &t : s.mouths[i])
      if (t.isAnchorLevel() && !t.isPose())
        top = std::max(top, t.frameId.getNumber());
  return top;
}

struct Library {
  QString   character;
  TFilePath scene;  // the character's scene (decoded)
};

// The characters' libraries, by the NAME of their scene file: it is the name
// the sub-scene takes when the scene is brought into a shot (loadChildLevel
// names it after the file), so it is what ties a shot back to its library.
QHash<QString, Library> librariesByName() {
  QHash<QString, Library> out;
  ZtoryModel *m = ZtoryModel::instance();
  for (const Asset &a : m->assets()) {
    if (!ZtoryModel::isCharacterType(a.type)) continue;
    const QString file = m->resolveAssetFile(a);
    if (!file.endsWith(".tnz", Qt::CaseInsensitive)) continue;
    Library l;
    l.character = a.name;
    l.scene     = TFilePath(file.toStdWString());
    out.insert(QFileInfo(file).completeBaseName().toLower(), l);
  }
  return out;
}

// Down the tree of sub-scenes from `top`, the first one called `name`: its
// xsheet, and the names of the sub-scenes above it (outermost first).
TXsheet *findSubScene(TXsheet *top, const QString &name, QStringList *above) {
  std::function<TXsheet *(TXsheet *, QStringList, QSet<TXsheet *> &, int)>
      walk = [&](TXsheet *xsh, QStringList path, QSet<TXsheet *> &seen,
                 int depth) -> TXsheet * {
    if (!xsh || depth > 8 || seen.contains(xsh)) return nullptr;
    seen.insert(xsh);
    for (int c = 0; c < xsh->getColumnCount(); c++) {
      TXshColumn *col = xsh->getColumn(c);
      if (!col || col->isEmpty()) continue;
      int r0 = 0, r1 = -1;
      xsh->getCellRange(c, r0, r1);
      QSet<TXshLevel *> done;
      for (int r = r0; r <= r1; r++) {
        const TXshCell cell = xsh->getCell(r, c);
        if (cell.isEmpty() || !cell.m_level) continue;
        TXshChildLevel *cl = cell.m_level->getChildLevel();
        if (!cl || done.contains(cell.m_level.getPointer())) continue;
        done.insert(cell.m_level.getPointer());
        const QString n = QString::fromStdWString(cell.m_level->getName());
        if (n == name) {
          if (above) *above = path;
          return cl->getXsheet();
        }
        QStringList deeper = path;
        deeper << n;
        if (TXsheet *found = walk(cl->getXsheet(), deeper, seen, depth + 1))
          return found;
      }
    }
    return nullptr;
  };
  QSet<TXsheet *> seen;
  return walk(top, QStringList(), seen, 0);
}

// The library of the sub-scene `sub`: the nearest sub-scene ABOVE it named as
// a character's scene. The sub-scene's own name is no help — «sub» is
// Tahoma's default, SOFIA and BRONTOLO both have one.
bool libraryOfSubScene(TXsheet *top, const QString &sub,
                       const QHash<QString, Library> &libs, Library *out) {
  QStringList above;
  if (!findSubScene(top, sub, &above)) return false;
  for (int i = above.size() - 1; i >= 0; --i) {
    // Not the bare name: a scene brought into a shot takes a «_1».
    auto it = libs.constFind(ZtoryMouthLibrary::characterKey(above[i]));
    if (it == libs.constEnd()) it = libs.constFind(above[i].toLower());
    if (it != libs.constEnd()) {
      *out = it.value();
      return true;
    }
  }
  return false;
}

// How many frames the library's own copy of the sub-scene has (-1 unknown).
int librarySubSceneFrames(const TFilePath &libScene, const QString &sub) {
  try {
    ToonzScene lib;
    lib.loadTnzFile(libScene);
    TXsheet *xsh = findSubScene(lib.getXsheet(), sub, nullptr);
    return xsh ? xsh->getFrameCount() : -1;
  } catch (...) {
    return -1;
  }
}

// The sets of `from` that `to` does not have, or has differently.
void compareSets(const MouthMap &from, const MouthMap &to,
                 MouthLibraryItem *item) {
  for (const MouthSet &s : from.sets) {
    if (!s.isUsable()) continue;
    const int i = to.indexOfSet(s.name);
    if (i < 0)
      item->added << s;
    else if (signatureOf(to.sets[i]) != signatureOf(s))
      item->changed << s;
  }
}

// The physical PSD behind a layer level «name#7#group.psd».
QString physicalPsdName(const TFilePath &level) {
  const QString name = QString::fromStdWString(level.getWideName());
  const int hash     = name.indexOf('#');
  return (hash < 0 ? name : name.left(hash)) + ".psd";
}

// What a scene has mapped. False for an untitled scene or a character's own
// scene, which IS the library.
bool scanScene(ToonzScene *scene, TFilePath *scenePath, QSet<QString> *subs,
               QVector<TFilePath> *levels) {
  if (!scene || scene->isUntitled()) return false;
  *scenePath = scene->decodeFilePath(scene->getScenePath());
  if (ZtoryCharacter::roleOf(scenePath->getQString()) ==
      QLatin1String("character"))
    return false;
  ZtoryMouthMap::mappedSubScenes(*scenePath, subs);
  TLevelSet *ls = scene->getLevelSet();
  for (int i = 0; i < ls->getLevelCount(); i++) {
    TXshSimpleLevel *sl = ls->getLevel(i)->getSimpleLevel();
    if (!sl || sl->getPath().getType() != "psd") continue;
    const TFilePath level = scene->decodeFilePath(sl->getPath());
    if (ZtoryMouthMap::exists(level)) levels->push_back(level);
  }
  return true;
}

struct LevelPair {
  QString   character;
  TFilePath shotLevel, libLevel;
};

// For each mapped level of the shot, the library's copy of the same layer:
// beside the PSD the character's scene is built from, same file name. Not
// when the shot uses the library's own file (Load): already shared.
QVector<LevelPair> levelPairs(const QVector<TFilePath> &levels,
                              const QHash<QString, Library> &libs) {
  QVector<LevelPair> out;
  QHash<QString, QStringList> libPsds;  // character -> its PSDs, read once
  for (const TFilePath &shotLevel : levels) {
    const QString psdName = physicalPsdName(shotLevel);
    for (auto it = libs.constBegin(); it != libs.constEnd(); ++it) {
      QStringList &psds = libPsds[it.value().character];
      if (psds.isEmpty())
        psds = ztoryPsdsUsedByScene(it.value().scene.getQString());
      for (const QString &p : psds) {
        if (QFileInfo(p).fileName().compare(psdName, Qt::CaseInsensitive))
          continue;
        const TFilePath libLevel =
            TFilePath(QFileInfo(p).absolutePath().toStdWString()) +
            shotLevel.withoutParentDir();
        if (libLevel.getQString() == shotLevel.getQString()) continue;
        out.push_back({it.value().character, shotLevel, libLevel});
      }
    }
  }
  return out;
}

// The one dialog, both ways: a box per set, new ones ticked, different ones
// not (replacing is the choice to make on purpose), and those that point at
// missing drawings not ticked either. Returns the chosen sets per item.
bool chooseSets(QWidget *parent, const QString &title, const QString &text,
                const QVector<MouthLibraryItem> &items, const char *askKey,
                const QString &dontAskText,
                QVector<QVector<MouthSet>> *chosen) {
  QDialog dlg(parent);
  dlg.setWindowTitle(title);
  dlg.setMinimumWidth(480);
  auto *lay  = new QVBoxLayout(&dlg);
  auto *head = new QLabel(text, &dlg);
  head->setWordWrap(true);
  lay->addWidget(head);

  auto *list = new QListWidget(&dlg);
  struct Pick { int item; const MouthSet *set; };
  QVector<Pick> picks;
  auto add = [&](int i, const MouthSet &s, bool changed) {
    const bool missing = items[i].missingFrames.contains(s.name);
    QString what = changed ? QObject::tr("differs — replaces it")
                           : QObject::tr("new");
    if (missing)
      what += QObject::tr("; ⚠️ uses drawings the library does not have");
    auto *w = new QListWidgetItem(
        QObject::tr("%1: «%2» — %3").arg(items[i].label, s.name, what), list);
    w->setFlags(w->flags() | Qt::ItemIsUserCheckable);
    w->setCheckState(!changed && !missing ? Qt::Checked : Qt::Unchecked);
    picks << Pick{i, &s};
  };
  for (int i = 0; i < items.size(); i++) {
    for (const MouthSet &s : items[i].added) add(i, s, false);
    for (const MouthSet &s : items[i].changed) add(i, s, true);
  }
  lay->addWidget(list);

  QCheckBox *dontAsk = nullptr;
  if (askKey) {
    dontAsk = new QCheckBox(dontAskText, &dlg);
    lay->addWidget(dontAsk);
  }
  auto *bb = new QDialogButtonBox(&dlg);
  bb->addButton(QObject::tr("OK"), QDialogButtonBox::AcceptRole);
  bb->addButton(askKey ? QObject::tr("Not now") : QObject::tr("Cancel"),
                QDialogButtonBox::RejectRole);
  QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  lay->addWidget(bb);

  const bool ok = dlg.exec() == QDialog::Accepted;
  if (dontAsk && dontAsk->isChecked()) QSettings().setValue(askKey, false);
  if (!ok) return false;
  chosen->resize(items.size());
  for (int p = 0; p < picks.size(); p++)
    if (list->item(p)->checkState() == Qt::Checked)
      (*chosen)[picks[p].item] << *picks[p].set;
  return true;
}

// Adds / replaces `sets` in the map at `owner`, keeping the rest. The
// character is taken from `fallbackOwner` when the map had none yet.
bool mergeInto(const TFilePath &owner, const QString &sub,
               const TFilePath &fallbackOwner, const QVector<MouthSet> &sets,
               QString *why) {
  MouthMap map;
  QString readError;
  if (!ZtoryMouthMap::load(owner, sub, map, &readError) &&
      QFile::exists(ZtoryMouthMap::pathFor(owner).getQString())) {
    // The file is there and cannot be read: saving would rewrite it with
    // only these sets, and the rest would be lost.
    if (why) *why = readError;
    return false;
  }
  if (map.characterName.isEmpty()) {
    MouthMap other;
    ZtoryMouthMap::load(fallbackOwner, sub, other);
    map.characterUuid = other.characterUuid;
    map.characterName = other.characterName;
  }
  for (const MouthSet &s : sets) {
    const int i = map.indexOfSet(s.name);
    if (i >= 0)
      map.sets[i] = s;
    else
      map.sets << s;
  }
  return ZtoryMouthMap::save(owner, sub, map, why);
}

void report(QWidget *parent, const QString &title, const QStringList &done,
            const QStringList &failed) {
  QString msg;
  if (!done.isEmpty()) msg += QObject::tr("Done:\n%1").arg(done.join("\n"));
  if (!failed.isEmpty())
    msg += (msg.isEmpty() ? QString() : QString("\n\n")) +
           QObject::tr("Not done:\n%1").arg(failed.join("\n"));
  if (!msg.isEmpty()) QMessageBox::information(parent, title, msg);
}

}  // namespace

//----------------------------------------------------------------------------

QVector<MouthLibraryItem> ZtoryMouthLibrary::pending(ToonzScene *scene) {
  QVector<MouthLibraryItem> out;
  TFilePath scenePath;
  QSet<QString> subs;
  QVector<TFilePath> levels;
  // Called at every save by the user: the libraries (a folder listing per
  // character) are looked for only when the scene HAS mouth maps.
  if (!scanScene(scene, &scenePath, &subs, &levels)) return out;
  if (subs.isEmpty() && levels.isEmpty()) return out;
  const QHash<QString, Library> libs = librariesByName();
  if (libs.isEmpty()) return out;

  for (const QString &sub : subs) {
    Library lib;
    if (!libraryOfSubScene(scene->getXsheet(), sub, libs, &lib)) continue;
    MouthMap shotMap, libMap;
    if (!ZtoryMouthMap::load(scenePath, sub, shotMap)) continue;
    ZtoryMouthMap::load(lib.scene, sub, libMap);  // none yet is fine
    MouthLibraryItem item;
    item.character = lib.character;
    item.subScene  = sub;
    item.label     = QObject::tr("%1 ▸ %2 (sub-scene)").arg(lib.character, sub);
    item.shotOwner = scenePath;
    item.libOwner  = lib.scene;
    compareSets(shotMap, libMap, &item);
    if (item.added.isEmpty() && item.changed.isEmpty()) continue;
    // Drawings made in the shot: the set would point at frames the
    // library's sub-scene does not have.
    const int frames = librarySubSceneFrames(lib.scene, sub);
    if (frames >= 0) {
      for (const QVector<MouthSet> *v : {&item.added, &item.changed})
        for (const MouthSet &s : *v)
          if (maxAnchorFrame(s) > frames) item.missingFrames.insert(s.name);
    }
    out << item;
  }

  for (const LevelPair &lp : levelPairs(levels, libs)) {
    MouthMap shotMap, libMap;
    if (!ZtoryMouthMap::load(lp.shotLevel, QString(), shotMap)) continue;
    ZtoryMouthMap::load(lp.libLevel, QString(), libMap);
    MouthLibraryItem item;
    item.character = lp.character;
    item.label     = QObject::tr("%1 ▸ %2").arg(
        lp.character, QString::fromStdWString(lp.shotLevel.getWideName()));
    item.shotOwner = lp.shotLevel;
    item.libOwner  = lp.libLevel;
    compareSets(shotMap, libMap, &item);
    if (!item.added.isEmpty() || !item.changed.isEmpty()) out << item;
  }
  return out;
}

void ZtoryMouthLibrary::showPublishDialog(QWidget *parent, bool onSave) {
  const QString title = QObject::tr("Publish mouths to the library");
  ToonzScene *scene   = TApp::instance()->getCurrentScene()->getScene();
  const QVector<MouthLibraryItem> items = pending(scene);
  if (items.isEmpty()) {
    if (!onSave)
      QMessageBox::information(
          parent, title,
          QObject::tr("Nothing to publish: every mouth set of this scene is "
                      "already in its character's library."));
    return;
  }
  QVector<QVector<MouthSet>> chosen;
  if (!chooseSets(parent, title,
                  QObject::tr("These mouth sets are in this scene and not in "
                              "the character's library. Published, every "
                              "scene with the character sees them."),
                  items, onSave ? kAskOnSaveKey : nullptr,
                  QObject::tr("Don't ask again when saving (the Publish button "
                              "in ZtoRig ▸ Mouths stays)"),
                  &chosen))
    return;
  QStringList done, failed;
  for (int i = 0; i < items.size(); i++) {
    if (chosen[i].isEmpty()) continue;
    QString why;
    if (mergeInto(items[i].libOwner, items[i].subScene, items[i].shotOwner,
                  chosen[i], &why))
      done << QObject::tr("%1: %2 set(s)").arg(items[i].label).arg(chosen[i].size());
    else
      failed << QObject::tr("%1: %2").arg(items[i].label, why);
  }
  report(parent, title, done, failed);
}

void ZtoryMouthLibrary::runWhenNoModal(std::function<void()> fn) {
  auto *timer = new QTimer(qApp);
  auto tries  = std::make_shared<int>(0);
  QObject::connect(timer, &QTimer::timeout, timer, [timer, fn, tries]() {
    if (QApplication::activeModalWidget() && ++*tries < 240) return;
    timer->stop();
    timer->deleteLater();
    if (!QApplication::activeModalWidget()) fn();
  });
  timer->start(250);
}

QWidget *ZtoryMouthLibrary::dialogParent() {
  return TApp::instance()->getMainWindow();
}

void ZtoryMouthLibrary::offerOnSave() {
  if (!QSettings().value(kAskOnSaveKey, true).toBool()) return;
  // After the save has finished, and with no modal window open.
  runWhenNoModal([]() {
    ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
    if (!pending(scene).isEmpty()) showPublishDialog(dialogParent(), true);
  });
}

//----------------------------------------------------------------------------

bool ZtoryMouthLibrary::libraryMapForSubScene(ToonzScene *scene,
                                              const QString &sub,
                                              MouthMap *map,
                                              QString *character) {
  if (!scene || scene->isUntitled()) return false;
  const TFilePath scenePath = scene->decodeFilePath(scene->getScenePath());
  if (ZtoryCharacter::roleOf(scenePath.getQString()) ==
      QLatin1String("character"))
    return false;
  Library lib;
  if (!libraryOfSubScene(scene->getXsheet(), sub, librariesByName(), &lib))
    return false;
  if (!ZtoryMouthMap::load(lib.scene, sub, *map) || map->sets.isEmpty())
    return false;
  if (character) *character = lib.character;
  return true;
}

QVector<MouthLibraryItem> ZtoryMouthLibrary::pendingFromLibrary(
    ToonzScene *scene) {
  QVector<MouthLibraryItem> out;
  TFilePath scenePath;
  QSet<QString> subs;
  QVector<TFilePath> levels;
  if (!scanScene(scene, &scenePath, &subs, &levels) || levels.isEmpty())
    return out;
  const QHash<QString, Library> libs = librariesByName();
  for (const LevelPair &lp : levelPairs(levels, libs)) {
    MouthMap shotMap, libMap;
    if (!ZtoryMouthMap::load(lp.libLevel, QString(), libMap)) continue;
    ZtoryMouthMap::load(lp.shotLevel, QString(), shotMap);
    MouthLibraryItem item;
    item.character = lp.character;
    item.label     = QObject::tr("%1 ▸ %2").arg(
        lp.character, QString::fromStdWString(lp.shotLevel.getWideName()));
    item.shotOwner = lp.shotLevel;
    item.libOwner  = lp.libLevel;
    compareSets(libMap, shotMap, &item);  // the LIBRARY's sets
    if (!item.added.isEmpty() || !item.changed.isEmpty()) out << item;
  }
  return out;
}

void ZtoryMouthLibrary::showUpdateDialog(QWidget *parent, bool onOpen) {
  const QString title = QObject::tr("Update mouths from the library");
  ToonzScene *scene   = TApp::instance()->getCurrentScene()->getScene();
  const QVector<MouthLibraryItem> items = pendingFromLibrary(scene);
  if (items.isEmpty()) {
    if (!onOpen)
      QMessageBox::information(
          parent, title,
          QObject::tr("Nothing to update: this scene has every mouth set of "
                      "its characters' libraries."));
    return;
  }
  QVector<QVector<MouthSet>> chosen;
  if (!chooseSets(parent, title,
                  QObject::tr("The characters' libraries have mouth sets this "
                              "scene's imported copy does not have. A set "
                              "that differs here may be a variant made on "
                              "purpose for this scene: it is not ticked."),
                  items, onOpen ? kAskOnOpenKey : nullptr,
                  QObject::tr("Don't ask again when opening a scene (the "
                              "button in ZtoRig ▸ Mouths stays)"),
                  &chosen))
    return;
  QStringList done, failed;
  for (int i = 0; i < items.size(); i++) {
    if (chosen[i].isEmpty()) continue;
    QString why;
    if (mergeInto(items[i].shotOwner, QString(), items[i].libOwner, chosen[i],
                  &why))
      done << QObject::tr("%1: %2 set(s)").arg(items[i].label).arg(chosen[i].size());
    else
      failed << QObject::tr("%1: %2").arg(items[i].label, why);
  }
  report(parent, title, done, failed);
}

QString ZtoryMouthLibrary::characterKey(const QString &subSceneName) {
  QString key = subSceneName.trimmed().toLower();
  static const QRegularExpression kSuffix(QStringLiteral("_\\d+$"));
  key.remove(kSuffix);
  return key;
}

QHash<QString, QPair<QString, TFilePath>>
ZtoryMouthLibrary::characterScenesByName() {
  QHash<QString, QPair<QString, TFilePath>> out;
  const QHash<QString, Library> libs = librariesByName();
  for (auto it = libs.constBegin(); it != libs.constEnd(); ++it)
    out.insert(it.key(), qMakePair(it.value().character, it.value().scene));
  return out;
}

void ZtoryMouthLibrary::offerOnOpen() {
  if (!QSettings().value(kAskOnOpenKey, true).toBool()) return;
  runWhenNoModal([]() {
    ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
    if (!pendingFromLibrary(scene).isEmpty())
      showUpdateDialog(dialogParent(), true);
  });
}
