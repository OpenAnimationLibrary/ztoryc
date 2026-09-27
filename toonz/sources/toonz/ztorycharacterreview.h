#pragma once

// Ztoryc: sending a character's rig for approval when its scene is closed
// (Franco, 2026-09-26). Kept apart from ztorycharacter, which is plain file
// and XML work used by the mouth sets too: this one opens dialogs, renders a
// frame and talks to Kitsu.

namespace ZtoryCharacterReview {

// Called while the current scene is being closed (new scene, another scene,
// quit), after the user has answered the save question. If it is a character
// scene whose task (Rigging) is in WIP, asks whether to send it for approval:
// on yes, renders the current frame to <project>/previews/, uploads it to
// the Kitsu task — which sets WFA there — and mirrors WFA locally. Without
// Kitsu in the project, WFA is set locally and the preview is kept on disk.
// Does nothing if `changesDiscarded`: a preview would show discarded work.
void offerWfaOnClose(bool changesDiscarded);

}  // namespace ZtoryCharacterReview
