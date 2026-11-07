#include "discovery.h"

namespace discovery {

namespace {

// Append `more` to `target`, skipping strings already present (order kept).
void unionAddresses(QStringList &target, const QStringList &more) {
    for (const QString &a : more) {
        if (!target.contains(a)) target.push_back(a);
    }
}

} // namespace

QVector<Record> merge(const QVector<Record> &existing,
                      const QVector<Record> &incoming,
                      const QDateTime &now) {
    QVector<Record> result = existing;
    QVector<bool> seen(result.size(), false);

    for (const Record &inc : incoming) {
        if (inc.key.isEmpty()) continue; // an unkeyed record is not mergeable
        int idx = -1;
        for (int i = 0; i < result.size(); ++i) {
            if (result.at(i).key == inc.key) { idx = i; break; }
        }
        if (idx < 0) {
            Record added = inc;
            added.lastSeen = now;
            result.push_back(added);
            seen.push_back(true);
        } else {
            // Refresh the existing record and union its addresses.
            Record &r = result[idx];
            unionAddresses(r.addresses, inc.addresses);
            r.lastSeen = now;
            if (inc.ttlMs > 0) r.ttlMs = inc.ttlMs; // refresh ttl if provided
            seen[idx] = true;
        }
    }

    // Drop existing records that were not seen this round and have expired.
    QVector<Record> kept;
    kept.reserve(result.size());
    for (int i = 0; i < result.size(); ++i) {
        const Record &r = result.at(i);
        if (!seen.at(i) && r.ttlMs > 0 &&
            now.msecsTo(r.lastSeen) < -r.ttlMs) {
            continue; // stale: not seen and past its ttl
        }
        kept.push_back(r);
    }
    return kept;
}

} // namespace discovery
