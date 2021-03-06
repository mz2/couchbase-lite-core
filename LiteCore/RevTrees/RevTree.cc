//
//  RevTree.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/13/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "RevTree.hh"
#include "RawRevTree.hh"
#include "Error.hh"
#include <algorithm>
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if DEBUG
#include <iostream>
#include <sstream>
#endif


namespace litecore {
    using namespace fleece;

    static bool compareRevs(const Rev *rev1, const Rev *rev2);

    RevTree::RevTree(slice raw_tree, sequence_t seq) {
        decode(raw_tree, seq);
    }

    RevTree::RevTree(const RevTree &other)
    :_insertedData(other._insertedData)
    ,_sorted(other._sorted)
    ,_changed(other._changed)
    ,_unknown(other._unknown)
    {
        // It's important to have _revs in the same order as other._revs.
        // That means we can't just copy other._revsStorage to _revsStorage;
        // we have to copy _revs in order:
        _revs.reserve(other._revs.size());
        for (const Rev *otherRev : other._revs) {
            _revsStorage.emplace_back(*otherRev);
            _revs.push_back(&_revsStorage.back());
        }
        // Fix up the newly copied Revs so they point to me (and my other Revs), not other:
        for (Rev *rev : _revs) {
            if (rev->parent)
                rev->parent = _revs[rev->parent->index()];
            rev->owner = this;
        }
    }

    void RevTree::decode(litecore::slice raw_tree, sequence_t seq) {
        _revsStorage = RawRevision::decodeTree(raw_tree, this, seq);
        initRevs();
    }

    void RevTree::initRevs() {
        _revs.resize(_revsStorage.size());
        auto i = _revs.begin();
        for (Rev &rev : _revsStorage) {
            *i = &rev;
            ++i;
        }
    }

    alloc_slice RevTree::encode() {
        sort();
        return RawRevision::encodeTree(_revs);
    }

#if DEBUG
    void Rev::dump(std::ostream& out) {
        out << "(" << sequence << ") " << (std::string)revID.expanded() << "  ";
        if (isLeaf())
            out << " leaf";
        if (isDeleted())
            out << " del";
        if (hasAttachments())
            out << " attachments";
        if (isNew())
            out << " (new)";
    }
#endif

#pragma mark - ACCESSORS:

    const Rev* RevTree::currentRevision() {
        Assert(!_unknown);
        sort();
        return _revs.size() == 0 ? nullptr : _revs[0];
    }

    const Rev* RevTree::get(unsigned index) const {
        Assert(!_unknown);
        Assert(index < _revs.size());
        return _revs[index];
    }

    const Rev* RevTree::get(revid revID) const {
        for (Rev *rev : _revs) {
            if (rev->revID == revID)
                return rev;
        }
        Assert(!_unknown);
        return nullptr;
    }

    const Rev* RevTree::getBySequence(sequence_t seq) const {
        for (Rev *rev : _revs) {
            if (rev->sequence == seq)
                return rev;
        }
        Assert(!_unknown);
        return nullptr;
    }

    bool RevTree::hasConflict() const {
        if (_revs.size() < 2) {
            Assert(!_unknown);
            return false;
        } else if (_sorted) {
            return _revs[1]->isActive();
        } else {
            unsigned nActive = 0;
            for (Rev *rev : _revs) {
                if (rev->isActive()) {
                    if (++nActive > 1)
                        return true;
                }
            }
            return false;
        }
    }

    unsigned Rev::index() const {
        auto &revs = owner->_revs;
        auto i = find(revs.begin(), revs.end(), this);
        Assert(i != revs.end());
        return (unsigned)(i - revs.begin());
    }

    const Rev* Rev::next() const {
        auto i = index() + 1;
        return i < owner->size() ? owner->get(i) : nullptr;
    }

    std::vector<const Rev*> Rev::history() const {
        std::vector<const Rev*> h;
        for (const Rev* rev = this; rev; rev = rev->parent)
            h.push_back(rev);
        return h;
    }

    bool RevTree::isBodyOfRevisionAvailable(const Rev* rev) const {
        return rev->_body.buf != nullptr; // VersionedDocument overrides this
    }

    alloc_slice RevTree::readBodyOfRevision(const Rev* rev) const {
        if (rev->_body.buf != nullptr)
            return alloc_slice(rev->_body);
        return alloc_slice(); // VersionedDocument overrides this
    }

    bool RevTree::confirmLeaf(Rev* testRev) {
        for (Rev *rev : _revs)
            if (rev->parent == testRev)
                return false;
        testRev->addFlag(Rev::kLeaf);
        return true;
    }
    

#pragma mark - INSERTION:

    // Lowest-level insert method. Does no sanity checking, always inserts.
    Rev* RevTree::_insert(revid unownedRevID,
                          slice body,
                          Rev *parentRev,
                          Rev::Flags revFlags)
    {
        static const auto kNewRevFlagsMask = (Rev::kDeleted | Rev::kHasAttachments
                                              | Rev::kKeepBody | Rev::kForeign);

        Assert(!_unknown);
        // Allocate copies of the revID and data so they'll stay around:
        _insertedData.emplace_back(unownedRevID);
        revid revID = revid(_insertedData.back());
        if (body.size > 0) {
            _insertedData.emplace_back(body);
            body = _insertedData.back();
        }

        _revsStorage.emplace_back();
        Rev *newRev = &_revsStorage.back();
        newRev->owner = this;
        newRev->revID = revID;
        newRev->_body = body;
        newRev->sequence = 0; // Sequence is unknown till record is saved
        newRev->flags = (Rev::Flags)(Rev::kLeaf | Rev::kNew | (revFlags & kNewRevFlagsMask));
        newRev->parent = parentRev;

        if (parentRev) {
            bool conflict = (!parentRev->isLeaf() || parentRev->isConflict());
            if (conflict)
                newRev->addFlag(Rev::kIsConflict);      // Creating or extending a branch
            parentRev->clearFlag(Rev::kLeaf);
            if (revFlags & Rev::kKeepBody) {
                // Only one rev in the main branch can have the keepBody flag
                for (auto ancestor = (Rev*)parentRev; ancestor; ancestor = (Rev*)ancestor->parent) {
                    if (conflict && !ancestor->isConflict())
                        break;
                    ancestor->clearFlag(Rev::kKeepBody);
                }
            }
        } else {
            // Root revision:
            if (!_revs.empty())
                newRev->addFlag(Rev::kIsConflict);      // Creating a 2nd root
        }

        _changed = true;
        if (!_revs.empty())
            _sorted = false;
        _revs.push_back(newRev);
        return newRev;
    }

    const Rev* RevTree::insert(revid revID, slice data, Rev::Flags revFlags,
                               const Rev* parent, bool allowConflict,
                               int &httpStatus)
    {
        // Make sure the given revID is valid:
        uint32_t newGen = revID.generation();
        if (newGen == 0) {
            httpStatus = 400;
            return nullptr;
        }

        if (get(revID)) {
            httpStatus = 200;
            return nullptr; // already exists
        }

        // Find the parent rev, if a parent ID is given:
        uint32_t parentGen;
        if (parent) {
            if (!allowConflict && !parent->isLeaf()) {
                httpStatus = 409;
                return nullptr;
            }
            parentGen = parent->revID.generation();
        } else {
            if (!allowConflict && _revs.size() > 0) {
                httpStatus = 409;
                return nullptr;
            }
            parentGen = 0;
        }

        // Enforce that generation number went up by 1 from the parent:
        if (newGen != parentGen + 1) {
            httpStatus = 400;
            return nullptr;
        }
        
        // Finally, insert:
        httpStatus = (revFlags & Rev::kDeleted) ? 200 : 201;
        return _insert(revID, data, (Rev*)parent, revFlags);
    }

    const Rev* RevTree::insert(revid revID, slice body, Rev::Flags revFlags,
                               revid parentRevID, bool allowConflict,
                               int &httpStatus)
    {
        const Rev* parent = nullptr;
        if (parentRevID.buf) {
            parent = get(parentRevID);
            if (!parent) {
                httpStatus = 404;
                return nullptr; // parent doesn't exist
            }
        }
        return insert(revID, body, revFlags, parent, allowConflict, httpStatus);
    }

    int RevTree::insertHistory(const std::vector<revidBuffer> history, slice data,
                               Rev::Flags revFlags) {
        Assert(history.size() > 0);
        // Find the common ancestor, if any. Along the way, preflight revision IDs:
        int i;
        unsigned lastGen = 0;
        Rev* parent = nullptr;
        size_t historyCount = history.size();
        for (i = 0; i < historyCount; i++) {
            unsigned gen = history[i].generation();
            if (lastGen > 0 && gen != lastGen - 1)
                return -1; // generation numbers not in sequence
            lastGen = gen;

            parent = (Rev*)get(history[i]);
            if (parent)
                break;
        }
        int commonAncestorIndex = i;

        if (i > 0) {
            // Insert all the new revisions in chronological order:
            auto ancestorFlags = (Rev::Flags)(revFlags & Rev::kForeign);
            while (--i > 0)
                parent = _insert(history[i], slice(), parent, ancestorFlags);
            _insert(history[0], data, parent, revFlags);
        }
        return commonAncestorIndex;
    }

#pragma mark - REMOVAL (prune / purge / compact):

    void RevTree::removeBody(const Rev* rev) {
        if (rev->flags & Rev::kKeepBody) {
            const_cast<Rev*>(rev)->removeBody();
            _changed = true;
        }
    }

    // Remove bodies of already-saved revs that are no longer leaves:
    void RevTree::removeNonLeafBodies() {
        for (Rev *rev : _revs) {
            if (rev->_body.size > 0 && !(rev->flags & (Rev::kLeaf | Rev::kNew | Rev::kKeepBody))) {
                rev->removeBody();
                _changed = true;
            }
        }
    }

    unsigned RevTree::prune(unsigned maxDepth) {
        Assert(maxDepth > 0);
        if (_revs.size() <= maxDepth)
            return 0;

        // First find all the leaves, and walk from each one down to its root:
        int numPruned = 0;
        for (auto &rev : _revs) {
            if (rev->isLeaf()) {
                // Starting from a leaf rev, trace its ancestry to find its depth:
                unsigned depth = 0;
                for (Rev* anc = rev; anc; anc = (Rev*)anc->parent) {
                    if (++depth > maxDepth) {
                        // Mark revs that are too far away:
                        anc->markForPurge();
                        numPruned++;
                    }
                }
            } else if (_sorted) {
                break;
            }
        }

        if (numPruned == 0)
            return 0;

        // Clear parent links that point to revisions being pruned:
        for (auto &rev : _revs)
            if (rev->parent && rev->parent->isMarkedForPurge())
                rev->parent = nullptr;
        compact();
        return numPruned;
    }

    int RevTree::purge(revid leafID) {
        int nPurged = 0;
        Rev* rev = (Rev*)get(leafID);
        if (!rev || !rev->isLeaf())
            return 0;
        do {
            nPurged++;
            rev->markForPurge();
            const Rev* parent = (Rev*)rev->parent;
            rev->parent = nullptr;                      // unlink from parent
            rev = (Rev*)parent;
        } while (rev && confirmLeaf(rev));
        compact();
        checkForResolvedConflict();
        return nPurged;
    }

    int RevTree::purgeAll() {
        int result = (int)_revs.size();
        _revs.resize(0);
        _changed = true;
        _sorted = true;
        return result;
    }

    void RevTree::compact() {
        // Slide the surviving revs down:
        auto dst = _revs.begin();
        for (auto rev = dst; rev != _revs.end(); rev++) {
            if (!(*rev)->isMarkedForPurge()) {
                if (dst != rev)
                    *dst = *rev;
                dst++;
            }
        }
        _revs.resize(dst - _revs.begin());
        _changed = true;
    }


#pragma mark - SORT / SAVE:

    // Sort comparison function for an array of Revisions. Higher priority comes _first_, so this
    // is a descending sort. The function returns true if rev1 is higher priority than rev2.
    static bool compareRevs(const Rev *rev1, const Rev *rev2) {
        // Leaf revs go before non-leaves.
        int delta = rev2->isLeaf() - rev1->isLeaf();
        if (delta)
            return delta < 0;
        // Live revs go before deletions.
        delta = rev1->isDeleted() - rev2->isDeleted();
        if (delta)
            return delta < 0;
        // Conflicting revs never go first.
        delta = rev1->isConflict() - rev2->isConflict();
        if (delta)
            return delta < 0;
        // Otherwise compare rev IDs, with higher rev ID going first:
        return rev2->revID < rev1->revID;
    }

    void RevTree::sort() {
        if (_sorted)
            return;
        std::sort(_revs.begin(), _revs.end(), &compareRevs);
        _sorted = true;
        checkForResolvedConflict();
    }

    // If there are no non-conflict leaves, remove the conflict marker from the 1st:
    void RevTree::checkForResolvedConflict() {
        if (_sorted && !_revs.empty() && _revs[0]->isConflict()) {
            for (auto rev = _revs[0]; rev; rev = (Rev*)rev->parent)
                rev->clearFlag(Rev::kIsConflict);
        }
    }

    void RevTree::saved(sequence_t newSequence) {
        for (Rev *rev : _revs) {
            rev->clearFlag(Rev::kNew);
            if (rev->sequence == 0)
                rev->sequence = newSequence;
        }
    }

#if DEBUG
    void RevTree::dump() {
        dump(std::cerr);
    }

    void RevTree::dump(std::ostream& out) {
        int i = 0;
        for (Rev *rev : _revs) {
            out << "\t" << (++i) << ": ";
            rev->dump(out);
            out << "\n";
        }
    }
#endif

}
