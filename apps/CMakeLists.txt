# apps/ — host applications. These are CONSUMERS of the SDK, never part of it:
# nothing in base/ media/ player/ render/ platform/ may include an apps/ header,
# which is the same rule C28 enforces for render/ (docs/02 §6). Keeping the UI
# here is what lets the core stay UI-framework-agnostic.
#
# Each app is gated by its own option so a machine without the toolkit still
# builds the rest of the tree.

if(AVBASE_BUILD_QT_PLAYER)
  add_subdirectory(player_qt)
endif()
