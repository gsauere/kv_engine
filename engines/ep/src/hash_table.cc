/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "hash_table.h"

#include "item.h"
#include "stats.h"
#include "stored_value_factories.h"

#include <folly/lang/Assume.h>
#include <phosphor/phosphor.h>
#include <platform/compress.h>

#include <logtags.h>
#include <cstring>

static const ssize_t prime_size_table[] = {
    3, 7, 13, 23, 47, 97, 193, 383, 769, 1531, 3079, 6143, 12289, 24571, 49157,
    98299, 196613, 393209, 786433, 1572869, 3145721, 6291449, 12582917,
    25165813, 50331653, 100663291, 201326611, 402653189, 805306357,
    1610612741, -1
};

/**
 * Define the increment factor for the ProbabilisticCounter being used for
 * the frequency counter.  The value is set such that it allows an 8-bit
 * ProbabilisticCounter to mimic a uint16 counter.
 *
 * The value was reached by running the following code using a variety of
 * incFactor values.
 *
 * ProbabilisticCounter<uint8_t> probabilisticCounter(incFactor);
 * uint64_t iterationCount{0};
 * uint8_t counter{0};
 *     while (counter != std::numeric_limits<uint8_t>::max()) {
 *         counter = probabilisticCounter.generateValue(counter);
 *         iterationCount++;
 *     }
 * std::cerr << "iterationCount=" <<  iterationCount << std::endl;
 *
 * To mimic a uint16 counter, iterationCount needs to be ~65000 (the maximum
 * value of a uint16_t is 65,536).  Through experimentation this was found to be
 * achieved with an incFactor of 0.012.
 */
static const double freqCounterIncFactor = 0.012;

std::string to_string(MutationStatus status) {
    switch (status) {
    case MutationStatus::NotFound:
        return "NotFound";
    case MutationStatus::InvalidCas:
        return "InvalidCas";
    case MutationStatus::WasClean:
        return "WasClean";
    case MutationStatus::WasDirty:
        return "WasDirty";
    case MutationStatus::IsLocked:
        return "IsLocked";
    case MutationStatus::NoMem:
        return "NoMem";
    case MutationStatus::NeedBgFetch:
        return "NeedBgFetch";
    case MutationStatus::IsPendingSyncWrite:
        return "IsPendingSyncWrite";
    }
    return "<invalid>(" + std::to_string(int(status)) + ")";
}

std::ostream& operator<<(std::ostream& os, const HashTable::Position& pos) {
    os << "{lock:" << pos.lock << " bucket:" << pos.hash_bucket << "/" << pos.ht_size << "}";
    return os;
}

HashTable::StoredValueProxy::StoredValueProxy(HashBucketLock&& hbl,
                                              StoredValue* sv,
                                              Statistics& stats)
    : lock(std::move(hbl)),
      value(sv),
      valueStats(stats),
      pre(valueStats.prologue(sv)) {
}

HashTable::StoredValueProxy::~StoredValueProxy() {
    valueStats.epilogue(pre, value);
}

HashTable::HashTable(EPStats& st,
                     std::unique_ptr<AbstractStoredValueFactory> svFactory,
                     size_t initialSize,
                     size_t locks)
    : initialSize(initialSize),
      size(initialSize),
      mutexes(locks),
      stats(st),
      valFact(std::move(svFactory)),
      visitors(0),
      valueStats(stats),
      numEjects(0),
      numResizes(0),
      maxDeletedRevSeqno(0),
      probabilisticCounter(freqCounterIncFactor) {
    values.resize(size);
    activeState = true;
}

HashTable::~HashTable() {
    // Use unlocked clear for the destructor, avoids lock inversions on VBucket
    // delete
    clear_UNLOCKED(true);
    // Wait for any outstanding visitors to finish.
    while (visitors > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

void HashTable::cleanupIfTemporaryItem(const HashBucketLock& hbl,
                                       StoredValue& v) {
    if (v.isTempDeletedItem() || v.isTempNonExistentItem()) {
        unlocked_del(hbl, v.getKey());
    }
}

void HashTable::clear(bool deactivate) {
    if (!deactivate) {
        // If not deactivating, assert we're already active.
        if (!isActive()) {
            throw std::logic_error("HashTable::clear: Cannot call on a "
                    "non-active object");
        }
    }
    MultiLockHolder mlh(mutexes);
    clear_UNLOCKED(deactivate);
}

void HashTable::clear_UNLOCKED(bool deactivate) {
    if (deactivate) {
        setActiveState(false);
    }
    size_t clearedMemSize = 0;
    size_t clearedValSize = 0;
    for (int i = 0; i < (int)size; i++) {
        while (values[i]) {
            // Take ownership of the StoredValue from the vector, update
            // statistics and release it.
            auto v = std::move(values[i]);
            clearedMemSize += v->size();
            clearedValSize += v->valuelen();
            values[i] = std::move(v->getNext());
        }
    }

    stats.coreLocal.get()->currentSize.fetch_sub(clearedMemSize -
                                                 clearedValSize);

    valueStats.reset();
}

static size_t distance(size_t a, size_t b) {
    return std::max(a, b) - std::min(a, b);
}

static size_t nearest(size_t n, size_t a, size_t b) {
    return (distance(n, a) < distance(b, n)) ? a : b;
}

static bool isCurrently(size_t size, ssize_t a, ssize_t b) {
    ssize_t current(static_cast<ssize_t>(size));
    return (current == a || current == b);
}

void HashTable::resize() {
    size_t ni = getNumInMemoryItems();
    int i(0);
    size_t new_size(0);

    // Figure out where in the prime table we are.
    ssize_t target(static_cast<ssize_t>(ni));
    for (i = 0; prime_size_table[i] > 0 && prime_size_table[i] < target; ++i) {
        // Just looking...
    }

    if (prime_size_table[i] == -1) {
        // We're at the end, take the biggest
        new_size = prime_size_table[i-1];
    } else if (prime_size_table[i] < static_cast<ssize_t>(initialSize)) {
        // Was going to be smaller than the initial size.
        new_size = initialSize;
    } else if (0 == i) {
        new_size = prime_size_table[i];
    }else if (isCurrently(size, prime_size_table[i-1], prime_size_table[i])) {
        // If one of the candidate sizes is the current size, maintain
        // the current size in order to remain stable.
        new_size = size;
    } else {
        // Somewhere in the middle, use the one we're closer to.
        new_size = nearest(ni, prime_size_table[i-1], prime_size_table[i]);
    }

    resize(new_size);
}

void HashTable::resize(size_t newSize) {
    if (!isActive()) {
        throw std::logic_error("HashTable::resize: Cannot call on a "
                "non-active object");
    }

    // Due to the way hashing works, we can't fit anything larger than
    // an int.
    if (newSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return;
    }

    // Don't resize to the same size, either.
    if (newSize == size) {
        return;
    }

    TRACE_EVENT2(
            "HashTable", "resize", "size", size.load(), "newSize", newSize);

    MultiLockHolder mlh(mutexes);
    if (visitors.load() > 0) {
        // Do not allow a resize while any visitors are actually
        // processing.  The next attempt will have to pick it up.  New
        // visitors cannot start doing meaningful work (we own all
        // locks at this point).
        return;
    }

    // Get a place for the new items.
    table_type newValues(newSize);

    stats.coreLocal.get()->memOverhead.fetch_sub(memorySize());
    ++numResizes;

    // Set the new size so all the hashy stuff works.
    size_t oldSize = size;
    size.store(newSize);

    // Move existing records into the new space.
    for (size_t i = 0; i < oldSize; i++) {
        while (values[i]) {
            // unlink the front element from the hash chain at values[i].
            auto v = std::move(values[i]);
            values[i] = std::move(v->getNext());

            // And re-link it into the correct place in newValues.
            int newBucket = getBucketForHash(v->getKey().hash());
            v->setNext(std::move(newValues[newBucket]));
            newValues[newBucket] = std::move(v);
        }
    }

    // Finally assign the new table to values.
    values = std::move(newValues);

    stats.coreLocal.get()->memOverhead.fetch_add(memorySize());
}

HashTable::FindInnerResult HashTable::findInner(const DocKey& key) {
    if (!isActive()) {
        throw std::logic_error(
                "HashTable::find: Cannot call on a "
                "non-active object");
    }
    HashBucketLock hbl = getLockedBucket(key);
    // Scan through all elements in the hash bucket chain looking for Committed
    // and Pending items with the same key.
    StoredValue* foundCmt = nullptr;
    StoredValue* foundPend = nullptr;
    for (StoredValue* v = values[hbl.getBucketNum()].get().get(); v;
         v = v->getNext().get().get()) {
        if (v->hasKey(key)) {
            if (v->isPending()) {
                Expects(!foundPend);
                foundPend = v;
            } else {
                Expects(!foundCmt);
                foundCmt = v;
            }
        }
    }

    return {std::move(hbl), foundCmt, foundPend};
}

std::unique_ptr<Item> HashTable::getRandomKey(long rnd) {
    /* Try to locate a partition */
    size_t start = rnd % size;
    size_t curr = start;
    std::unique_ptr<Item> ret;

    do {
        ret = getRandomKeyFromSlot(curr++);
        if (curr == size) {
            curr = 0;
        }
    } while (ret == NULL && curr != start);

    return ret;
}

MutationStatus HashTable::set(Item& val) {
    auto htRes = findForWrite(val.getKey());
    if (htRes.storedValue) {
        return unlocked_updateStoredValue(htRes.lock, *htRes.storedValue, val)
                .status;
    } else {
        unlocked_addNewStoredValue(htRes.lock, val);
        return MutationStatus::WasClean;
    }
}

HashTable::UpdateResult HashTable::unlocked_updateStoredValue(
        const HashBucketLock& hbl, StoredValue& v, const Item& itm) {
    if (!hbl.getHTLock()) {
        throw std::invalid_argument(
                "HashTable::unlocked_updateStoredValue: htLock "
                "not held");
    }

    if (!isActive()) {
        throw std::logic_error(
                "HashTable::unlocked_updateStoredValue: Cannot "
                "call on a non-active HT object");
    }

    switch (v.getCommitted()) {
    case CommittedState::Pending:
    case CommittedState::PreparedMaybeVisible:
        // Cannot update a SV if it's a Pending item.
        return {MutationStatus::IsPendingSyncWrite, nullptr};

    case CommittedState::CommittedViaMutation:
    case CommittedState::CommittedViaPrepare:
        // Logically /can/ update a non-Pending StoredValue with a Pending Item;
        // however internally this is implemented as a separate (new)
        // StoredValue object for the Pending item.
        if (itm.isPending()) {
            auto* sv = HashTable::unlocked_addNewStoredValue(hbl, itm);
            return {MutationStatus::WasClean, sv};
        }

        // item is not Pending; can directly replace the existing SV.
        MutationStatus status = v.isDirty() ? MutationStatus::WasDirty
                                            : MutationStatus::WasClean;

        const auto preProps = valueStats.prologue(&v);

        /* setValue() will mark v as undeleted if required */
        v.setValue(itm);
        updateFreqCounter(v);

        valueStats.epilogue(preProps, &v);

        return {status, &v};
    }
    folly::assume_unreachable();
}

StoredValue* HashTable::unlocked_addNewStoredValue(const HashBucketLock& hbl,
                                                   const Item& itm) {
    if (!hbl.getHTLock()) {
        throw std::invalid_argument(
                "HashTable::unlocked_addNewStoredValue: htLock "
                "not held");
    }

    if (!isActive()) {
        throw std::invalid_argument(
                "HashTable::unlocked_addNewStoredValue: Cannot "
                "call on a non-active HT object");
    }

    const auto emptyProperties = valueStats.prologue(nullptr);

    // Create a new StoredValue and link it into the head of the bucket chain.
    auto v = (*valFact)(itm, std::move(values[hbl.getBucketNum()]));

    valueStats.epilogue(emptyProperties, v.get().get());

    values[hbl.getBucketNum()] = std::move(v);
    return values[hbl.getBucketNum()].get().get();
}

HashTable::Statistics::StoredValueProperties::StoredValueProperties(
        const StoredValue* sv) {
    // If no previous StoredValue exists; return default constructed object.
    if (sv == nullptr) {
        return;
    }

    // Record all properties of the stored value which statistics require.
    isValid = true;
    size = sv->size();
    metaDataSize = sv->metaDataSize();
    datatype = sv->getDatatype();
    uncompressedSize = sv->uncompressedSize();
    isResident = sv->isResident();
    isDeleted = sv->isDeleted();
    isTempItem = sv->isTempItem();
    isSystemItem = sv->getKey().getCollectionID().isSystem();
    isPreparedSyncWrite = sv->isPending();
}

HashTable::Statistics::StoredValueProperties HashTable::Statistics::prologue(
        const StoredValue* v) const {
    return StoredValueProperties(v);
}

void HashTable::Statistics::epilogue(StoredValueProperties pre,
                                     const StoredValue* v) {
    // After performing updates to sv; compare with the previous properties and
    // update all statistics for all properties which have changed.

    const auto post = StoredValueProperties(v);

    // Update size, metadataSize & uncompressed size if pre/post differ.
    if (pre.size != post.size) {
        cacheSize.fetch_add(post.size - pre.size);
        memSize.fetch_add(post.size - pre.size);
    }
    if (pre.metaDataSize != post.metaDataSize) {
        metaDataMemory.fetch_add(post.metaDataSize - pre.metaDataSize);
        epStats.coreLocal.get()->currentSize.fetch_add(post.metaDataSize -
                                                       pre.metaDataSize);
    }
    if (pre.uncompressedSize != post.uncompressedSize) {
        uncompressedMemSize.fetch_add(post.uncompressedSize -
                                      pre.uncompressedSize);
    }

    // Determine if valid, non resident; and update numNonResidentItems if
    // differ.
    bool preNonResident = pre.isValid && (!pre.isResident && !pre.isDeleted &&
                                          !pre.isTempItem);
    bool postNonResident =
            post.isValid &&
            (!post.isResident && !post.isDeleted && !post.isTempItem);
    if (preNonResident != postNonResident) {
        numNonResidentItems.fetch_add(postNonResident - preNonResident);
    }

    if (pre.isTempItem != post.isTempItem) {
        numTempItems.fetch_add(post.isTempItem - pre.isTempItem);
    }

    // nonItems only considers valid; non-temporary items:
    bool preNonTemp = pre.isValid && !pre.isTempItem;
    bool postNonTemp = post.isValid && !post.isTempItem;
    if (preNonTemp != postNonTemp) {
        numItems.fetch_add(postNonTemp - preNonTemp);
    }

    if (pre.isSystemItem != post.isSystemItem) {
        numSystemItems.fetch_add(post.isSystemItem - pre.isSystemItem);
    }

    // numPreparedItems counts valid, prepared (not yet committed) items.
    const bool prePrepared = pre.isValid && pre.isPreparedSyncWrite;
    const bool postPrepared = post.isValid && post.isPreparedSyncWrite;
    if (prePrepared != postPrepared) {
        numPreparedSyncWrites.fetch_add(postPrepared - prePrepared);
    }

    // Don't include system items in the deleted count, numSystemItems will
    // count both types (a marked deleted system event still has purpose)
    // Don't include prepared items in the deleted count - they haven't (yet)
    // been deleted.
    const bool preDeleted =
            pre.isDeleted && !pre.isSystemItem && !pre.isPreparedSyncWrite;
    const bool postDeleted =
            post.isDeleted && !post.isSystemItem && !post.isPreparedSyncWrite;
    if (preDeleted != postDeleted) {
        numDeletedItems.fetch_add(postDeleted - preDeleted);
    }

    // Update datatypes. These are only tracked for non-temp, non-deleted,
    // committed items.
    if (preNonTemp && !pre.isDeleted && !pre.isPreparedSyncWrite) {
        --datatypeCounts[pre.datatype];
    }
    if (postNonTemp && !post.isDeleted && !post.isPreparedSyncWrite) {
        ++datatypeCounts[post.datatype];
    }
}

void HashTable::Statistics::reset() {
    datatypeCounts.fill(0);
    numItems.store(0);
    numTempItems.store(0);
    numNonResidentItems.store(0);
    memSize.store(0);
    cacheSize.store(0);
    uncompressedMemSize.store(0);
}

std::pair<StoredValue*, StoredValue::UniquePtr>
HashTable::unlocked_replaceByCopy(const HashBucketLock& hbl,
                                  const StoredValue& vToCopy) {
    if (!hbl.getHTLock()) {
        throw std::invalid_argument(
                "HashTable::unlocked_replaceByCopy: htLock "
                "not held");
    }

    if (!isActive()) {
        throw std::invalid_argument(
                "HashTable::unlocked_replaceByCopy: Cannot "
                "call on a non-active HT object");
    }

    /* Release (remove) the StoredValue from the hash table */
    auto releasedSv = unlocked_release(hbl, vToCopy.getKey());

    /* Copy the StoredValue and link it into the head of the bucket chain. */
    auto newSv = valFact->copyStoredValue(
            vToCopy, std::move(values[hbl.getBucketNum()]));

    // Adding a new item into the HashTable; update stats.
    const auto emptyProperties = valueStats.prologue(nullptr);
    valueStats.epilogue(emptyProperties, newSv.get().get());

    values[hbl.getBucketNum()] = std::move(newSv);
    return {values[hbl.getBucketNum()].get().get(), std::move(releasedSv)};
}

HashTable::DeleteResult HashTable::unlocked_softDelete(
        const HashBucketLock& hbl,
        StoredValue& v,
        bool onlyMarkDeleted,
        DeleteSource delSource) {
    switch (v.getCommitted()) {
    case CommittedState::Pending:
    case CommittedState::PreparedMaybeVisible:
        // Cannot update a SV if it's a Pending item.
        return {DeletionStatus::IsPendingSyncWrite, nullptr};

    case CommittedState::CommittedViaMutation:
    case CommittedState::CommittedViaPrepare:
        const auto preProps = valueStats.prologue(&v);

        if (onlyMarkDeleted) {
            v.markDeleted(delSource);
        } else {
            v.del(delSource);
        }

        valueStats.epilogue(preProps, &v);
        return {DeletionStatus::Success, &v};
    }
    folly::assume_unreachable();
}

StoredValue::UniquePtr HashTable::unlocked_createSyncDeletePrepare(
        const HashTable::HashBucketLock& hbl,
        const StoredValue& v,
        DeleteSource delSource) {
    auto pendingDel = valFact->copyStoredValue(v, nullptr /*next chain ptr*/);
    pendingDel->setCommitted(CommittedState::Pending);
    pendingDel->del(delSource);
    return pendingDel;
}

HashTable::FindROResult HashTable::findForRead(const DocKey& key,
                                               TrackReference trackReference,
                                               WantsDeleted wantsDeleted) {
    auto result = findInner(key);

    /// Reading normally uses the Committed StoredValue - however if a
    /// pendingSV is found we must check if it's marked as MaybeVisible -
    /// which will block reading.
    if (result.pendingSV && result.pendingSV->isPreparedMaybeVisible()) {
        // Return the pending one as an indication the caller cannot read it.
        return {result.pendingSV, std::move(result.lock)};
    }
    auto* sv = result.committedSV;

    if (!sv) {
        // No item found - return null.
        return {nullptr, std::move(result.lock)};
    }

    if (result.committedSV->isDeleted()) {
        // Deleted items should only be returned if caller asked for them,
        // and we don't update ref-counts for them.
        return {(wantsDeleted == WantsDeleted::Yes) ? sv : nullptr,
                std::move(result.lock)};
    }

    // Found a non-deleted item. Now check if we should update ref-count.
    if (trackReference == TrackReference::Yes) {
        updateFreqCounter(*sv);

        // @todo remove the referenced call when eviction algorithm is
        // updated to use the frequency counter value.
        sv->referenced();
    }

    return {sv, std::move(result.lock)};
}

HashTable::FindResult HashTable::findForWrite(const DocKey& key,
                                              WantsDeleted wantsDeleted) {
    auto result = findInner(key);

    /// Writing using the Pending StoredValue (if found), else committed.
    auto* sv = result.pendingSV ? result.pendingSV : result.committedSV;

    if (!sv) {
        // No item found - return null.
        return {nullptr, std::move(result.lock)};
    }

    // Early return if we found a prepare. We should always return prepares
    // regardless of whether or not they are deleted or the caller has asked for
    // deleted SVs. For example, consider searching for a SyncDelete, we should
    // always return the deleted prepare.
    if (result.pendingSV) {
        return {sv, std::move(result.lock)};
    }

    if (sv->isDeleted() && wantsDeleted == WantsDeleted::No) {
        // Deleted items should only be returned if caller asked for them -
        // otherwise return null.
        return {nullptr, std::move(result.lock)};
    }
    return {sv, std::move(result.lock)};
}

HashTable::StoredValueProxy HashTable::findForWrite(StoredValueProxy::RetSVPTag,
                                                    const DocKey& key,
                                                    WantsDeleted wantsDeleted) {
    auto result = findForWrite(key, wantsDeleted);
    return StoredValueProxy(
            std::move(result.lock), result.storedValue, valueStats);
}

HashTable::FindCommitResult HashTable::findForCommit(const DocKey& key) {
    auto result = findInner(key);

    StoredValueProxy prepare{
            std::move(result.lock), result.pendingSV, valueStats};
    return {std::move(prepare), result.committedSV};
}

HashTable::FindResult HashTable::findOnlyCommitted(const DocKey& key) {
    auto result = findInner(key);
    return {result.committedSV, std::move(result.lock)};
}

HashTable::FindResult HashTable::findOnlyPrepared(const DocKey& key) {
    auto result = findInner(key);
    return {result.pendingSV, std::move(result.lock)};
}

void HashTable::unlocked_del(const HashBucketLock& hbl, const DocKey& key) {
    unlocked_release(hbl, key).reset();
}

void HashTable::unlocked_del(const HashBucketLock& hbl, StoredValue* value) {
    unlocked_release(hbl, value).reset();
}

StoredValue::UniquePtr HashTable::unlocked_release(
        const HashBucketLock& hbl, const DocKey& key) {
    // Remove the first (should only be one) StoredValue with the given key.
    auto releasePredicate = [&key](const StoredValue* v) {
        return v->hasKey(key);
    };
    return unlocked_release_inner(hbl, releasePredicate);
}

StoredValue::UniquePtr HashTable::unlocked_release(
        const HashBucketLock& hbl,
        StoredValue* valueToRelease) {
    // Remove the first (should only be one) StoredValue matching the given
    // pointer
    auto releasePredicate = [&valueToRelease](const StoredValue* v) {
        return v == valueToRelease;
    };
    return unlocked_release_inner(hbl, releasePredicate);
}

template <typename Pred>
StoredValue::UniquePtr HashTable::unlocked_release_inner(
        const HashBucketLock& hbl, Pred& releasePredicate) {
    if (!hbl.getHTLock()) {
        throw std::invalid_argument(
                "HashTable::unlocked_release_base: htLock not held");
    }

    if (!isActive()) {
        throw std::logic_error(
                "HashTable::unlocked_release_base: Cannot call on a "
                "non-active object");
    }

    // Remove the first (should only be one) StoredValue that matches the given
    // releasePredicate
    auto released = hashChainRemoveFirst(
            values[hbl.getBucketNum()], releasePredicate);

    if (!released) {
        /* We shouldn't reach here, we must delete the StoredValue in the
           HashTable */
        throw std::logic_error(
                "HashTable::unlocked_release_base: StoredValue to be released "
                "not found in HashTable; possibly HashTable leak");
    }

    // Update statistics for the item which is now gone.
    const auto preProps = valueStats.prologue(released.get().get());
    valueStats.epilogue(preProps, nullptr);

    return released;
}

MutationStatus HashTable::insertFromWarmup(const Item& itm,
                                           bool eject,
                                           bool keyMetaDataOnly,
                                           EvictionPolicy evictionPolicy) {
    auto htRes = findInner(itm.getKey());
    auto* v = (itm.isCommitted() ? htRes.committedSV : htRes.pendingSV);
    auto& hbl = htRes.lock;

    if (v == NULL) {
        v = unlocked_addNewStoredValue(hbl, itm);

        // TODO: Would be faster if we just skipped creating the value in the
        // first place instead of adding it to the Item and then discarding it
        // in markNotResident.
        if (keyMetaDataOnly) {
            const auto preProps = valueStats.prologue(v);
            v->markNotResident();
            valueStats.epilogue(preProps, v);
        }
        v->setNewCacheItem(false);
    } else {
        if (keyMetaDataOnly) {
            // We don't have a better error code ;)
            return MutationStatus::InvalidCas;
        }

        // Existing item found. This should only occur if:
        // a) The existing item is temporary (i.e. result of a front-end
        //    thread attempting to read and triggered a bgFetch); or
        // b) The existing item is non-temporary and was loaded as the result of
        //    a previous BGfetch (and has the same CAS).
        //
        // Verify that the CAS isn't changed
        if (v->getCas() != itm.getCas()) {
            if (v->getCas() == 0) {
                v->setCas(itm.getCas());
                v->setFlags(itm.getFlags());
                v->setExptime(itm.getExptime());
                v->setRevSeqno(itm.getRevSeqno());
            } else {
                return MutationStatus::InvalidCas;
            }
        }

        // CAS is equal - exact same item. Update the SV if it's not already
        // resident.
        if (!v->isResident()) {
            Expects(unlocked_restoreValue(hbl.getHTLock(), itm, *v));
        }
    }

    v->markClean();

    if (eject && !keyMetaDataOnly) {
        unlocked_ejectItem(hbl, v, evictionPolicy);
    }

    return MutationStatus::NotFound;
}

bool HashTable::reallocateStoredValue(StoredValue&& sv) {
    // Search the chain and reallocate
    for (StoredValue::UniquePtr* curr =
                 &values[getBucketForHash(sv.getKey().hash())];
         curr->get().get();
         curr = &curr->get()->getNext()) {
        if (&sv == curr->get().get()) {
            auto newSv = valFact->copyStoredValue(sv, std::move(sv.getNext()));
            curr->swap(newSv);
            return true;
        }
    }
    return false;
}

void HashTable::dump() const {
    std::cerr << *this << std::endl;
}

void HashTable::storeCompressedBuffer(cb::const_char_buffer buf,
                                      StoredValue& v) {
    const auto preProps = valueStats.prologue(&v);

    v.storeCompressedBuffer(buf);

    valueStats.epilogue(preProps, &v);
}

void HashTable::visit(HashTableVisitor& visitor) {
    HashTable::Position ht_pos;
    while (ht_pos != endPosition()) {
        ht_pos = pauseResumeVisit(visitor, ht_pos);
    }
}

void HashTable::visitDepth(HashTableDepthVisitor &visitor) {
    if (valueStats.getNumItems() == 0 || !isActive()) {
        return;
    }
    size_t visited = 0;
    // Acquire one (any) of the mutexes before incrementing {visitors}, this
    // prevents any race between this visitor and the HashTable resizer.
    // See comments in pauseResumeVisit() for further details.
    std::unique_lock<std::mutex> lh(mutexes[0]);
    VisitorTracker vt(&visitors);
    lh.unlock();

    for (int l = 0; l < static_cast<int>(mutexes.size()); l++) {
        for (int i = l; i < static_cast<int>(size); i+= mutexes.size()) {
            // (re)acquire mutex on each HashBucket, to minimise any impact
            // on front-end threads.
            LockHolder lh(mutexes[l]);

            size_t depth = 0;
            StoredValue* p = values[i].get().get();
            if (p) {
                // TODO: Perf: This check seems costly - do we think it's still
                // worth keeping?
                auto hashbucket = getBucketForHash(p->getKey().hash());
                if (i != hashbucket) {
                    throw std::logic_error("HashTable::visit: inconsistency "
                            "between StoredValue's calculated hashbucket "
                            "(which is " + std::to_string(hashbucket) +
                            ") and bucket it is located in (which is " +
                            std::to_string(i) + ")");
                }
            }
            size_t mem(0);
            while (p) {
                depth++;
                mem += p->size();
                p = p->getNext().get().get();
            }
            visitor.visit(i, depth, mem);
            ++visited;
        }
    }
}

HashTable::Position HashTable::pauseResumeVisit(HashTableVisitor& visitor,
                                                Position& start_pos) {
    if ((valueStats.getNumItems() + valueStats.getNumTempItems()) == 0 ||
        !isActive()) {
        // Nothing to visit
        return endPosition();
    }

    bool paused = false;

    // To attempt to minimize the impact the visitor has on normal frontend
    // operations, we deliberately acquire (and release) the mutex between
    // each hash_bucket - see `lh` in the inner for() loop below. This means we
    // hold a given mutex for a large number of short durations, instead of just
    // one single, long duration.
    // *However*, there is a potential race with this approach - the {size} of
    // the HashTable may be changed (by the Resizer task) between us first
    // reading it to calculate the starting hash_bucket, and then reading it
    // inside the inner for() loop. To prevent this race, we explicitly acquire
    // (any) mutex, increment {visitors} and then release the mutex. This
    //avoids the race as if visitors >0 then Resizer will not attempt to resize.
    std::unique_lock<std::mutex> lh(mutexes[0]);
    VisitorTracker vt(&visitors);
    lh.unlock();

    // Start from the requested lock number if in range.
    size_t lock = (start_pos.lock < mutexes.size()) ? start_pos.lock : 0;
    size_t hash_bucket = 0;

    for (; isActive() && !paused && lock < mutexes.size(); lock++) {

        // If the bucket position is *this* lock, then start from the
        // recorded bucket (as long as we haven't resized).
        hash_bucket = lock;
        if (start_pos.lock == lock &&
            start_pos.ht_size == size &&
            start_pos.hash_bucket < size) {
            hash_bucket = start_pos.hash_bucket;
        }

        // Iterate across all values in the hash buckets owned by this lock.
        // Note: we don't record how far into the bucket linked-list we
        // pause at; so any restart will begin from the next bucket.
        for (; !paused && hash_bucket < size; hash_bucket += mutexes.size()) {
            visitor.setUpHashBucketVisit();

            // HashBucketLock scope. If a visitor needs additional locking
            // around the HashBucket visit then we need to release it before
            // tearDownHashBucketVisit() is called.
            {
                HashBucketLock lh(hash_bucket, mutexes[lock]);

                StoredValue* v = values[hash_bucket].get().get();
                while (!paused && v) {
                    StoredValue* tmp = v->getNext().get().get();
                    paused = !visitor.visit(lh, *v);
                    v = tmp;
                }
            }

            visitor.tearDownHashBucketVisit();
        }

        // If the visitor paused us before we visited all hash buckets owned
        // by this lock, we don't want to skip the remaining hash buckets, so
        // stop the outer for loop from advancing to the next lock.
        if (paused && hash_bucket < size) {
            break;
        }

        // Finished all buckets owned by this lock. Set hash_bucket to 'size'
        // to give a consistent marker for "end of lock".
        hash_bucket = size;
    }

    // Return the *next* location that should be visited.
    return HashTable::Position(size, lock, hash_bucket);
}

HashTable::Position HashTable::endPosition() const  {
    return HashTable::Position(size, mutexes.size(), size);
}

bool HashTable::unlocked_ejectItem(const HashTable::HashBucketLock&,
                                   StoredValue*& vptr,
                                   EvictionPolicy policy) {
    if (vptr == nullptr) {
        throw std::invalid_argument("HashTable::unlocked_ejectItem: "
                "Unable to delete NULL StoredValue");
    }

    if (!vptr->eligibleForEviction(policy)) {
        ++stats.numFailedEjects;
        return false;
    }

    const auto preProps = valueStats.prologue(vptr);

    switch (policy) {
    case EvictionPolicy::Value: {
        vptr->ejectValue();
        ++stats.numValueEjects;
        valueStats.epilogue(preProps, vptr);
        break;
    }
    case EvictionPolicy::Full: {
        // Remove the item from the hash table.
        int bucket_num = getBucketForHash(vptr->getKey().hash());
        auto removed = hashChainRemoveFirst(
                values[bucket_num],
                [vptr](const StoredValue* v) { return v == vptr; });

        if (removed->isResident()) {
            ++stats.numValueEjects;
        }
        valueStats.epilogue(preProps, nullptr);

        updateMaxDeletedRevSeqno(vptr->getRevSeqno());
        break;
    }
    }

    ++numEjects;
    return true;
}

std::unique_ptr<Item> HashTable::getRandomKeyFromSlot(int slot) {
    auto lh = getLockedBucket(slot);
    for (StoredValue* v = values[slot].get().get(); v;
            v = v->getNext().get().get()) {
        if (!v->isTempItem() && !v->isDeleted() && v->isResident() &&
            v->isCommitted()) {
            return v->toItem(Vbid(0));
        }
    }

    return nullptr;
}

bool HashTable::unlocked_restoreValue(
        const std::unique_lock<std::mutex>& htLock,
        const Item& itm,
        StoredValue& v) {
    if (!htLock || !isActive() || v.isResident()) {
        return false;
    }

    const auto preProps = valueStats.prologue(&v);

    if (v.isTempItem()) {
        /* set it back to false as we created a temp item by setting it to true
           when bg fetch is scheduled (full eviction mode). */
        v.setNewCacheItem(false);
    }

    v.restoreValue(itm);

    valueStats.epilogue(preProps, &v);

    return true;
}

void HashTable::unlocked_restoreMeta(const std::unique_lock<std::mutex>& htLock,
                                     const Item& itm,
                                     StoredValue& v) {
    if (!htLock) {
        throw std::invalid_argument(
                "HashTable::unlocked_restoreMeta: htLock "
                "not held");
    }

    if (!isActive()) {
        throw std::logic_error(
                "HashTable::unlocked_restoreMeta: Cannot "
                "call on a non-active HT object");
    }

    const auto preProps = valueStats.prologue(&v);

    v.restoreMeta(itm);

    valueStats.epilogue(preProps, &v);
}

uint8_t HashTable::generateFreqValue(uint8_t counter) {
    return probabilisticCounter.generateValue(counter);
}

void HashTable::updateFreqCounter(StoredValue& v) {
    // Attempt to increment the storedValue frequency counter
    // value.  Because a probabilistic counter is used the new
    // value will either be the same or an increment of the
    // current value.
    auto updatedFreqCounterValue = generateFreqValue(v.getFreqCounterValue());
    v.setFreqCounterValue(updatedFreqCounterValue);

    if (updatedFreqCounterValue == std::numeric_limits<uint8_t>::max()) {
        // Invoke the registered callback function which
        // wakeups the ItemFreqDecayer task.
        frequencyCounterSaturated();
    }
}

std::ostream& operator<<(std::ostream& os, const HashTable& ht) {
    os << "HashTable[" << &ht << "] with"
       << " numItems:" << ht.getNumItems()
       << " numInMemory:" << ht.getNumInMemoryItems()
       << " numDeleted:" << ht.getNumDeletedItems()
       << " numNonResident:" << ht.getNumInMemoryNonResItems()
       << " numTemp:" << ht.getNumTempItems()
       << " numSystemItems:" << ht.getNumSystemItems()
       << " numPreparedSW:" << ht.getNumPreparedSyncWrites()
       << " values: " << std::endl;
    for (const auto& chain : ht.values) {
        if (chain) {
            for (StoredValue* sv = chain.get().get(); sv != nullptr;
                 sv = sv->getNext().get().get()) {
                os << "    " << *sv << std::endl;
            }
        }
    }
    return os;
}
