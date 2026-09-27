#pragma once

// ZtoRig: the character's animation library — a walk, a jump, a piece of
// acting, saved from a shot and inserted again, then edited like any other
// animation (Franco, 2026-09-27).
//
// A clip is a STRETCH OF TIME, not a state (a pose is a state:
// ztoriglibrary.h). It holds, for the rows chosen, the keys of the character's
// columns — their movement, the angles and distances of the skeleton's
// vertices by NAME, the pose sliders — and, when asked, the drawings exposed
// (which hand, which mouth). One file per clip, in a folder beside the
// character's scene: SOFIA_clips/walk.zclip, in Tahoma's own stream format so
// every key keeps its interpolation, handles and expressions.
//
// Inserted: once, or repeated, faster or slower; for a walk the forward
// movement either continues from one repeat to the next or restarts (in
// place). After that they are ordinary keys and cells.

class QWidget;

namespace ZtoRigClips {

void showSaveDialog(QWidget *parent);
void showInsertDialog(QWidget *parent);

}  // namespace ZtoRigClips
