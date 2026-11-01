#pragma once

#include <QString>
#include <QStringList>

// P-217: combined filter conditions over one asset's facts. Every condition is
// ANDed; within the tag set an asset matches if it has ANY listed tag (OR).
// Name matches the logical path (folder/name) case-insensitively. Rating:
// -2 = no rating condition, -1 = explicit "no rating" (only no-rating assets
// match), 0..5 = at least N stars (a no-rating asset does NOT satisfy ">= N").
struct AssetFilter {
    QString name;        // substring of the logical path; empty = no condition
    QStringList tags;    // any-of; empty = no condition
    QString format;      // exact detected format (e.g. "PNG"); empty = no condition
    int minWidth = 0;    // dimension floor; 0 = no condition
    int minHeight = 0;
    int minRating = -2;  // -2 none, -1 explicit no-rating, 0..5 >= N stars
    bool active() const {
        return !name.isEmpty() || !tags.isEmpty() || !format.isEmpty()
            || minWidth > 0 || minHeight > 0 || minRating != -2;
    }
};

// The facts of one asset as seen by the filter: everything is derived from the
// catalog (format/size from the last committed revision), nothing is inferred.
struct AssetFacts {
    QString name;
    QString folder;
    QStringList tags;
    QString format;   // detected format, empty = unknown
    int width = 0;    // 0 = unknown
    int height = 0;
    int rating = -1;  // -1 = no rating
    QString logicalPath() const {
        return folder.isEmpty() ? name : folder + QStringLiteral("/") + name;
    }
};

// Pure matching: no I/O, no inference. A no-rating asset fails a ">= N" rating
// condition (no rating is not a score) but passes an explicit "no rating" one.
inline bool filterMatches(const AssetFacts &facts, const AssetFilter &c) {
    if (!c.name.isEmpty() && !facts.logicalPath().contains(c.name, Qt::CaseInsensitive))
        return false;
    if (!c.tags.isEmpty()) {
        bool any = false;
        for (const QString &tag : c.tags)
            if (facts.tags.contains(tag.trimmed())) { any = true; break; }
        if (!any) return false;
    }
    if (!c.format.isEmpty() && facts.format.compare(c.format, Qt::CaseInsensitive) != 0)
        return false;
    if (c.minWidth > 0 || c.minHeight > 0) {
        if (facts.width < c.minWidth || facts.height < c.minHeight) return false;
    }
    if (c.minRating != -2) {
        if (c.minRating == -1) { if (facts.rating != -1) return false; }
        else if (facts.rating < c.minRating) return false;
    }
    return true;
}
