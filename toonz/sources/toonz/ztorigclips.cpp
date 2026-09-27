#include "ztorigclips.h"

#include "tapp.h"
#include "ztoriglibrary.h"

#include "ext/plasticskeletondeformation.h"
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "tstream.h"
#include "tsystem.h"
#include "tundo.h"
#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/tframehandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>
#include <map>
#include <set>

namespace {

// ── A clip in memory ─────────────────────────────────────────────────────

struct ClipCurve {
  enum Kind { Stage = 0, Vertex = 1, Guide = 2 } kind = Stage;
  QString column;
  int channel = 0;   // Stage: TStageObject::Channel; Vertex: SkVD param
  QString name;      // Vertex: vertex name; Guide: action name
  TDoubleParamP keys;  // from frame 0
};

struct ClipCell {
  QString column;
  int row = 0;  // from 0
  QString level;
  int frame = 0;
  QString letter;
};

struct Clip {
  QString name, tags;
  int rows = 0;  // how many rows the clip covers: its period when repeated
  bool drawings = false;
  QVector<ClipCurve> curves;
  QVector<ClipCell> cells;
};

QString columnName(TXsheet *xsh, int col) {
  TStageObject *obj = xsh->getStageObject(TStageObjectId::ColumnId(col));
  QString n = obj ? QString::fromStdString(obj->getName()) : QString();
  return n.isEmpty() ? QString("Col%1").arg(col + 1) : n;
}

int columnByName(TXsheet *xsh, const QString &name) {
  for (int c = 0; c < xsh->getColumnCount(); c++)
    if (columnName(xsh, c) == name) return c;
  return -1;
}

// The keys of `src` in [f0, f1], moved to start at 0. The curve's value at
// the two ends is keyed too, so the clip starts and ends in the pose it had
// there even when no key fell exactly on them. Null for a curve with no keys:
// nothing animated to take.
TDoubleParamP extractRange(TDoubleParam *src, double f0, double f1) {
  if (!src || src->getKeyframeCount() == 0) return TDoubleParamP();
  TDoubleParamP out = new TDoubleParam(*src);
  out->clearKeyframes();
  const double eps = 1e-6, len = f1 - f0;
  bool atStart = false, atEnd = false, any = false;
  // The segment a frame falls in is governed by the key after it: its type
  // is the one the added end keys take.
  TDoubleKeyframe::Type startType = TDoubleKeyframe::Linear,
                        endType   = TDoubleKeyframe::Linear;
  for (int i = 0; i < src->getKeyframeCount(); i++) {
    TDoubleKeyframe kf = src->getKeyframe(i);
    if (kf.m_frame > f0 + eps && startType == TDoubleKeyframe::Linear &&
        i > 0)
      startType = kf.m_type;
    if (kf.m_frame >= f1 - eps && endType == TDoubleKeyframe::Linear && i > 0)
      endType = kf.m_type;
    if (kf.m_frame < f0 - eps || kf.m_frame > f1 + eps) continue;
    kf.m_frame -= f0;
    if (std::abs(kf.m_frame) < eps) atStart = true;
    if (std::abs(kf.m_frame - len) < eps) atEnd = true;
    out->setKeyframe(kf);
    any = true;
  }
  if (!any && src->getValue(f0) == src->getValue(f1)) {
    // No key inside and flat across: the curve does not move here.
    bool moves = false;
    for (double f = f0; f <= f1 && !moves; f += 1)
      moves = src->getValue(f) != src->getValue(f0);
    if (!moves) return TDoubleParamP();
  }
  if (!atStart) {
    TDoubleKeyframe k(0, src->getValue(f0));
    k.m_type = startType;
    out->setKeyframe(k);
  }
  if (!atEnd && len > eps) {
    TDoubleKeyframe k(len, src->getValue(f1));
    k.m_type = endType;
    out->setKeyframe(k);
  }
  return out;
}

Clip capture(TXsheet *xsh, int r0, int r1, bool withCells) {
  Clip clip;
  clip.rows     = r1 - r0 + 1;
  clip.drawings = withCells;
  for (int c = 0; c < xsh->getColumnCount(); c++) {
    TStageObject *obj = xsh->getStageObject(TStageObjectId::ColumnId(c));
    if (!obj) continue;
    const QString col = columnName(xsh, c);
    for (int ch = 0; ch < TStageObject::T_ChannelCount; ch++) {
      TDoubleParamP k = extractRange(obj->getParam(TStageObject::Channel(ch)),
                                     r0, r1);
      if (k) clip.curves << ClipCurve{ClipCurve::Stage, col, ch, QString(), k};
    }
    if (const PlasticSkeletonDeformationP &sd =
            obj->getPlasticSkeletonDeformation()) {
      PlasticSkeletonDeformation::vd_iterator vb, ve;
      sd->vertexDeformations(vb, ve);
      for (; vb != ve; ++vb) {
        const QString vname = *(*vb).first;
        SkVD *vd            = (*vb).second;
        for (int p = 0; p < SkVD::PARAMS_COUNT; p++) {
          TDoubleParamP k = extractRange(vd->m_params[p].getPointer(), r0, r1);
          if (k) clip.curves << ClipCurve{ClipCurve::Vertex, col, p, vname, k};
        }
      }
      for (int a = 0; a < sd->poseActionsCount(); a++) {
        const PoseAction *act = sd->poseAction(a);
        if (!act || !act->m_guide) continue;
        TDoubleParamP k = extractRange(act->m_guide.getPointer(), r0, r1);
        if (k)
          clip.curves << ClipCurve{ClipCurve::Guide, col, 0, act->m_name, k};
      }
    }
    if (withCells) {
      for (int r = r0; r <= r1; r++) {
        const TXshCell cell = xsh->getCell(r, c);
        if (cell.isEmpty() || !cell.m_level) continue;
        ClipCell cc;
        cc.column = col;
        cc.row    = r - r0;
        cc.level  = QString::fromStdWString(cell.m_level->getName());
        cc.frame  = cell.m_frameId.getNumber();
        cc.letter = cell.m_frameId.getLetter();
        clip.cells << cc;
      }
    }
  }
  return clip;
}

// ── On disk ──────────────────────────────────────────────────────────────

TFilePath clipsFolder(const TFilePath &charScene) {
  return charScene.getParentDir() +
         TFilePath((charScene.getWideName() + L"_clips"));
}

bool saveClip(const TFilePath &file, const Clip &clip) {
  try {
    TSystem::touchParentDir(file);
    TOStream os(file);
    std::map<std::string, std::string> attr;
    attr["name"]     = clip.name.toStdString();
    attr["rows"]     = std::to_string(clip.rows);
    attr["drawings"] = clip.drawings ? "1" : "0";
    os.openChild("ztorigClip", attr);
    os.child("tags") << clip.tags.toStdString();
    for (const ClipCurve &cv : clip.curves) {
      std::map<std::string, std::string> a;
      a["kind"]    = std::to_string(int(cv.kind));
      a["column"]  = cv.column.toStdString();
      a["channel"] = std::to_string(cv.channel);
      if (!cv.name.isEmpty()) a["name"] = cv.name.toStdString();
      os.openChild("curve", a);
      cv.keys->saveData(os);
      os.closeChild();
    }
    for (const ClipCell &cc : clip.cells) {
      std::map<std::string, std::string> a;
      a["column"] = cc.column.toStdString();
      a["row"]    = std::to_string(cc.row);
      a["level"]  = cc.level.toStdString();
      a["frame"]  = std::to_string(cc.frame);
      if (!cc.letter.isEmpty()) a["letter"] = cc.letter.toStdString();
      os.openChild("cell", a);
      os.closeChild();
    }
    os.closeChild();
    return true;
  } catch (...) {
    return false;
  }
}

bool loadClip(const TFilePath &file, Clip *clip) {
  try {
    TIStream is(file);
    std::string tag, v;
    if (!is.matchTag(tag) || tag != "ztorigClip") return false;
    if (is.getTagParam("name", v)) clip->name = QString::fromStdString(v);
    int n = 0;
    if (is.getTagParam("rows", n)) clip->rows = n;
    if (is.getTagParam("drawings", v)) clip->drawings = v == "1";
    while (is.matchTag(tag)) {
      if (tag == "tags") {
        std::string t;
        if (!is.eos()) is >> t;
        clip->tags = QString::fromStdString(t);
        is.matchEndTag();
      } else if (tag == "curve") {
        ClipCurve cv;
        int k = 0;
        is.getTagParam("kind", k);
        cv.kind = ClipCurve::Kind(k);
        if (is.getTagParam("column", v)) cv.column = QString::fromStdString(v);
        is.getTagParam("channel", cv.channel);
        if (is.getTagParam("name", v)) cv.name = QString::fromStdString(v);
        cv.keys = new TDoubleParam();
        cv.keys->loadData(is);
        is.matchEndTag();
        clip->curves << cv;
      } else if (tag == "cell") {
        ClipCell cc;
        if (is.getTagParam("column", v)) cc.column = QString::fromStdString(v);
        is.getTagParam("row", cc.row);
        if (is.getTagParam("level", v)) cc.level = QString::fromStdString(v);
        is.getTagParam("frame", cc.frame);
        if (is.getTagParam("letter", v)) cc.letter = QString::fromStdString(v);
        is.matchEndTag();
        clip->cells << cc;
      } else
        is.skipCurrentTag();
    }
    is.matchEndTag();
    // A clip with no length cannot be pasted: the insert loops by it.
    return clip->rows > 0;
  } catch (...) {
    return false;
  }
}

// ── Inserting ────────────────────────────────────────────────────────────

class ClipUndo final : public TUndo {
public:
  struct Curve {
    TDoubleParamP param;
    TDoubleParamP before, after;
  };
  struct Cell {
    int col, row;
    TXshCell before, after;
  };
  TXsheetP m_xsh;  // held: the character's sub-scene may go while undo stays
  std::vector<Curve> m_curves;
  std::vector<Cell> m_cells;
  QString m_name;
  // Inserted (the rest pushed forward): undo takes the rows out again, redo
  // puts them back and the clip's cells in them.
  bool m_inserted = false;
  int m_at = 0, m_rows = 0;

  void put(bool after) const {
    if (m_inserted) {
      for (int col = 0; col < m_xsh->getColumnCount(); col++) {
        if (after)
          m_xsh->insertCells(m_at, col, m_rows);
        else
          m_xsh->removeCells(m_at, col, m_rows);
      }
      if (after)
        for (const Cell &c : m_cells) m_xsh->setCell(c.row, c.col, c.after);
    } else
      for (const Cell &c : m_cells)
        m_xsh->setCell(c.row, c.col, after ? c.after : c.before);
    // The curves AFTER the cells: with «Keyframes follow exposure» on,
    // insertCells/removeCells move the keys too, and would spoil the ones
    // just put back. copy() and not «=»: the assignment puts the keys back
    // WITHOUT telling anyone (tdoubleparam.cpp, operator=), so the viewer and
    // the function editor kept showing the inserted keys (Franco, 2026-09-27).
    for (const Curve &c : m_curves)
      c.param->copy(after ? c.after.getPointer() : c.before.getPointer());
    for (int col = 0; col < m_xsh->getColumnCount(); col++)
      if (TStageObject *o = m_xsh->getStageObject(TStageObjectId::ColumnId(col)))
        o->invalidate();
    TApp::instance()->getCurrentXsheet()->notifyXsheetChanged();
    TApp::instance()->getCurrentScene()->setDirtyFlag(true);
  }
  void undo() const override { put(false); }
  void redo() const override { put(true); }
  int getSize() const override { return sizeof(*this) + 4096; }
  QString getHistoryString() override {
    return QObject::tr("ZtoRig: Insert Clip %1").arg(m_name);
  }
};

TDoubleParam *targetParam(TXsheet *xsh, const ClipCurve &cv) {
  const int c = columnByName(xsh, cv.column);
  if (c < 0) return nullptr;
  TStageObject *obj = xsh->getStageObject(TStageObjectId::ColumnId(c));
  if (!obj) return nullptr;
  if (cv.kind == ClipCurve::Stage)
    return cv.channel >= 0 && cv.channel < TStageObject::T_ChannelCount
               ? obj->getParam(TStageObject::Channel(cv.channel))
               : nullptr;
  const PlasticSkeletonDeformationP &sd = obj->getPlasticSkeletonDeformation();
  if (!sd) return nullptr;
  if (cv.kind == ClipCurve::Vertex) {
    SkVD *vd = sd->vertexDeformation(cv.name);
    return vd && cv.channel >= 0 && cv.channel < SkVD::PARAMS_COUNT
               ? vd->m_params[cv.channel].getPointer()
               : nullptr;
  }
  PoseAction *act = sd->poseAction(cv.name);
  return act && act->m_guide ? act->m_guide.getPointer() : nullptr;
}

// A level's name without the «_1» Tahoma appends to keep names unique when a
// scene is brought in: the same drawing is «sub_5_mesh» in the character's
// scene and «sub_5_mesh_1» in a shot (2026-09-27).
QString baseLevelName(const QString &name) {
  QString n = name.trimmed();
  static const QRegularExpression kSuffix(QStringLiteral("_\\d+$"));
  n.remove(kSuffix);
  return n;
}

// The level a clip's cell names, in this scene: the exact name; else, among
// the levels the target column already shows, the same name but for the
// «_N»; else any level of the scene so named.
TXshLevel *findLevel(ToonzScene *scene, TXsheet *xsh, int col,
                     const QString &name) {
  TLevelSet *ls = scene->getLevelSet();
  if (TXshLevel *lv = ls->getLevel(name.toStdWString())) return lv;
  const QString base = baseLevelName(name);
  if (col >= 0) {
    int r0 = 0, r1 = -1;
    xsh->getCellRange(col, r0, r1);
    for (int r = r0; r <= r1; r++) {
      const TXshCell cell = xsh->getCell(r, col);
      if (cell.m_level &&
          baseLevelName(QString::fromStdWString(cell.m_level->getName())) ==
              base)
        return cell.m_level.getPointer();
    }
  }
  for (int i = 0; i < ls->getLevelCount(); i++)
    if (baseLevelName(QString::fromStdWString(ls->getLevel(i)->getName())) ==
        base)
      return ls->getLevel(i);
  return nullptr;
}

// A column is the character's ROOT when its parent is not another column:
// its X/Y is where the character walks to.
bool isRootColumn(TXsheet *xsh, int c) {
  TStageObject *obj = xsh->getStageObject(TStageObjectId::ColumnId(c));
  return obj && !obj->getParent().isColumn();
}

struct InsertOptions {
  int at      = 0;    // first row
  int repeats = 1;
  double speed = 1.0;  // 2 = twice as fast
  bool drawings = true;
  bool travel   = true;  // the walk goes on from one repeat to the next
  // INSERT: what follows is pushed forward by the clip's length (Franco,
  // 2026-09-27: «se mi metto sul frame 10 e faccio un insert di una clip da
  // 20 ftg deve slittare tutto di 20 frame»). Off = overwrite in place.
  bool insert = true;
};

// Every animated curve of the character's xsheet: the columns' movement, the
// skeleton's vertices, the pose sliders — what an insert pushes forward.
std::vector<TDoubleParam *> animatedCurves(TXsheet *xsh) {
  std::vector<TDoubleParam *> out;
  // Each curve ONCE: columns can share one deformation, and a curve listed
  // twice was pushed forward twice — too far — and its «before» snapshot was
  // taken after the first push, so undo did not come back either (Franco,
  // 2026-09-27).
  std::set<TDoubleParam *> seen;
  auto take = [&out, &seen](TDoubleParam *p) {
    if (p && p->getKeyframeCount() > 0 && seen.insert(p).second)
      out.push_back(p);
  };
  for (int c = 0; c < xsh->getColumnCount(); c++) {
    TStageObject *obj = xsh->getStageObject(TStageObjectId::ColumnId(c));
    if (!obj) continue;
    for (int ch = 0; ch < TStageObject::T_ChannelCount; ch++)
      take(obj->getParam(TStageObject::Channel(ch)));
    if (const PlasticSkeletonDeformationP &sd =
            obj->getPlasticSkeletonDeformation()) {
      PlasticSkeletonDeformation::vd_iterator vb, ve;
      sd->vertexDeformations(vb, ve);
      for (; vb != ve; ++vb)
        for (int p = 0; p < SkVD::PARAMS_COUNT; p++)
          take((*vb).second->m_params[p].getPointer());
      for (int a = 0; a < sd->poseActionsCount(); a++)
        if (const PoseAction *act = sd->poseAction(a))
          take(act->m_guide.getPointer());
    }
  }
  return out;
}

// The keys of `p` at or after `from`, moved `by` frames later.
void shiftKeys(TDoubleParam *p, double from, double by) {
  std::vector<TDoubleKeyframe> moved;
  for (int i = p->getKeyframeCount() - 1; i >= 0; --i) {
    TDoubleKeyframe kf = p->getKeyframe(i);
    if (kf.m_frame < from - 1e-6) continue;
    p->deleteKeyframe(kf.m_frame);
    kf.m_frame += by;
    moved.push_back(kf);
  }
  for (const TDoubleKeyframe &kf : moved) p->setKeyframe(kf);
}

// Pastes the clip; returns the curves that found no place (column or vertex
// missing) so the user is told.
QStringList insertClip(TXsheet *xsh, const Clip &clip, const InsertOptions &o,
                       ClipUndo *undo) {
  QStringList missing;
  const double stretch = 1.0 / std::max(0.05, o.speed);
  const double period  = clip.rows * stretch;
  const double total   = period * o.repeats;
  const int rowsTotal  = int(std::ceil(total));

  // INSERT: first make room. Every animated curve of the character — also
  // those the clip does not touch — moves its keys from `at` on by the
  // clip's length, and every column gets empty rows there.
  std::map<TDoubleParam *, TDoubleParamP> before;  // curve -> as it was
  if (o.insert) {
    const auto curves = animatedCurves(xsh);
    for (TDoubleParam *p : curves) before[p] = new TDoubleParam(*p);
    for (int c = 0; c < xsh->getColumnCount(); c++) {
      // What the column shows where the clip goes in: held over the inserted
      // rows when the clip brings no drawings of its own — keys over empty
      // rows move nothing, there is no drawing and no mesh there (Franco,
      // 2026-09-27).
      TXshCell held = xsh->getCell(o.at, c);
      if (held.isEmpty() && o.at > 0) held = xsh->getCell(o.at - 1, c);
      xsh->insertCells(o.at, c, rowsTotal);
      const bool clipFills = o.drawings && clip.drawings;
      if (!clipFills && !held.isEmpty())
        for (int r = 0; r < rowsTotal; r++) {
          xsh->setCell(o.at + r, c, held);
          undo->m_cells.push_back({c, o.at + r, TXshCell(), held});
        }
    }
    // The keys after the cells: with «Keyframes follow exposure» on,
    // insertCells has already moved some of them. Start again from the
    // snapshot, so they move once, by the clip's length.
    for (TDoubleParam *p : curves) {
      p->copy(before[p].getPointer());
      shiftKeys(p, o.at, rowsTotal);
    }
    undo->m_inserted = true;
    undo->m_at       = o.at;
    undo->m_rows     = rowsTotal;
  }

  for (const ClipCurve &cv : clip.curves) {
    TDoubleParam *dst = targetParam(xsh, cv);
    if (!dst) {
      const QString what = cv.kind == ClipCurve::Vertex
                               ? cv.column + " ▸ " + cv.name
                               : cv.kind == ClipCurve::Guide
                                     ? cv.column + " ▸ " + cv.name
                                     : cv.column;
      if (!missing.contains(what)) missing << what;
      continue;
    }
    if (!before.count(dst)) before[dst] = new TDoubleParam(*dst);
    // Overwrite: the rows the clip covers are the clip's, keys already there
    // go. Not after an insert: the room is empty, and the key that moved on to
    // exactly the end of it was being deleted with them.
    if (!o.insert)
      for (int i = dst->getKeyframeCount() - 1; i >= 0; --i) {
        const double f = dst->getKeyframe(i).m_frame;
        if (f >= o.at - 1e-6 && f < o.at + total - 1e-6) dst->deleteKeyframe(f);
      }
    // A walk moves on: each repeat starts where the last one would have
    // arrived — the step of one period, extrapolated from its keys.
    double step = 0.0;
    if (o.travel && cv.kind == ClipCurve::Stage &&
        (cv.channel == TStageObject::T_X || cv.channel == TStageObject::T_Y)) {
      const int c = columnByName(xsh, cv.column);
      const int n = cv.keys->getKeyframeCount();
      if (c >= 0 && isRootColumn(xsh, c) && n > 1 && clip.rows > 1) {
        const double last = cv.keys->getKeyframe(n - 1).m_frame;
        if (last > 0)
          step = (cv.keys->getValue(last) - cv.keys->getValue(0)) *
                 clip.rows / last;
      }
    }
    for (int r = 0; r < o.repeats; r++)
      for (int i = 0; i < cv.keys->getKeyframeCount(); i++) {
        TDoubleKeyframe kf = cv.keys->getKeyframe(i);
        kf.m_frame = o.at + r * period + kf.m_frame * stretch;
        if (kf.m_type != TDoubleKeyframe::Expression) kf.m_value += r * step;
        kf.m_speedIn.x *= stretch;
        kf.m_speedOut.x *= stretch;
        dst->setKeyframe(kf);
      }
  }
  // Every curve that changed — shifted, pasted or both — as it was and is.
  for (const auto &b : before) {
    ClipUndo::Curve u;
    u.param  = b.first;
    u.before = b.second;
    u.after  = new TDoubleParam(*b.first);
    undo->m_curves.push_back(u);
  }

  if (o.drawings && clip.drawings && !clip.cells.isEmpty()) {
    ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
    // Per column, the clip's row -> cell; the target rows are filled from
    // it, so a slower clip holds each drawing longer.
    QMap<QString, QMap<int, TXshCell>> byColumn;
    for (const ClipCell &cc : clip.cells) {
      TXshLevel *lv = findLevel(scene, xsh, columnByName(xsh, cc.column),
                                cc.level);
      if (!lv) {
        const QString what = QObject::tr("drawing %1").arg(cc.level);
        if (!missing.contains(what)) missing << what;
        continue;
      }
      byColumn[cc.column][cc.row] =
          TXshCell(lv, TFrameId(cc.frame, cc.letter));
    }
    for (auto it = byColumn.constBegin(); it != byColumn.constEnd(); ++it) {
      const int c = columnByName(xsh, it.key());
      if (c < 0) continue;
      for (int t = 0; t < rowsTotal; t++) {
        const int src = int(std::floor(t / stretch)) % clip.rows;
        // The last cell at or before `src`: a clip row with no cell of its
        // own is a hold of the one before.
        auto cell = it.value().upperBound(src);
        if (cell == it.value().constBegin()) continue;
        --cell;
        ClipUndo::Cell u;
        u.col    = c;
        u.row    = o.at + t;
        u.before = xsh->getCell(u.row, c);
        u.after  = cell.value();
        xsh->setCell(u.row, c, u.after);
        undo->m_cells.push_back(u);
      }
    }
  }
  for (int c = 0; c < xsh->getColumnCount(); c++)
    if (TStageObject *obj = xsh->getStageObject(TStageObjectId::ColumnId(c)))
      obj->invalidate();
  return missing;
}

// The character one works in, and that one IS inside its xsheet: the clip's
// rows are that xsheet's rows.
bool characterHere(QWidget *parent, const QString &title, QString *name,
                   TFilePath *scene, TXsheet **xsh) {
  if (!ZtoRigLibrary::currentCharacter(name, scene, xsh) ||
      TApp::instance()->getCurrentXsheet()->getXsheet() != *xsh) {
    QMessageBox::information(
        parent, title,
        QObject::tr("Open the character's scene, or its sub-scene inside the "
                    "shot: a clip is made of — and goes into — the "
                    "character's own rows."));
    return false;
  }
  return true;
}

}  // namespace

//----------------------------------------------------------------------------

void ZtoRigClips::showSaveDialog(QWidget *parent) {
  const QString title = QObject::tr("Save Animation Clip");
  QString character;
  TFilePath charScene;
  TXsheet *xsh = nullptr;
  if (!characterHere(parent, title, &character, &charScene, &xsh)) return;

  int r0 = 0, r1 = std::max(0, xsh->getFrameCount() - 1);
  int in = 0, out = -1;
  xsh->getInOutMarkers(in, out);
  if (out >= in && out >= 0) { r0 = in; r1 = out; }

  QDialog dlg(parent);
  dlg.setWindowTitle(title);
  auto *lay  = new QVBoxLayout(&dlg);
  auto *form = new QFormLayout();
  auto *name = new QLineEdit(&dlg);
  auto *tags = new QLineEdit(&dlg);
  tags->setPlaceholderText(QObject::tr("walk, jump, acting…"));
  auto *from = new QSpinBox(&dlg);
  auto *to   = new QSpinBox(&dlg);
  from->setRange(1, 100000);
  to->setRange(1, 100000);
  from->setValue(r0 + 1);
  to->setValue(r1 + 1);
  form->addRow(QObject::tr("Name:"), name);
  form->addRow(QObject::tr("Tags:"), tags);
  form->addRow(QObject::tr("From frame:"), from);
  form->addRow(QObject::tr("To frame:"), to);
  lay->addLayout(form);
  auto *note = new QLabel(
      QObject::tr("For a cycle, select it WITHOUT repeating the first pose at "
                  "the end: repeated, the clip starts again right after its "
                  "last frame."),
      &dlg);
  note->setWordWrap(true);
  lay->addWidget(note);
  auto *bb = new QDialogButtonBox(QDialogButtonBox::Save |
                                      QDialogButtonBox::Cancel,
                                  &dlg);
  QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  lay->addWidget(bb);
  if (dlg.exec() != QDialog::Accepted) return;

  const QString clipName = name->text().trimmed();
  if (clipName.isEmpty() || to->value() < from->value()) {
    QMessageBox::warning(parent, title,
                         QObject::tr("A name, and a range that ends after it "
                                     "starts."));
    return;
  }
  // Always with the drawings: whether they go in is chosen at INSERT time,
  // each time (Franco, 2026-09-27: «a volte potrei volere solo le chiavi, a
  // volte anche i disegni»).
  Clip clip  = capture(xsh, from->value() - 1, to->value() - 1, true);
  clip.name  = clipName;
  clip.tags  = tags->text().trimmed();
  if (clip.curves.isEmpty() && clip.cells.isEmpty()) {
    QMessageBox::information(parent, title,
                             QObject::tr("Nothing moves in these frames: there "
                                         "is no animation to save."));
    return;
  }
  QString file = clipName;
  file.replace(QRegExp("[\\\\/:*?\"<>|]"), "_");
  const TFilePath path =
      clipsFolder(charScene) + TFilePath((file + ".zclip").toStdWString());
  if (QFileInfo::exists(path.getQString()) &&
      QMessageBox::question(parent, title,
                            QObject::tr("%1 has a clip «%2» already. Replace "
                                        "it?")
                                .arg(character, clipName)) != QMessageBox::Yes)
    return;
  if (!saveClip(path, clip)) {
    QMessageBox::warning(parent, title,
                         QObject::tr("Could not write %1").arg(path.getQString()));
    return;
  }
  QMessageBox::information(
      parent, title,
      QObject::tr("«%1» saved in %2's library: %3 frames, %4 curves%5.")
          .arg(clipName, character)
          .arg(clip.rows)
          .arg(clip.curves.size())
          .arg(clip.drawings ? QObject::tr(", with drawings") : QString()));
}

void ZtoRigClips::showInsertDialog(QWidget *parent) {
  const QString title = QObject::tr("Insert Animation Clip");
  QString character;
  TFilePath charScene;
  TXsheet *xsh = nullptr;
  if (!characterHere(parent, title, &character, &charScene, &xsh)) return;

  const QDir dir(clipsFolder(charScene).getQString());
  const QStringList files =
      dir.entryList(QStringList() << "*.zclip", QDir::Files, QDir::Name);
  QVector<Clip> clips;
  QVector<TFilePath> paths;
  for (const QString &f : files) {
    Clip c;
    const TFilePath p(dir.absoluteFilePath(f).toStdWString());
    if (loadClip(p, &c)) {
      clips << c;
      paths << p;
    }
  }
  if (clips.isEmpty()) {
    QMessageBox::information(parent, title,
                             QObject::tr("%1 has no clips yet: save one with "
                                         "«Save Clip…».")
                                 .arg(character));
    return;
  }

  QDialog dlg(parent);
  dlg.setWindowTitle(title);
  dlg.setMinimumWidth(420);
  auto *lay  = new QVBoxLayout(&dlg);
  auto *list = new QListWidget(&dlg);
  for (const Clip &c : clips)
    list->addItem(QObject::tr("%1  —  %2 frames%3%4")
                      .arg(c.name)
                      .arg(c.rows)
                      .arg(c.drawings ? QObject::tr(", drawings") : QString())
                      .arg(c.tags.isEmpty() ? QString() : "  [" + c.tags + "]"));
  list->setCurrentRow(0);
  lay->addWidget(list);
  auto *form    = new QFormLayout();
  auto *at      = new QSpinBox(&dlg);
  auto *repeats = new QSpinBox(&dlg);
  auto *speed   = new QDoubleSpinBox(&dlg);
  at->setRange(1, 100000);
  at->setValue(TApp::instance()->getCurrentFrame()->getFrame() + 1);
  repeats->setRange(1, 999);
  speed->setRange(10, 1000);
  speed->setSuffix(" %");
  speed->setValue(100);
  auto *what = new QComboBox(&dlg);
  what->addItem(QObject::tr("Keys and drawings"));
  what->addItem(QObject::tr("Keys only (movement, skeleton, pose sliders)"));
  auto *overwrite = new QCheckBox(
      QObject::tr("Overwrite instead of insert (what follows stays where it "
                  "is)"),
      &dlg);
  auto *travel = new QRadioButton(
      QObject::tr("Repeats move on (a walk goes forward)"), &dlg);
  auto *inPlace = new QRadioButton(QObject::tr("Repeats in place"), &dlg);
  travel->setChecked(true);
  form->addRow(QObject::tr("At frame:"), at);
  form->addRow(QObject::tr("Repeat:"), repeats);
  form->addRow(QObject::tr("Speed:"), speed);
  form->addRow(QObject::tr("Insert:"), what);
  form->addRow(QString(), overwrite);
  form->addRow(QString(), travel);
  form->addRow(QString(), inPlace);
  lay->addLayout(form);
  // A clip saved before the drawings were always kept has none to offer.
  auto sync = [&]() {
    const int i = list->currentRow();
    const bool has = i >= 0 && clips[i].drawings;
    what->setEnabled(has);
    if (!has) what->setCurrentIndex(1);
  };
  QObject::connect(list, &QListWidget::currentRowChanged, &dlg, sync);
  sync();
  auto *bb = new QDialogButtonBox(&dlg);
  bb->addButton(QObject::tr("Insert"), QDialogButtonBox::AcceptRole);
  bb->addButton(QDialogButtonBox::Cancel);
  QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  lay->addWidget(bb);
  if (dlg.exec() != QDialog::Accepted || list->currentRow() < 0) return;

  const Clip &clip = clips[list->currentRow()];
  InsertOptions o;
  o.at       = at->value() - 1;
  o.repeats  = repeats->value();
  o.speed    = speed->value() / 100.0;
  o.drawings = what->isEnabled() && what->currentIndex() == 0;
  o.travel   = travel->isChecked();
  o.insert   = !overwrite->isChecked();
  auto *undo   = new ClipUndo();
  undo->m_xsh  = xsh;
  undo->m_name = clip.name;
  const QStringList missing = insertClip(xsh, clip, o, undo);
  TUndoManager::manager()->add(undo);
  TApp::instance()->getCurrentXsheet()->notifyXsheetChanged();
  TApp::instance()->getCurrentScene()->setDirtyFlag(true);
  if (!missing.isEmpty())
    QMessageBox::information(
        parent, title,
        QObject::tr("Inserted, but these parts of the clip have no place in "
                    "this character and were left out:\n\n%1")
            .arg(missing.join("\n")));
}
