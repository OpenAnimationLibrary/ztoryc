#pragma once

// Ztoryc: the picture of an asset, for its preview on Kitsu (2026-09-27).
//
// Franco's choices: the preview goes on the task IN PROGRESS — the first of
// the chain not Done, or the last one when all are Done — it becomes the
// asset's cover on Kitsu, and it is uploaded again when the file changes.
// Not on the last Done while another is in progress: Kitsu runs its status
// automations at EVERY comment, status changed or not
// (zou comments_service.create_comment), so a preview's comment on a Done
// task would run «Done → next Ready» again and send a started task back to
// Ready. Franco's automations all start from Done; a comment on a task that
// is not Done starts none.
// What the file «is» for each type:
//   - Prop / Environment / FX: the linked image (PSD flattened, PNG, JPG…);
//   - Character: the icon of its scene (.tnz), else its PSD to rig.
// Only a file found for sure — linked, exact name or the studio's
// convention — gets a preview: a near name (the blue dot) may be the wrong
// picture, and a wrong cover on Kitsu is worse than none.

#include "ztorymodel.h"

#include <QFileInfoList>
#include <QHash>
#include <QString>
#include <QStringList>

struct ZtoryAssetPreviewSource {
  QString file;       // what the picture is made from; empty = none
  QString signature;  // changes when the file does: name, size, time, asset
  QString    task;    // the task that carries it; empty = none on Kitsu
  TaskStatus status = TaskStatus::Todo;  // its status on Kitsu, unchanged
  bool       nearNameOnly = false;  // a file was deduced, not sure enough
};

// What the preview of `a` would be made from, and on which task it goes.
// `dirCache`: folders already listed, shared by a whole queue.
ZtoryAssetPreviewSource ztoryAssetPreviewSource(
    const Asset &a, QHash<QString, QFileInfoList> *dirCache = nullptr);

// The PSDs a character's scene is built from, read in the .tnz (its levels
// are their layers), in the order they appear; only those that exist.
QStringList ztoryPsdsUsedByScene(const QString &tnz);

// Writes the picture of `file` as a PNG at most 1920 px on the long side.
// False (with `why`) if the file cannot be read.
bool ztoryRenderAssetPreview(const QString &file, const QString &outPng,
                             QString *why);
