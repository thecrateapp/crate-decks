#pragma once

#include <Qt>

namespace crate::delegates {

/// The item-data roles that the delegates of src/crate/ui/delegates/ read.
/// A model that wants a Crate cell (F3's track models, F4's cache state,
/// F5's compatibles) returns its data under these roles; the delegates never
/// look at a model's columns. They start well above upstream's own roles
/// (TrackModel and SidebarModel use Qt::UserRole to Qt::UserRole + 3).
enum Role : int {
    /// QString, the Camelot key of a track, such as "8A". Without it the
    /// Camelot delegate reads the display text of a key column, in any
    /// notation upstream knows, and shows it in Camelot.
    CamelotRole = Qt::UserRole + 0x200,
    /// int, a CamelotTone: how the key relates to the master deck's.
    CamelotToneRole,
    /// int, a ProfileQuality: how complete Crate's mix profile is.
    ProfileQualityRole,
    /// int, a CacheState: where the track's audio is.
    CacheStateRole,
    /// double from 0 to 1, how much of a download is done.
    CacheProgressRole,
    /// QString, the letter of the deck that has the track loaded ("A" to
    /// "D"); the track table marks the row and the title with its colour.
    LoadedDeckRole,
    /// QString, on a top-level sidebar item: the section it belongs to,
    /// shown as a MonoLabel header above the section's first item.
    SidebarSectionRole,
    /// QString, a count shown in mono on the right of a sidebar item.
    SidebarCountRole,
};

/// How a track's key relates to the master deck's key (design 14.3).
enum class CamelotTone {
    Rest = 0,
    /// Compatible: the same key, ±1, the relative key or +7.
    Compatible,
    /// The master deck's own key.
    Master,
};

/// Crate's mix profile of a track (design 14.3, Crate states).
enum class ProfileQuality {
    Unknown = 0,
    Full,
    Partial,
    Missing,
};

/// Where a track's audio is (design 14.3, Crate states).
enum class CacheState {
    None = 0,
    Ready,
    Downloading,
    Failed,
};

} // namespace crate::delegates
